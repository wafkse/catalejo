#![cfg_attr(all(not(test), not(feature = "stealth-mode")), no_std)]
#![forbid(
    clippy::alloc_instead_of_core,
    clippy::std_instead_of_core,
    clippy::std_instead_of_alloc,
    clippy::inline_asm_x86_intel_syntax,
    clippy::missing_const_for_fn,
    clippy::missing_const_for_thread_local,
    missing_unsafe_on_extern,
    missing_abi
)]
#![deny(
    missing_docs,
    reason = "this is non-forbid due to bindgen-generated code being undocumented"
)]
#![doc = include_str!("../README.md")]

#[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
compile_error!("not supported: a non-x86 architecture is not supported");
#[cfg(not(target_arch = "x86_64"))]
compile_error!("not supported: a 64-bit x86 architecture is required");

#[cfg(not(target_os = "linux"))]
compile_error!("not supported: no non-linux kernel is supported");

pub mod ffi;

pub mod maybe;

pub mod behavior;
