//! Foreign pointer representation.

use core::{fmt, marker};

use catalejo_fault::behavior::Faultable;
use catalejo_memory::{
    behavior::{Immortal, Unassociated},
    prelude::Primitive,
};

use num_traits::Num;

mod detail {
    /// A seal supertrait for the [`Address`] trait.
    pub trait Sealed {}
}

/// A marker trait for address-representing faultable primitives.
pub trait Address: Num + Faultable + detail::Sealed {}

impl Address for u32 {}
impl Address for u64 {}

impl detail::Sealed for u32 {}
impl detail::Sealed for u64 {}

/// A pointee-tagged generic pointer presumed to reside in foreign memory.
#[derive(PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct Pointer<T, A>(A, marker::PhantomData<fn() -> T>)
where
    T: Immortal,
    A: Address;

impl<T, A> Pointer<T, A>
where
    T: Immortal,
    A: Address,
{
    /// Construct a type-tagged foreign pointer for a width `A`.
    #[inline]
    pub const fn new(target_address: A) -> Self {
        let target_type = marker::PhantomData;

        Self(target_address, target_type)
    }

    /// Determine the encoded foreign address.
    #[inline]
    pub const fn address(self) -> A {
        let Self(target_address, ..) = self;

        target_address
    }

    /// Determine whether the pointer is null.
    #[inline]
    pub fn null(self) -> bool {
        let Self(target_address, ..) = self;

        target_address.is_zero()
    }

    /// Determine whether the pointer is non-null.
    #[inline]
    pub fn nonnull(self) -> bool {
        !Self::null(self)
    }
}

// SAFETY: `Pointer` is a transparent wrapper over `A`, which implements `Unassociated`.
unsafe impl<T, A> Unassociated for Pointer<T, A>
where
    T: Immortal,
    A: Address,
{
}

// SAFETY: `Pointer` is a transparent wrapper over `A`, which implements `Faultable`.
unsafe impl<T, A> Faultable for Pointer<T, A>
where
    T: Immortal,
    A: Address,
{
    const PRIMITIVE: Primitive = <A as Faultable>::PRIMITIVE;
}

impl<T, A> Clone for Pointer<T, A>
where
    T: Immortal,
    A: Address,
{
    fn clone(&self) -> Self {
        *self
    }
}

impl<T, A> Copy for Pointer<T, A>
where
    T: Immortal,
    A: Address,
{
}

impl<T: fmt::Debug, A: fmt::Debug> fmt::Debug for Pointer<T, A>
where
    T: Immortal,
    A: Address,
{
    fn fmt(&self, target_formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let Self(target_address, ..) = self;

        write!(target_formatter, "#{target_address:p}")
    }
}

/// A type-level alias to the respective 32-bit pointer type.
pub type Pointer32<T> = Pointer<T, u32>;

/// A type-level alias to the respective 64-bit pointer type.
pub type Pointer64<T> = Pointer<T, u64>;

/// Common capability of typed foreign pointers with a fixed encoded address width.
///
/// Implementations preserve their pointee type in the concrete pointer identity. The associated
/// address type remains the encoded foreign width until a caller explicitly converts it into a
/// process-wide address representation.
pub trait ForeignPointer: Unassociated + Copy + Eq + fmt::Debug {
    /// Encoded foreign address width carried by this pointer family.
    type Address: Copy + Eq + Ord + fmt::Debug + Into<u64>;

    /// Construct a typed foreign pointer from one encoded target address.
    #[must_use]
    fn new(target_address: Self::Address) -> Self;

    /// Return the encoded foreign address without widening it.
    #[must_use]
    fn address(self) -> Self::Address;

    /// Determine whether the encoded foreign address is null.
    #[must_use]
    fn null(self) -> bool;

    /// Determine whether the encoded foreign address is non-null.
    #[must_use]
    fn nonnull(self) -> bool;
}

#[cfg(test)]
mod test {
    use core::{any::TypeId, mem};

    use super::{Pointer32, Pointer64};

    #[test]
    fn foreign_pointer_layout_matches_its_encoded_width() {
        assert_eq!(mem::size_of::<Pointer32<*mut u8>>(), mem::size_of::<u32>());
        assert_eq!(
            mem::align_of::<Pointer32<*mut u8>>(),
            mem::align_of::<u32>()
        );
        assert_eq!(mem::size_of::<Pointer64<*mut u8>>(), mem::size_of::<u64>());
        assert_eq!(
            mem::align_of::<Pointer64<*mut u8>>(),
            mem::align_of::<u64>()
        );
    }

    #[test]
    fn foreign_pointer_null_predicates_are_complements() {
        let target_null32 = Pointer32::<*mut u8>::new(0);
        let target_value32 = Pointer32::<*mut u8>::new(1);
        let target_null64 = Pointer64::<*mut u8>::new(0);
        let target_value64 = Pointer64::<*mut u8>::new(1);

        assert!(target_null32.null());
        assert!(!target_null32.nonnull());
        assert!(!target_value32.null());
        assert!(target_value32.nonnull());
        assert!(target_null64.null());
        assert!(!target_null64.nonnull());
        assert!(!target_value64.null());
        assert!(target_value64.nonnull());
    }

    #[test]
    fn foreign_pointer_identity_includes_width_and_raw_pointer_type() {
        assert_ne!(
            TypeId::of::<Pointer32<*const u8>>(),
            TypeId::of::<Pointer32<*mut u8>>()
        );
        assert_ne!(
            TypeId::of::<Pointer64<*const u8>>(),
            TypeId::of::<Pointer64<*mut u8>>()
        );
        assert_ne!(
            TypeId::of::<Pointer32<*const u8>>(),
            TypeId::of::<Pointer64<*const u8>>()
        );
    }
}
