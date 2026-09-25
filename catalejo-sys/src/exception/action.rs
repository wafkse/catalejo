//! Construction of fixed-ABI immutable exception actions.

use core::{
    mem::{self, MaybeUninit},
    num::NonZero,
};

use crate::ffi::binding;

/// A complete architectural recovery policy.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// Leave the exception to native Linux handling.
    None,

    /// Replace the saved userspace instruction pointer.
    Ip(
        /// The nonzero userspace instruction address restored on recovery.
        NonZero<usize>,
    ),

    /// Retry the faulting instruction.
    Retry,
}

impl Action {
    /// Encode the immutable kernel ABI value.
    #[inline]
    pub const fn encode(self) -> binding::mirilla_except_action {
        let mut target_storage = MaybeUninit::<binding::mirilla_except_action>::zeroed();

        let target_base = target_storage.as_mut_ptr().cast::<u8>();
        let target_tag = target_base
            .wrapping_add(mem::offset_of!(binding::mirilla_except_action, tag))
            .cast::<u16>();

        match self {
            Self::None => {
                // SAFETY: The field offset locates the complete tag within live zeroed storage.
                unsafe {
                    target_tag.write(binding::MIRILLA_EXCEPT_ACTION_NONE as u16);
                }
            }
            Self::Ip(target_address) => {
                let address = target_address.get() as binding::virtual_address_t;

                let ip = binding::mirilla_except_action_ip_context { address };

                let action_context = binding::mirilla_except_action_context { ip };

                let target_context = target_base
                    .wrapping_add(mem::offset_of!(binding::mirilla_except_action, context))
                    .cast::<binding::mirilla_except_action_context>();

                // SAFETY: The field offset locates the complete context within live zeroed
                // storage. The tag written below selects its initialized union member.
                unsafe { target_context.write(action_context) };

                // SAFETY: The field offset locates the complete tag within live zeroed storage.
                unsafe {
                    target_tag.write(binding::MIRILLA_EXCEPT_ACTION_IP as u16);
                }
            }
            Self::Retry => {
                // SAFETY: The field offset locates the complete tag within live zeroed storage.
                unsafe {
                    target_tag.write(binding::MIRILLA_EXCEPT_ACTION_RETRY as u16);
                }
            }
        }

        // SAFETY: The buffer began fully zeroed and every semantic action writes its valid tag.
        unsafe { target_storage.assume_init() }
    }
}
