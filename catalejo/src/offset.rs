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
/// The returned offset must place the complete byte extent of [`Self::Value`]
/// within [`Self::Structure`]. The resulting address must satisfy the alignment
/// required by [`Self::Value`] when the structure base is properly aligned.
pub unsafe trait Field: Unassociated {
    /// The structure that this fields belongs to.
    type Structure: Unassociated;

    /// The value that this field is for and the thing actually read.
    type Value: Unassociated;

    /// Determine the offset of this field in respect to its structure.
    fn offset(target_value: impl Borrow<Self>) -> Offset;
}

/// A request that resolves an output from an offset source.
pub trait Retrieve: Immortal {
    /// The source that resolves this request.
    type Source: Source;

    /// The value produced by a successful request.
    type Output;

    /// Resolve this request against the target source.
    ///
    /// Returns [`None`] when the source cannot resolve the request.
    fn resolve(self, target_source: &Self::Source) -> Option<Self::Output>;
}

/// An offset database or source that resolves typed requests.
pub trait Source {
    /// Attempt to resolve a typed request against this source.
    ///
    /// Returns [`None`] when the request cannot be resolved.
    fn retrieve<R>(&self, target_request: R) -> Option<R::Output>
    where
        Self: Sized,
        R: Retrieve<Source = Self>,
    {
        Retrieve::resolve(target_request, self)
    }
}

/// A marker type to represent an address space marker for non-relative virtual addresses.
#[derive(Debug, Copy, Clone)]
#[repr(transparent)]
pub struct Absolute(marker::PhantomData<Self>);

// SAFETY: `Absolute` is a fieldless zero-sized type with exactly one
// inhabitant (`PhantomData` contributes no bytes and has a single valid
// representation). Its sole bit-pattern is the empty one, always valid, so it
// round-trips through a byte representation and cannot tear. Reading one is a
// no-op yielding the sole inhabitant.
unsafe impl Unassociated for Absolute {}

#[cfg(test)]
mod test {
    //! Focused tests for request dependent field marker retrieval.

    use core::{borrow::Borrow, mem};

    use crate::{
        ffi,
        prelude::{Field, Offset, Retrieve, Source, Unassociated},
    };

    /// A structure with two projection compatible words.
    #[derive(Clone, Copy)]
    #[repr(C)]
    // NOTE(invariant): The C layout keeps both words complete and naturally aligned.
    struct DynamicStructure {
        /// The first requestable word.
        target_first: u32,

        /// The second requestable word.
        target_second: u32,
    }

    // SAFETY: Both words accept every bit pattern and remain valid under tearing.
    unsafe impl Unassociated for DynamicStructure {}

    /// The offset of the first requestable word.
    const FIRST_OFFSET: Offset = Offset::byte(
        mem::offset_of!(DynamicStructure, target_first) as ffi::binding::virtual_offset_t
    );

    /// The offset of the second requestable word.
    const SECOND_OFFSET: Offset = Offset::byte(
        mem::offset_of!(DynamicStructure, target_second) as ffi::binding::virtual_offset_t
    );

    /// A request for a field marker at a candidate offset.
    struct Request {
        /// The candidate field offset.
        target_offset: Offset,
    }

    impl Retrieve for Request {
        /// The source that validates dynamic word requests.
        type Source = DynamicSource;

        /// The dynamic field marker produced by a valid request.
        type Output = DynamicWord;

        /// Validate the requested field extent and construct its marker.
        fn resolve(self, _target_source: &Self::Source) -> Option<Self::Output> {
            let Self { target_offset } = self;
            let target_start = Offset::value(target_offset);
            let target_value_size =
                mem::size_of::<<Self::Output as Field>::Value>() as ffi::binding::virtual_offset_t;
            let target_structure_size = mem::size_of::<<Self::Output as Field>::Structure>()
                as ffi::binding::virtual_offset_t;
            let target_value_alignment =
                mem::align_of::<<Self::Output as Field>::Value>() as ffi::binding::virtual_offset_t;
            let target_end = target_start.checked_add(target_value_size)?;

            if target_end > target_structure_size
                || !target_start.is_multiple_of(target_value_alignment)
            {
                return None;
            }

            let target_selector = if target_offset == FIRST_OFFSET { 0 } else { 1 };

            Some(DynamicWord(target_selector))
        }
    }

    /// A source that validates dynamic word requests.
    struct DynamicSource;

    impl Source for DynamicSource {}

    /// A dynamic marker for either requestable word.
    #[derive(Clone, Copy)]
    #[repr(transparent)]
    // NOTE(invariant): Every selector maps to a complete and aligned word in `DynamicStructure`.
    struct DynamicWord(
        /// The selector that identifies one of the requestable words.
        u8,
    );

    // SAFETY: Every `u8` bit pattern is valid and each selector maps to a valid field.
    unsafe impl Unassociated for DynamicWord {}

    // SAFETY: Every selector returns a complete and aligned `u32` field within `DynamicStructure`.
    unsafe impl Field for DynamicWord {
        /// The structure containing both requestable words.
        type Structure = DynamicStructure;

        /// The projected word value.
        type Value = u32;

        /// Resolve the selected word offset.
        fn offset(target_value: impl Borrow<Self>) -> Offset {
            let &Self(target_selector) = target_value.borrow();

            if target_selector == 0 {
                FIRST_OFFSET
            } else {
                SECOND_OFFSET
            }
        }
    }

    /// Resolve an offset through the type identity required by `Foreign::project`.
    fn projection_offset<P>(target_marker: impl Borrow<P>) -> Offset
    where
        P: Field<Structure = DynamicStructure, Value = u32>,
    {
        Field::offset(target_marker)
    }

    /// Verify that two requests preserve their projection compatible offsets.
    #[test]
    fn retrieves_two_dynamic_projection_offsets() {
        let target_source = DynamicSource;

        for target_expected in [FIRST_OFFSET, SECOND_OFFSET] {
            let target_marker = target_source
                .retrieve(Request {
                    target_offset: target_expected,
                })
                .expect("a valid field request should resolve");

            assert_eq!(projection_offset(target_marker), target_expected);
        }
    }

    /// Verify that a request beyond the complete structure is rejected.
    #[test]
    fn rejects_out_of_bounds_offset() {
        let target_source = DynamicSource;
        let target_offset =
            Offset::byte(mem::size_of::<DynamicStructure>() as ffi::binding::virtual_offset_t);

        assert!(target_source.retrieve(Request { target_offset }).is_none());
    }

    /// Verify that a request with insufficient value alignment is rejected.
    #[test]
    fn rejects_misaligned_offset() {
        let target_source = DynamicSource;
        let target_offset = Offset::byte(1);

        assert!(target_source.retrieve(Request { target_offset }).is_none());
    }
}
