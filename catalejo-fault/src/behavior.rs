//! Traits that describe the behavior of fault-tolerant types.

use core::mem::MaybeUninit;

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
