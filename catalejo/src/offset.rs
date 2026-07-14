//! Offset management for elaborate structures.

use core::{borrow::Borrow, marker, mem};

use catalejo_memory::behavior::Immortal;

use catalejo_sys::ffi;

// NOTE: Re-export so downstream crates can consume both `Field` as a trait and a derive macro.
pub use catalejo_macro::Field;

// NOTE: Re-exported so downstream `Field` derive output can name `Unassociated`
// through `catalejo` without depending on `catalejo-memory` directly.
pub use catalejo_memory::behavior::Unassociated;

/// A local offset applied to a virtual base.
#[derive(Debug, Eq, PartialEq, PartialOrd, Ord, Default, Hash, Clone, Copy)]
#[repr(transparent)]
pub struct Offset(ffi::binding::virtual_offset_t);

impl Offset {
    /// Construct a byte-level offset from the target value.
    #[inline]
    pub const fn byte(target_value: ffi::binding::virtual_offset_t) -> Self {
        Self(target_value)
    }

    /// Construct a type-level offset from the target value.
    ///
    /// This determines the offset using the size of the parametric type.
    #[inline]
    pub const fn typed<T>(target_value: ffi::binding::virtual_offset_t) -> Self {
        let target_size = mem::size_of::<T>() as ffi::binding::virtual_offset_t;

        Self::byte(target_size.wrapping_mul(target_value))
    }

    /// Stack two disjoint offsets onto a singular one.
    ///
    /// This is equivalent to add the two byte-level together via wrapping arithmetic.
    #[inline]
    pub const fn stack(self, target_value: Self) -> Self {
        let Self(target_left) = self;
        let Self(target_right) = target_value;

        Self(target_left.wrapping_add(target_right))
    }
}

impl Offset {
    /// Determine the encapsulated offset value.
    #[inline]
    pub const fn value(self) -> ffi::binding::virtual_offset_t {
        let Self(target_value) = self;

        target_value
    }

    /// Borrow the encapsulated offset value.
    #[inline]
    pub const fn value_mut(&mut self) -> &mut ffi::binding::virtual_offset_t {
        let Self(target_value) = self;

        target_value
    }

    /// Determine the encapsulated offset value as a native pointer-width integer.
    #[inline]
    pub const fn native(self) -> usize {
        let Self(target_value) = self;

        target_value as usize
    }
}

/// A trait that describes a field inside a specific structure.
///
/// # Safety
///
/// The returned offset must be within in bounds of the associated structure.
pub unsafe trait Field: Unassociated {
    /// The structure that this fields belongs to.
    type Structure: Unassociated;

    /// The value that this field is for and the thing actually read.
    type Value: Unassociated;

    /// Determine the offset of this field in respect to its structure.
    fn offset(target_value: impl Borrow<Self>) -> Offset;
}

/// A supertrait over [`Field`] to represent possibly-dynamic structure field offsets.
///
/// This is intended as a way to automatically resolve offsets at runtime
pub trait Retrievable: Field {
    /// The offset database source.
    type Source: Source;

    /// The packet provided to the database
    type Packet: Retrieve
    where
        Self::Source: Source<Packet = Self::Packet>;
}

/// A marker trait to describe a packet of information required for offset retrieval.
pub trait Retrieve: Immortal {}

/// A trait to describe an offset database or source.
pub trait Source {
    /// The type of packet this source requires.
    type Packet: Retrieve;

    /// Attempt to retrieve the [`Retrievable`] value from the source.
    fn retrieve<O>(&self, target_packet: O::Packet) -> Option<O>
    where
        O: Retrievable<Source = Self>,
        Self: Source<Packet = O::Packet>;
}

/// A marker type to represent an address space marker for non-relative virtual addresses.
#[derive(Debug, Copy, Clone)]
#[repr(transparent)]
pub struct Absolute(marker::PhantomData<Self>);

// SAFETY: `AddressSpace` is a fieldless zero-sized type with exactly one
// inhabitant (`PhantomData` contributes no bytes and has a single valid
// representation). Its sole bit-pattern is the empty one, always valid, so it
// round-trips through a byte representation and cannot tear. Reading one is a
// no-op yielding the sole inhabitant.
unsafe impl Unassociated for Absolute {}
