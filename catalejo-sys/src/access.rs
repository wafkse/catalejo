//! Raw fault-protected memory access through the initialized exception image.

use catalejo_memory::primitive::{Primitive, PrimitiveUnion};

use crate::{exception::Image, ffi::binding};

/// Perform a bare-bones primitive read via the C-implemented shims.
///
/// # Safety
///
/// This has the same safety constraints as an individual `binding::catalejo_read_uN` operation,
/// where `N` is the size of the primitive read. `image` must be registered for the current address
/// space through a live Mirilla session before a fault can occur.
#[inline]
pub unsafe fn read(
    image: &Image,
    target_source: *const PrimitiveUnion,
    target_value: *mut PrimitiveUnion,
    target_type: Primitive,
) -> binding::catalejo_outcome_t {
    let target_runtime = image.runtime();

    macro_rules! implement {
        ($target_type:ident) => {
            tokel::stream!(
                [< binding::catalejo_read _ $target_type >]:concatenate (target_runtime, target_source.cast::<$target_type>(), target_value.cast::<$target_type>())
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

/// Perform a bare-bones primitive write via the C-implemented shims.
///
/// # Safety
///
/// This has the same safety constraints as an individual `binding::catalejo_write_uN` operation,
/// where `N` is the size of the primitive written. `image` must be registered for the current
/// address space through a live Mirilla session before a fault can occur.
#[inline]
pub unsafe fn write(
    image: &Image,
    target_value: *mut PrimitiveUnion,
    target_source: *const PrimitiveUnion,
    target_type: Primitive,
) -> binding::catalejo_outcome_t {
    let target_runtime = image.runtime();

    macro_rules! implement {
        ($target_type:ident) => {
            tokel::stream!(
                [< binding::catalejo_write _ $target_type >]:concatenate (target_runtime, target_value.cast::<$target_type>(), target_source.cast::<$target_type>())
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

/// Perform a fault-protected byte copy.
///
/// # Safety
///
/// Exactly one side must be the fault-adjudicated range. The other side must be valid for the
/// complete byte count. The ranges must not overlap. The image must be registered for the current
/// address space through a live Mirilla session before a fault can occur.
#[inline]
pub unsafe fn copy(
    image: &Image,
    target: *mut u8,
    source: *const u8,
    count: usize,
) -> Result<(), usize> {
    let target_runtime = image.runtime();

    // SAFETY:
    // The caller supplies the complete protected-copy contract and the image proof.
    let outcome = unsafe { binding::catalejo_copy(target_runtime, target, source, count) };

    match outcome.outcome_status {
        binding::CATALEJO_OUTCOME_SUCCESS => Ok(()),
        binding::CATALEJO_OUTCOME_ERROR => Err(outcome.byte_count),
        #[cfg(not(feature = "stealth-mode"))]
        _ => unreachable!(),

        #[cfg(feature = "stealth-mode")]
        _ => std::process::abort(),
    }
}
