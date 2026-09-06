//! Monotonic identifiers for kernel-managed state.

use core::mem;
use core::num::NonZero;
use core::ops::{Deref, DerefMut};

use crate::ffi::binding::mirilla_id_t;

/// A type alias to the identifier type for a engaged target.
pub type TargetId = Id;

/// A type alias to the identifier type for a peephole.
pub type PeepholeId = Id;

/// An identifier managed by the module.
///
/// This is used to refer to any contextual structure held in the kernel for `mirilla`.
#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct Id(pub NonZero<mirilla_id_t>);

impl Deref for Id {
    type Target = NonZero<mirilla_id_t>;

    #[inline]
    fn deref(&self) -> &Self::Target {
        let Self(target_value) = self;

        target_value
    }
}

impl DerefMut for Id {
    #[inline]
    fn deref_mut(&mut self) -> &mut Self::Target {
        let Self(target_value) = self;

        target_value
    }
}

// NOTE: Assert that such `Id` type is valid to be layed out as an `mirilla` identifier.
const _: () = const {
    use crate::ffi::binding;

    assert!(mem::size_of::<Option<Id>>() == mem::size_of::<binding::mirilla_id_t>());
    assert!(mem::align_of::<Option<Id>>() == mem::align_of::<binding::mirilla_id_t>());

    // NOTE: Bogus, but better to be safe than sorry..

    assert!(
        mem::size_of::<Option<TargetId>>() == mem::size_of::<binding::mirilla_map_target_id_t>()
    );
    assert!(
        mem::align_of::<Option<TargetId>>() == mem::align_of::<binding::mirilla_map_target_id_t>()
    );

    assert!(
        mem::size_of::<Option<PeepholeId>>()
            == mem::size_of::<binding::mirilla_map_peephole_id_t>()
    );
    assert!(
        mem::align_of::<Option<PeepholeId>>()
            == mem::align_of::<binding::mirilla_map_peephole_id_t>()
    );
};
