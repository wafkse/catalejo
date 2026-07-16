//! Traits that describe the behavior of fault-tolerant types.

use core::{mem, mem::MaybeUninit, ptr};

use catalejo_memory::{behavior::Unassociated, primitive::Primitive};

/// A trait that describes a faulting primitive access to memory.
///
/// This provides the guarantees already by the [`Unassociated`] trait (as this is a supertrait of such), on top of:
///
/// * That the fault-resilient reads performed are of both *machine-word* and *snapshot* coherence.
///
/// # Safety
///
/// * The implementor type must have the same in-memory representation as the [`PrimitiveUnion`] type, and be sound to copy-transmuted to and from it.
///
/// [`PrimitiveUnion`]: catalejo_memory::primitive::PrimitiveUnion
pub unsafe trait Faultable: Unassociated {
    /// The primitive used for a read or a write.
    const PRIMITIVE: Primitive;
}

// SAFETY: If `T` implements `Faultable`, `MaybeUninit` does too.
unsafe impl<F> Faultable for MaybeUninit<F>
where
    F: Faultable,
{
    const PRIMITIVE: Primitive = F::PRIMITIVE;
}

/// Implement the [`Faultable`] trait for a primitive integer type.
macro_rules! faultable {
    (
        $(
            #[$target_meta:meta]
        )*

        for $target_type:ident
    ) => {
        tokel::stream!(
            $(
                #[$target_meta]
            )*
            // SAFETY: A primitive integer type is always a sound `Faultable` implementor.
            unsafe impl Faultable for $target_type {
                const PRIMITIVE: Primitive = const {
                    Primitive::appropriate::<$target_type>().expect([< "error: no proper machine-coherent primitive found for `" $target_type "`" >]:to_string:concatenate)
                };
            }
        );
    };
}

faultable!(for u8);
faultable!(for u16);
faultable!(for u32);
faultable!(for u64);

faultable!(for i8);
faultable!(for i16);
faultable!(for i32);
faultable!(for i64);

faultable!(for f32);
faultable!(for f64);

faultable!(for usize);
faultable!(for isize);

/// Determine whether two [`Faultable`] values have equal bit representations.
///
/// This compares the complete primitive representation rather than using [`PartialEq`].
/// Floating point values are therefore compared by their encoded bits and equal NaN payloads
/// compare as equal.
#[inline]
pub const fn equal<F>(target_left: F, target_right: F) -> bool
where
    F: Faultable,
{
    let mut left_value: u64 = u64::MIN;
    let mut right_value: u64 = u64::MIN;

    // SAFETY
    //
    // `Faultable` guarantees that `F` has one of the supported primitive representations.
    // Both destinations are initialized and each copy writes exactly the represented width.
    unsafe {
        ptr::copy_nonoverlapping(
            (&raw const target_left).cast::<u8>(),
            (&raw mut left_value).cast::<u8>(),
            mem::size_of::<F>(),
        );

        ptr::copy_nonoverlapping(
            (&raw const target_right).cast::<u8>(),
            (&raw mut right_value).cast::<u8>(),
            mem::size_of::<F>(),
        );
    }

    left_value == right_value
}

#[cfg(test)]
mod test {
    use super::equal;

    #[test]
    fn monitor_snapshot_comparison_uses_value_bits() {
        assert!(equal(0xA5_u8, 0xA5_u8));
        assert!(equal(0xA55A_u16, 0xA55A_u16));
        assert!(equal(0xA55A_5AA5_u32, 0xA55A_5AA5_u32));
        assert!(equal(0xA55A_5AA5_DEAD_BEEF_u64, 0xA55A_5AA5_DEAD_BEEF_u64,));
        assert!(!equal(1_u64, 2_u64));

        let target_nan = f64::from_bits(0x7FF8_0000_0000_0001);

        assert!(equal(target_nan, target_nan));
        assert!(!equal(target_nan, f64::from_bits(0x7FF8_0000_0000_0002),));
    }
}
