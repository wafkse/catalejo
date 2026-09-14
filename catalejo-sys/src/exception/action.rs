//! Construction of fixed-ABI immutable exception actions.

use core::{mem::MaybeUninit, num::NonZero};

use crate::ffi::binding;

/// A complete architectural recovery policy.
///
/// NOTE(invariant): Each variant encodes to the fixed 16-byte action ABI with every unused context
/// byte zero.
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
    pub fn encode(self) -> binding::mirilla_except_action {
        let mut raw = MaybeUninit::<binding::mirilla_except_action>::zeroed();

        let raw_pointer = raw.as_mut_ptr();

        match self {
            Self::None => {
                // SAFETY: raw_pointer names live zeroed storage and tag is an initialized field.
                unsafe {
                    (&raw mut (*raw_pointer).tag).write(binding::MIRILLA_EXCEPT_ACTION_NONE as u16);
                }
            }
            Self::Ip(target_address) => {
                let action_context = binding::mirilla_except_action_context {
                    ip: binding::mirilla_except_action_ip_context {
                        address: target_address.get() as binding::virtual_address_t,
                    },
                };

                // SAFETY: raw_pointer names live zeroed storage and context is an initialized
                // union field selected by the tag written below.
                unsafe { (&raw mut (*raw_pointer).context).write(action_context) };

                // SAFETY: raw_pointer names live zeroed storage and tag is an initialized field.
                unsafe {
                    (&raw mut (*raw_pointer).tag).write(binding::MIRILLA_EXCEPT_ACTION_IP as u16);
                }
            }
            Self::Retry => {
                // SAFETY: raw_pointer names live zeroed storage and tag is an initialized field.
                unsafe {
                    (&raw mut (*raw_pointer).tag)
                        .write(binding::MIRILLA_EXCEPT_ACTION_RETRY as u16);
                }
            }
        }

        // SAFETY: The buffer began fully zeroed and every semantic action writes its valid tag.
        unsafe { raw.assume_init() }
    }
}
