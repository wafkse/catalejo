//! The Foreign-Function-Interface module for `catalejo-fault`.
//!
//! This is used to bind the with the *C* side of the crate, which implements signal guarding.

use core::mem;
use core::{marker, mem::MaybeUninit, ptr};

use catalejo_memory::primitive::PrimitiveUnion;

use crate::behavior::Faultable;

pub mod binding {
    #![allow(
        nonstandard_style,
        missing_docs,
        reason = "bindgen-generated bindings have largely non-standard style and missing documentation"
    )]
    //! Bare automatically-generated bindings to the C-based subsystem.

    // NOTE: Include the `bindgen`-generated bindings for our own crate.
    include!(concat!(env!("OUT_DIR"), "/catalejo-binding.rs"));
}

pub mod lower {
    //! Low-level and plumbing structures and functions towards the Foreign-Function-Interface boundary.

    use catalejo_memory::primitive::{Primitive, PrimitiveUnion};

    use crate::ffi::binding;

    /// Perform a bare-bones read via the C-implemented shims.
    ///
    /// # Safety
    ///
    /// This has the same safety constraints as an individual `binding::catalejo_read_uN` operation.
    #[inline]
    pub unsafe fn read(
        target_source: *const PrimitiveUnion,
        target_value: *mut PrimitiveUnion,
        target_type: Primitive,
    ) -> binding::catalejo_faultable_outcome_t {
        macro_rules! implement {
            ($target_type:ident) => {
                tokel::stream!(
                    [< binding::catalejo_read _ $target_type >]:concatenate (target_source.cast::<$target_type>(), target_value.cast::<$target_type>())
                )
            };
        }

        // SAFETY: The safety concerns of the foreign call have been satisfied by the caller.
        unsafe {
            match target_type {
                Primitive::U8 => implement!(u8),
                Primitive::U16 => implement!(u16),
                Primitive::U32 => implement!(u32),
                Primitive::U64 => implement!(u64),
            }
        }
    }

    /// Perform a bare-bones write via the C-implemented shims.
    ///
    /// # Safety
    ///
    /// This has the same safety constraints as an individual `binding::catalejo_write_uN` operation.
    #[inline]
    pub unsafe fn write(
        target_value: *mut PrimitiveUnion,
        target_source: *const PrimitiveUnion,
        target_type: Primitive,
    ) -> binding::catalejo_faultable_outcome_t {
        macro_rules! implement {
            ($target_type:ident) => {
                tokel::stream!(
                    [< binding::catalejo_write _ $target_type >]:concatenate (target_value.cast::<$target_type>(), target_source.cast::<$target_type>())
                )
            };
        }

        // SAFETY: The safety concerns of the foreign call have been satisfied by the caller.
        unsafe {
            match target_type {
                Primitive::U8 => implement!(u8),
                Primitive::U16 => implement!(u16),
                Primitive::U32 => implement!(u32),
                Primitive::U64 => implement!(u64),
            }
        }
    }
}

/// A token that guarantees that the *catalejo* C-based subsystem has been initialized properly.
#[repr(transparent)]
#[derive(Debug, Copy, Clone)]
pub struct Subsystem(marker::PhantomData<Self>);

impl Subsystem {
    /// Initialize the C-based subsystem if it has not been initialized, providing a token to prove it.
    ///
    /// # Safety
    ///
    /// This is a low-level initialization function for the C-based subsystem of the `catalejo-fault` crate.
    ///
    /// Particularly, this initializes the signal-catching mechanisms to be able to handle synchronous hardware
    /// exceptions properly.
    ///
    /// To guarantee the safety of all posterior `catalejo` operations, the following must be guaranteed.
    ///
    /// * No other thread may register a signal handler for `SIGBUS` and `SIGSEGV` simultaneously while this function is executed.
    ///
    /// * Any posterior signal handler must chain the behavior of the existing `catalejo`-installed signal handlers, preserving exact behavior.
    #[inline]
    pub unsafe fn initialize() -> Option<Subsystem> {
        let target_outcome =
            // SAFETY: The caller asserts that this is not inherently racy with the kernel.
            unsafe { binding::catalejo_fault_initialize() };

        match target_outcome {
            binding::CATALEJO_OUTCOME_SUCCESS => Some(Self(marker::PhantomData::<Self>)),
            binding::CATALEJO_OUTCOME_ERROR => None::<Self>,
            // NOTE: Any other value is impossible.
            _ => unreachable!(),
        }
    }
}

