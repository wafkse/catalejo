#![cfg_attr(all(not(test), not(feature = "stealth-mode")), no_std)]
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

pub mod ffi;

pub mod id;
