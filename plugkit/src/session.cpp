#include "session.hpp"
#include "dissector_thread.hpp"
#include "dissector_thread_pool.hpp"
#include "filter_thread.hpp"
#include "filter_thread_pool.hpp"
#include "listener_thread_pool.hpp"
#include "listener_status.hpp"
#include "listener.hpp"
#include "frame_store.hpp"
#include "layer.hpp"
#include "pcap.hpp"
#include "private/frame.hpp"
#include "stream_dissector_thread_pool.hpp"
#include "uvloop_logger.hpp"
#include "variant.hpp"
#include <atomic>
#include <unordered_map>
#include <uv.h>

namespace plugkit {

struct Session::Config {
  std::string networkInterface;
  bool promiscuous = false;
  int snaplen = 2048;
  std::string bpf;
  std::unordered_map<int, minins> linkLayers;
  std::vector<DissectorFactoryConstPtr> dissectors;
  std::vector<StreamDissectorFactoryConstPtr> streamDissectors;
  std::unordered_map<std::string, ListenerFactoryConstPtr> listenerFactories;
  Variant options;
};

class Session::Private {
public:
  enum UpdateType {
    UPDATE_STATUS = 1,
    UPDATE_FILTER = 2,
    UPDATE_LISTENER = 4,
    UPDATE_FRAME = 8,
  };

public:
  uint32_t getSeq();
  void updateStatus();
  void notifyStatus(UpdateType type);

public:
  std::atomic<uint32_t> index;
  std::atomic<int> updates;
  std::shared_ptr<UvLoopLogger> logger;
  std::unique_ptr<DissectorThreadPool> dissectorPool;
  std::unique_ptr<StreamDissectorThreadPool> streamDissectorPool;
  std::unordered_map<std::string, ListenerFactoryConstPtr> listenerFactories;
  std::unordered_map<std::string, std::unique_ptr<FilterThreadPool>> filters;
  std::unordered_map<std::string, std::unique_ptr<ListenerThreadPool>>
      listeners;
  std::unordered_map<std::string, ListenerStatusConstPtr> listenerStatuses;
  std::unordered_map<int, minins> linkLayers;
  std::shared_ptr<FrameStore> frameStore;
  std::unique_ptr<Pcap> pcap;
  StatusCallback statusCallback;
  FilterCallback filterCallback;
  FrameCallback frameCallback;
  ListenerCallback listenerCallback;
  LoggerCallback loggerCallback;

  std::unique_ptr<Status> status;
  std::unique_ptr<FilterStatusMap> filterStatus;
  std::unique_ptr<FrameStatus> frameStatus;

