#![forbid(
    clippy::inline_asm_x86_intel_syntax,
    clippy::missing_const_for_fn,
    clippy::missing_const_for_thread_local,
    missing_unsafe_on_extern,
    missing_abi
)]
#![deny(
    clippy::alloc_instead_of_core,
    clippy::std_instead_of_core,
    clippy::std_instead_of_alloc,
    reason = "clippy false-lints nightly-only exports"
)]
#![deny(missing_docs)]
#![doc = include_str!("../README.md")]

extern crate alloc;

pub mod target;

pub mod peephole;

pub mod address;

pub mod manage;

pub mod offset;

pub mod prelude {
    //! The prelude of the `catalejo` crate.

    pub use crate::{
        address::{ViAddr, ViRange},
        offset::{AddressSpace, Field, Offset, Retrievable, Retrieve, Unassociated},
        peephole::{Foreign, InitializeWord, Lift, Peephole, Window},
        target::Target,
    };
}