/// Perform a primitive-level fault-tolerant read of a memory address.
///
/// This function attempts to read from the target address using a memory-coherent hardware fetch.
/// If the underlying physical page is unmapped, the hardware exception is caught by the subsystem
/// and the function gracefully returns `None`.
///
/// # Safety
///
/// * The address names a `Faultable`-typed value, so every possible bit-pattern is valid.
///   If the page is physically mapped but logically freed, this safely returns garbage bytes.
///
/// * The address is non-null and accessible under *exposed* provenance. This covers memory outside
///   the Rust Abstract Machine, such as an MMU-adjudicated foreign mapping disjoint from the
///   abstract machine's stack, heap, and statics, as well as memory whose provenance was previously
///   exposed. The access is carried out by hand-written assembly, never as an abstract-machine load.
///   Its liveness is adjudicated by the hardware MMU, and a fault is caught and reported instead of
///   being undefined behavior.
///
/// * The address is properly aligned to maintain machine-word and snapshot coherence. Misaligned
///   reads crossing cache-lines or pages lose hardware atomicity.
///
/// * The address targets standard memory. It must not point to memory-mapped
///   I/O hardware registers where a speculative read could trigger a device-level side-effect.
///
/// * No foreign library has hijacked the synchronous POSIX signal handlers without implementing
///   perfect chaining since the subsystem token was issued.
#[inline]
pub unsafe fn read<F>(_: Subsystem, target_address: *mut F) -> Option<F>
where
    F: Faultable,
{
    let mut target_value =
        // SAFETY: The `PrimitiveUnion` type is composed of integer primitives, so this is safe.
        unsafe { MaybeUninit::<PrimitiveUnion>::zeroed().assume_init() };

    // SAFETY:
    // * The address names a `Faultable`-typed value, so every bit-pattern is valid.
    //
    // * The pointer carries exposed provenance over outside-abstract-machine memory and is
    //   accessed by assembly; a resulting fault is caught by the subsystem, never undefined behavior.
    //
    // * The address is properly aligned to maintain machine-word coherence.
    let target_outcome = unsafe {
        lower::read(
            target_address.cast::<PrimitiveUnion>(),
            ptr::from_mut(&mut target_value),
            F::PRIMITIVE,
        )
    };

    match target_outcome {
        binding::CATALEJO_OUTCOME_SUCCESS => Some(
            // SAFETY: The `F` implements `Faultable`, which guarantees the soundness of this transmute.
            //
            // The transmute is mixed in size, but it is valid as the respective union houses up to the biggest primitive type.
            unsafe { mem::transmute_copy::<PrimitiveUnion, F>(&target_value) },
        ),
        binding::CATALEJO_OUTCOME_ERROR => None,
        // NOTE: No other such result may be possible.
        _ => unreachable!(),
    }
}

/// Perform a primitive-level fault-tolerant write to a memory address.
///
/// This function attempts to write to the target address using a memory-coherent hardware access.
/// If the underlying physical page is unmapped, the hardware exception is caught by the subsystem
/// and the function gracefully returns `false`. If the write is indeed successful, it returns `true`.
///
/// # Safety
///
/// This function has the same safety constraints as the [`read`] function, however, there are some additions:
///
/// * The address must also name memory that is permitted to be written; otherwise the store faults
///   and is reported as `false`.
/// * The store must not race, in the Rust sense, with another thread of the *current* process for
///   the same address. A concurrent mutation by a foreign process is not such a race: nothing
///   aliases the address as a reference, and the store is an indivisible hardware access.
#[inline]
pub unsafe fn write<F>(_: Subsystem, target_address: *mut F, target_value: F) -> bool
where
    F: Faultable,
{
    let target_source = &raw const target_value;

    // SAFETY:
    // * The address names a `Faultable`-typed value, so every bit-pattern is valid.
    //
    // * The pointer carries exposed provenance over outside-abstract-machine memory and is
    //   accessed by assembly; a resulting fault is caught by the subsystem, never undefined behavior.
    //
    // * The address is properly aligned to maintain machine-word coherence.
    let target_outcome = unsafe {
        lower::write(
            target_address.cast::<PrimitiveUnion>(),
            target_source.cast_mut().cast::<PrimitiveUnion>(),
            F::PRIMITIVE,
        )
    };

    match target_outcome {
        binding::CATALEJO_OUTCOME_SUCCESS => true,
        binding::CATALEJO_OUTCOME_ERROR => false,
        // NOTE: No other such result may be possible.
        _ => unreachable!(),
    }
}
