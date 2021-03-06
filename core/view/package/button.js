import m from 'mithril'
export default class ButtonBoxView {
  view (vnode) {
    const { pkg, install } = vnode.attrs
    if (pkg.archive) {
      return m('span', { class: 'button-box' }, [
        m('input', {
          type: 'button',
          value: 'Install',
          onclick: () => {
            install(pkg)
          },
        })
      ])
    }
    const remote =
      deplug.registry.packages.find((rem) => rem.id === pkg.id)
    return m('span', { class: 'button-box' }, [
      m('input', {
        type: 'button',
        value: pkg.disabled
          ? 'Enable'
          : 'Disable',
        onclick: () => {
          if (pkg.disabled) {
            deplug.packages.enable(pkg.id)
          } else {
            deplug.packages.disable(pkg.id)
          }
        },
      }),
      m('input', {
        type: 'button',
        value: 'Reinstall',
        style: {
          display: remote
            ? 'block'
            : 'none',
        },
        onclick: () => {
          install(remote)
        },
      }),
      m('input', {
        type: 'button',
        value: 'Uninstall',
        style: {
          display: pkg.builtin
            ? 'none'
            : 'block',
        },
        onclick: () => {
          deplug.packages.uninstall(pkg.id)
        },
      })
    ])
  }
}
