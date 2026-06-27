//! Primitive types in respect to a memory operation.

use core::mem;

use crate::behavior::Unassociated;

/// A primitive that can be read with machine-word coherence.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Primitive {
    /// [`prim@u8`]-level coherence.
    U8 = mem::size_of::<u8>().cast_signed(),

    /// [`prim@u16`]-level coherence.
    U16 = mem::size_of::<u16>().cast_signed(),

    /// [`prim@u32`]-level coherence.
    U32 = mem::size_of::<u32>().cast_signed(),

    /// [`prim@u64`]-level coherence.
    U64 = mem::size_of::<u64>().cast_signed(),
}

impl Primitive {
    /// Determine an appropiate [`Primitive`] type variant for the target type.
    #[inline]
    pub const fn appropiate<T>() -> Option<Self>
    where
        T: Unassociated,
    {
        match mem::size_of::<T>() {
            1 => Some(Self::U8),
            2 => Some(Self::U16),
            4 => Some(Self::U32),
            8 => Some(Self::U64),
            _ => None,
        }
    }
}

/// An union of all primitives.
///
/// This is used for primitive-independent operations.
#[derive(Clone, Copy)]
#[repr(C)]
pub union PrimitiveUnion {
    /// [`prim@u8`]
    pub u8: u8,

    /// [`prim@u16`]
    pub u16: u16,

    /// [`prim@u32`]
    pub u32: u32,

    /// [`prim@u64`]
    pub u64: u64,
}