  Variant options;
  uv_async_t async;
  int id;
};

uint32_t Session::Private::getSeq() {
  return index.fetch_add(1u, std::memory_order_relaxed);
}

void Session::Private::updateStatus() {
  int flags =
      std::atomic_fetch_and_explicit(&updates, 0, std::memory_order_relaxed);
  if (flags & Private::UPDATE_STATUS) {
    Status status;
    status.capture = pcap->running();
    statusCallback(status);
  }
  if (flags & Private::UPDATE_FILTER) {
    FilterStatusMap status;
    for (const auto &pair : filters) {
      FilterStatus filter;
      filter.frames = pair.second->size();
      status[pair.first] = filter;
    }
    filterCallback(status);
  }
  if (flags & Private::UPDATE_LISTENER) {
    ListenerRevStatusMap map;
    for (const auto &pair : listenerStatuses) {
      ListenerRevStatus status;
      status.revision = pair.second->revision();
      map[pair.first] = status;
    }
    listenerCallback(map);
  }
  if (flags & Private::UPDATE_FRAME) {
    FrameStatus status;
    status.frames = frameStore->dissectedSize();
    frameCallback(status);
  }
}

void Session::Private::notifyStatus(UpdateType type) {
  std::atomic_fetch_or_explicit(&updates, static_cast<int>(type),
                                std::memory_order_relaxed);
  uv_async_send(&async);
}

Session::Session(const Config &config) : d(new Private()) {
  std::atomic_init(&d->index, 1u);
  std::atomic_init(&d->updates, 0);

  d->async.data = d;
  uv_async_init(uv_default_loop(), &d->async, [](uv_async_t *handle) {
    Session::Private *d = static_cast<Session::Private *>(handle->data);
    d->updateStatus();
  });

  static int id = 0;
  d->id = id++;

  d->options = config.options;

  d->logger = std::make_shared<UvLoopLogger>(
      uv_default_loop(), [this](Logger::MessagePtr &&msg) {
        if (d->loggerCallback)
          d->loggerCallback(std::move(msg));
      });
  d->pcap = Pcap::create();
  d->pcap->setNetworkInterface(config.networkInterface);
  d->pcap->setPromiscuous(config.promiscuous);
  d->pcap->setSnaplen(config.snaplen);
  d->pcap->setBpf(config.bpf);
  d->pcap->setLogger(d->logger);

  d->frameStore = std::make_shared<FrameStore>(
      [this]() { d->notifyStatus(Private::UPDATE_FRAME); });

  d->dissectorPool.reset(new DissectorThreadPool(
      config.options, [this](Frame **begin, size_t size) {
        d->frameStore->insert(begin, size);
      }));
  d->dissectorPool->setLogger(d->logger);

  d->streamDissectorPool.reset(new StreamDissectorThreadPool(
      config.options, d->frameStore,
      [this](uint32_t maxSeq) { d->frameStore->update(maxSeq); }));
  d->streamDissectorPool->setLogger(d->logger);

  d->pcap->setCallback([this](Frame *frame) {
    frame->d->setIndex(d->getSeq());
    d->dissectorPool->push(&frame, 1);
  });

  d->linkLayers = config.linkLayers;
  for (const auto &pair : config.linkLayers) {
    d->pcap->registerLinkLayer(pair.first, pair.second);
  }
  for (const auto &factory : config.dissectors) {
    d->dissectorPool->registerDissector(factory);
  }
  for (const auto &factory : config.streamDissectors) {
    d->streamDissectorPool->registerDissector(factory);
  }
  d->listenerFactories = config.listenerFactories;
  d->streamDissectorPool->start();
  d->dissectorPool->start();
}

Session::~Session() {
  stopPcap();
  d->updateStatus();
  d->frameStore->close();
  d->filters.clear();
  d->listeners.clear();
  d->pcap.reset();
  d->dissectorPool.reset();
  d->streamDissectorPool.reset();
  d->logger.reset();
  uv_run(uv_default_loop(), UV_RUN_ONCE);

  uv_close(reinterpret_cast<uv_handle_t *>(&d->async), [](uv_handle_t *handle) {
    Session::Private *d = static_cast<Session::Private *>(handle->data);
    d->frameStore.reset();
    delete d;
  });
}

bool Session::startPcap() {
  if (d->pcap->start()) {
    d->notifyStatus(Private::UPDATE_STATUS);
    return true;
  }
  return false;
}

bool Session::stopPcap() {
  if (d->pcap->stop()) {
    d->notifyStatus(Private::UPDATE_STATUS);
    return true;
  }
  return false;
}

std::string Session::networkInterface() const {
  return d->pcap->networkInterface();
}

bool Session::promiscuous() const { return d->pcap->promiscuous(); }

int Session::snaplen() const { return d->pcap->snaplen(); }

int Session::id() const { return d->id; }

Variant Session::options() const { return d->options; }

void Session::setDisplayFilter(const std::string &name,
                               const std::string &body) {

  auto filter = d->filters.find(name);
  if (body.empty()) {
    if (filter != d->filters.end()) {
      d->filters.erase(filter);
    }
  } else {
    auto pool = std::unique_ptr<FilterThreadPool>(
        new FilterThreadPool(body, d->frameStore, [this]() {
          d->notifyStatus(Private::UPDATE_FILTER);
        }));
    pool->setLogger(d->logger);
    pool->start();
    d->filters[name] = std::move(pool);
  }
  d->notifyStatus(Private::UPDATE_FILTER);
}

void Session::setListener(const std::string &id, const std::string &name,
                          const Variant &args) {
  auto listener = d->listeners.find(name);
  if (id.empty()) {
    if (listener != d->listeners.end()) {
      d->listeners.erase(listener);
    }
    d->listenerStatuses.erase(name);
  } else {
    auto status = std::make_shared<ListenerStatus>();
    d->listenerStatuses[name] = status;
    auto factory = d->listenerFactories.find(id);
    if (factory != d->listenerFactories.end()) {
      auto pool = std::unique_ptr<ListenerThreadPool>(new ListenerThreadPool(
          factory->second, status, args, d->frameStore,
          [this]() { d->notifyStatus(Private::UPDATE_LISTENER); }));
      pool->setLogger(d->logger);
      pool->start();
      d->listeners[name] = std::move(pool);
    }
  }
}

std::vector<uint32_t> Session::getFilteredFrames(const std::string &name,
                                                 uint32_t offset,
                                                 uint32_t length) const {
  auto filter = d->filters.find(name);
  if (filter != d->filters.end()) {
    return filter->second->get(offset, length);
  }
  return std::vector<uint32_t>();
}

std::vector<const FrameView *> Session::getFrames(uint32_t offset,
                                                  uint32_t length) const {
  return d->frameStore->get(offset, length);
}

ListenerStatusConstPtr
Session::getListenerStatus(const std::string &name) const {
  auto it = d->listenerStatuses.find(name);
  if (it != d->listenerStatuses.end()) {
    return it->second;
  }
  return ListenerStatusConstPtr();
}

void Session::analyze(const std::vector<RawFrame> &rawFrames) {
  std::vector<Frame *> frames;
  for (const RawFrame &raw : rawFrames) {
    Layer *rootLayer;
    const auto &linkLayer = d->linkLayers.find(raw.link);
    if (linkLayer != d->linkLayers.end()) {
      rootLayer = new Layer(linkLayer->second);
    } else {
      rootLayer = new Layer(MNS("?"));
    }
    rootLayer->setPayload(raw.payload);

    Frame *frame = Frame::Private::create();
    frame->d->setSourceId(raw.sourceId);
    frame->d->setTimestamp(raw.timestamp);
    frame->d->setLength((raw.length < raw.payload.length())
                            ? raw.payload.length()
                            : raw.length);
    frame->d->setRootLayer(rootLayer);
    frame->d->setIndex(d->getSeq());
    rootLayer->setFrame(frame);
    frames.push_back(frame);
  }
  d->dissectorPool->push(&frames[0], frames.size());
}

void Session::setStatusCallback(const StatusCallback &callback) {
  d->statusCallback = callback;
}

void Session::setFilterCallback(const FilterCallback &callback) {
  d->filterCallback = callback;
}

void Session::setFrameCallback(const FrameCallback &callback) {
  d->frameCallback = callback;
}

void Session::setListenerCallback(const ListenerCallback &callback) {
  d->listenerCallback = callback;
}

void Session::setLoggerCallback(const LoggerCallback &callback) {
  d->loggerCallback = callback;
}

SessionFactory::SessionFactory() : d(new Session::Config()) {}

SessionFactory::~SessionFactory() {}

SessionPtr SessionFactory::create() const {
  return std::make_shared<Session>(*d);
}

void SessionFactory::setNetworkInterface(const std::string &id) {
  d->networkInterface = id;
}

std::string SessionFactory::networkInterface() const {
  return d->networkInterface;
}
void SessionFactory::setPromiscuous(bool promisc) { d->promiscuous = promisc; }

bool SessionFactory::promiscuous() const { return d->promiscuous; }

void SessionFactory::setSnaplen(int len) { d->snaplen = len; }

int SessionFactory::snaplen() const { return d->snaplen; }

void SessionFactory::setBpf(const std::string &filter) { d->bpf = filter; }

std::string SessionFactory::bpf() const { return d->bpf; }

void SessionFactory::setOptions(const Variant &options) {
  d->options = options;
}

Variant SessionFactory::options() const { return d->options; }

void SessionFactory::registerLinkLayer(int link, const minins &ns) {
  d->linkLayers[link] = ns;
}

void SessionFactory::registerDissector(
    const DissectorFactoryConstPtr &factory) {
  d->dissectors.push_back(factory);
}

void SessionFactory::registerStreamDissector(
    const StreamDissectorFactoryConstPtr &factory) {
  d->streamDissectors.push_back(factory);
}

void SessionFactory::registerListener(const std::string &id,
                                      const ListenerFactoryConstPtr &factory) {
  d->listenerFactories[id] = factory;
}
}
