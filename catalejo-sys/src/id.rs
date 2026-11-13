//! Monotonic identifiers for kernel-managed state.

use core::mem;
use core::num::NonZero;
use core::ops::{Deref, DerefMut};

use crate::ffi::binding::mirilla_id_t;

/// A kernel identifier for an engaged target.
pub type TargetId = Id;

/// A kernel identifier for a peephole.
pub type PeepholeId = Id;

/// A nonzero identifier for state owned by Mirilla.
///
/// # Notes
///
/// [`TargetId`] and [`PeepholeId`] are aliases of this type, so an identifier's
/// resource kind and session membership must be tracked by its owner.
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

    // The aliases must also match their corresponding ABI identifier widths.

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
