//! Offset management for elaborate structures.

use core::marker;

use catalejo_memory::{behavior::Immortal, prelude::Unassociated};

use crate::address::Offset;

/// A trait that describes a field inside a specific structure.
pub trait Field: Unassociated {
    /// The structure that this fields belongs to.
    type Structure: Unassociated;

    /// Determine the offset of this field in respect to its structure.
    fn offset(&self) -> Offset;
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

/// A marker type to represent an address space.
#[derive(Debug, Copy, Clone)]
#[repr(transparent)]
pub struct AddressSpace(marker::PhantomData<Self>);

// SAFETY: `AddressSpace` is a fieldless zero-sized type with exactly one
// inhabitant (`PhantomData` contributes no bytes and has a single valid
// representation). Its sole bit-pattern is the empty one, always valid, so it
// round-trips through a byte representation and cannot tear. Reading one is a
// no-op yielding the sole inhabitant.
unsafe impl Unassociated for AddressSpace {}
