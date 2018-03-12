//! Layer values.
//!
//! Type Layer represents a layer of a protocol stack.

use super::token::Token;
use super::attr::Attr;
use super::payload::Payload;
use super::context::Context;
use super::symbol;
use super::range::Range;
use std::mem;
use std::slice;

extern crate libc;

pub const MAX_WORKER: u8 = 16;

#[derive(Debug)]
#[repr(C)]
pub struct Layer {
    id: Token,
    data: u32,
    parent: *mut Layer,
    range: (u32, u32)
}

#[derive(Debug)]
pub enum Confidence {
    Error = 0,
    Possible = 1,
    Probable = 2,
    Exact = 3,
}

impl Layer {
    pub fn id(&self) -> Token {
        self.id
    }

    pub fn attr(&self, id: Token) -> Option<&Attr> {
        unsafe { symbol::Layer_attr.unwrap()(self, id).as_ref() }
    }

    pub fn payloads(&self) -> Box<Iterator<Item = &Payload>> {
        unsafe {
            let mut size: libc::size_t = 0;
            let ptr = symbol::Layer_payloads.unwrap()(self, &mut size);
            let s = slice::from_raw_parts(ptr, size);
            Box::new(s.iter().map(|elem| &**elem))
        }
    }

    pub fn add_layer(&mut self, ctx: &mut Context, id: Token) -> &mut Layer {
        unsafe { &mut *symbol::Layer_addLayer.unwrap()(self, ctx, id) }
    }

    pub fn add_attr(&mut self, ctx: &mut Context, id: Token) -> &mut Attr {
        unsafe { &mut *symbol::Layer_addAttr.unwrap()(self, ctx, id) }
    }

    pub fn add_attr_alias(&mut self, ctx: &mut Context, alias: Token, target: Token) {
        unsafe { symbol::Layer_addAttrAlias.unwrap()(self, ctx, alias, target) }
    }

    pub fn add_payload(&mut self, ctx: &mut Context) -> &mut Payload {
        unsafe { &mut *symbol::Layer_addPayload.unwrap()(self, ctx) }
    }

    pub fn add_error(&mut self, ctx: &mut Context, id: Token, msg: &str) {
        unsafe { symbol::Layer_addError.unwrap()(self, ctx, id, msg.as_ptr() as *const i8, msg.len()) }
    }

    pub fn add_tag(&mut self, ctx: &mut Context, id: Token) {
        unsafe {
            symbol::Layer_addTag.unwrap()(self, ctx, id);
        }
    }

    pub fn parent(&self) -> Option<&Layer> {
        if self.is_root() || self.parent.is_null() {
            None
        } else {
            unsafe { Some(&*self.parent) }
        }
    }

    pub fn parent_mut(&mut self) -> Option<&mut Layer> {
        if self.is_root() || self.parent.is_null() {
            None
        } else {
            unsafe { Some(&mut *self.parent) }
        }
    }

    pub fn range(&self) -> Range {
        Range {
            start: self.range.0,
            end: self.range.1,
        }
    }

    pub fn set_range(&mut self, range: &Range) {
        self.range = (range.start, range.end)
    }

    pub fn confidence(&self) -> Confidence {
        unsafe { mem::transmute(((self.data >> 4) & 0b11) as u8) }
    }

    pub fn set_confidence(&mut self, conf: Confidence) {
        self.data = (self.data & !0b11_0000) | ((conf as u32) << 4)
    }

    pub fn worker(&self) -> u8 {
        (self.data & 0b1111) as u8
    }

    pub fn set_worker(&mut self, worker: u8) {
        self.data = (self.data & !0b1111) | (worker % MAX_WORKER) as u32
    }

    fn is_root(&self) -> bool {
        ((self.data >> 6) & 0b1) != 0
    }
}
