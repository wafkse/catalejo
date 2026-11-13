//! Raw fault-protected memory access through the inherited fault backend.

use catalejo_memory::primitive::{Primitive, PrimitiveUnion};

use crate::{exception::backend::Backend, ffi::binding};

/// Read a primitive with fault recovery.
///
/// # Safety
///
/// For soundness purposes, the following must be satisfied:
///
/// * The source must be ordinary memory suitable for fault-protected access,
///   aligned to the selected primitive width.
/// * The destination must be writable for that width and must not overlap the
///   source. It must remain valid for the duration of the call.
/// * The recovery capability must belong to the current address space.
#[inline]
pub unsafe fn read(
    _fault_backend: &Backend,
    target_source: *const PrimitiveUnion,
    target_value: *mut PrimitiveUnion,
    target_type: Primitive,
) -> binding::catalejo_outcome_t {
    // Dispatch to the fixed-width C shim selected by the primitive type.
    macro_rules! implement {
        ($target_type:ident) => {
            tokel::stream!(
                [< binding::catalejo_read _ $target_type >]:concatenate (target_source.cast::<$target_type>(), target_value.cast::<$target_type>())
            )
        };
    }

    // SAFETY:
    // The safety concerns of the foreign call have been satisfied by the caller.
    unsafe {
        match target_type {
            Primitive::U8 => implement!(u8),
            Primitive::U16 => implement!(u16),
            Primitive::U32 => implement!(u32),
            Primitive::U64 => implement!(u64),
        }
    }
}

/// Write a primitive with fault recovery.
///
/// # Safety
///
/// For soundness purposes, the following must be satisfied:
///
/// * The destination must be ordinary memory suitable for fault-protected
///   stores, aligned to the selected primitive width.
/// * The source must contain initialized bytes readable for that width and must
///   not overlap the destination. It must remain valid during the call.
/// * The recovery capability must belong to the current address space.
#[inline]
pub unsafe fn write(
    _fault_backend: &Backend,
    target_value: *mut PrimitiveUnion,
    target_source: *const PrimitiveUnion,
    target_type: Primitive,
) -> binding::catalejo_outcome_t {
    // Dispatch to the fixed-width C shim selected by the primitive type.
    macro_rules! implement {
        ($target_type:ident) => {
            tokel::stream!(
                [< binding::catalejo_write _ $target_type >]:concatenate (target_value.cast::<$target_type>(), target_source.cast::<$target_type>())
            )
        };
    }

    // SAFETY:
    // The safety concerns of the foreign call have been satisfied by the caller.
    unsafe {
        match target_type {
            Primitive::U8 => implement!(u8),
            Primitive::U16 => implement!(u16),
            Primitive::U32 => implement!(u32),
            Primitive::U64 => implement!(u64),
        }
    }
}

/// Copy bytes with recovery from faults in the protected range.
///
/// Success means all requested bytes were copied. Failure reports the number
/// left uncopied, leaving an initialized prefix in the destination.
///
/// # Safety
///
/// For soundness purposes, the following must be satisfied:
///
/// * Exactly one range must be designated for fault-protected access to ordinary
///   memory. The other range must remain valid for the requested byte count.
/// * Source bytes must be initialized when read, and destination bytes must be
///   writable. The ranges must not overlap.
/// * The recovery capability must belong to the current address space.
#[inline]
pub unsafe fn copy(
    _fault_backend: &Backend,
    target: *mut u8,
    source: *const u8,
    count: usize,
) -> Result<(), usize> {
    // SAFETY:
    // The caller supplies the complete protected-copy contract and an initialized backend.
    let outcome = unsafe { binding::catalejo_copy(target, source, count) };

    match outcome.outcome_status {
        binding::CATALEJO_OUTCOME_SUCCESS => Ok(()),
        binding::CATALEJO_OUTCOME_ERROR => Err(outcome.byte_count),
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(feature = "stealth-mode")]
        _ => std::process::abort(),
    }
}
