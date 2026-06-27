#![cfg_attr(not(test), no_std)]
#![forbid(
    clippy::alloc_instead_of_core,
    clippy::std_instead_of_core,
    clippy::std_instead_of_alloc,
    clippy::inline_asm_x86_intel_syntax,
    clippy::missing_const_for_fn,
    clippy::missing_const_for_thread_local,
    missing_unsafe_on_extern,
    missing_abi,
    missing_docs
)]
#![doc = include_str!("../README.md")]

pub mod primitive;

pub mod behavior;

pub mod prelude {
    //! The prelude module to the `catalejo-memory` crate.

    pub use crate::primitive::Primitive;

    pub use crate::behavior::Unassociated;
}
