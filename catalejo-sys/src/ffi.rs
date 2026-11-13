//! Mirilla bindings, device commands, and raw layout queries.

pub mod binding {
    #![allow(
        nonstandard_style,
        missing_docs,
        unsafe_op_in_unsafe_fn,
        improper_ctypes,
        clippy::missing_safety_doc,
        reason = "bindgen-generated bindings have non-standard style and generated unsafe helper methods"
    )]
    //! Generated declarations for the userspace C routines and Mirilla ABI.

    // NOTE: Include the `bindgen`-generated bindings for our own crate.
    include!(concat!(env!("OUT_DIR"), "/catalejo-binding.rs"));
}

pub mod command {
    //! Commands for userspace-kernel device ioctls.
    #![allow(
        clippy::std_instead_of_core,
        reason = "imports are false-flagged by clippy where the fix would be nightly-only"
    )]

    #[cfg(feature = "default-device-path")]
    use core::ffi::CStr;

    use std::{
        io,
        io::ErrorKind,
        os::fd::{AsRawFd, BorrowedFd, OwnedFd, RawFd},
        path::Path,
    };

    #[cfg(feature = "default-device-path")]
    use std::{path::PathBuf, sync::LazyLock};

    use core::ptr;

    use crate::{
        ffi::binding::{self, mirilla_map_peephole_initialize_word_t, virtual_address_t},
        id::{PeepholeId, TargetId},
    };

    /// The configured device name used to locate the Mirilla character device.
    #[cfg(feature = "default-device-path")]
    pub const MIRILLA_DEVICE_NAME: &str = const {
        // SAFETY: The `CStr` is obtained from a `bindgen`-generated C string literal, so it always properly nul-delimited.
        let target_value =
            unsafe { CStr::from_bytes_with_nul_unchecked(binding::MIRILLA_DEVICE_DEFAULT_NAME) };

        match target_value.to_str() {
            Ok(target_value) => target_value,
            Err(..) => unreachable!(),
        }
    };

    /// Determine the optionally configured default path to the kernel character device.
    ///
    /// A build without the `default-device-path` feature returns [`None`]. Callers must then
    /// provide a path explicitly or use an already-open device file descriptor.
    #[inline]
    #[must_use]
    pub fn default_device_path() -> Option<&'static Path> {
        #[cfg(feature = "default-device-path")]
        {
            static DEFAULT_DEVICE_PATH: LazyLock<PathBuf> =
                LazyLock::new(|| PathBuf::from("/dev/").join(self::MIRILLA_DEVICE_NAME));

            Some(DEFAULT_DEVICE_PATH.as_path())
        }

        #[cfg(not(feature = "default-device-path"))]
        {
            None
        }
    }

    /// Engage with the target process.
    ///
    /// # Failure
    ///
    /// Engagement fails if the process does not exist or the caller lacks
    /// permission to observe it.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn engage(fd: BorrowedFd, process_id: binding::pid_t) -> io::Result<TargetId> {
        let mut target_engagement = None::<TargetId>;

        let target_outcome =
            // SAFETY:
            //
            // * The caller has asserted that the provided file descriptor comes from `mirilla`.
            // * `mirilla_map_target_id_t` is identical ABI-wise to `Option<TargetId>`.
            unsafe { binding::catalejo_mirilla_engage(fd.as_raw_fd(), process_id, ptr::from_mut(&mut target_engagement).cast::<binding::mirilla_map_target_id_t>()) };

        match (target_outcome, target_engagement) {
            (binding::MIRILLA_COMMAND_OK, Some(target_id)) => Ok(target_id),
            (binding::MIRILLA_COMMAND_OK, ..) => Err(io::Error::from(ErrorKind::InvalidInput)),
            (
                target_errno @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK,
                ..,
            ) => Err(io::Error::from_raw_os_error(target_errno.abs())),
            // NOTE: This is impossible, hence unreachable.
            #[cfg(not(feature = "stealth-mode"))]
            _ => unreachable!(),

            #[cfg(feature = "stealth-mode")]
            _ => std::process::abort(),
        }
    }

    /// Disengage from the target process.
    ///
    /// # Failure
    ///
    /// The kernel rejects identifiers not engaged through this session.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn disengage(fd: OwnedFd, target_id: TargetId) -> io::Result<()> {
        let target_outcome =
            // SAFETY: The caller has asserted that the provided file descriptor comes from `mirilla`.
            unsafe { binding::catalejo_mirilla_disengage(fd.as_raw_fd(), target_id.get()) };

        match target_outcome {
            binding::MIRILLA_COMMAND_OK => Ok(()),
            target_errno @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK => {
                Err(io::Error::from_raw_os_error(target_errno.abs()))
            }
            // NOTE: This is impossible, hence unreachable.
            #[cfg(not(feature = "stealth-mode"))]
            _ => unreachable!(),

            #[cfg(feature = "stealth-mode")]
            _ => std::process::abort(),
        }
    }

    /// For an engaged target process, create a peephole over the specified virtual memory range.
    ///
    /// `initialize_word` is a bitset of `MIRILLA_MAP_PEEPHOLE_INITIALIZE_*` preferences the kernel
    /// applies at creation, so that a caller can request one-shot behavior such as populating the
    /// mapping without a follow-up command.
    ///
    /// # Failure
    ///
    /// The target must already be engaged through this session. The kernel
    /// rejects requests with an unknown target or a malformed address range.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn peephole(
        fd: BorrowedFd,
        target_id: TargetId,
        start_address: virtual_address_t,
        end_address: virtual_address_t,
        initialize_word: mirilla_map_peephole_initialize_word_t,
    ) -> io::Result<(PeepholeId, OwnedFd)> {
        let mut peephole_id = None::<PeepholeId>;
        let mut peephole_fd = None::<OwnedFd>;

        let target_outcome =
            // SAFETY:
            //
            // * The caller has asserted that the provided file descriptor comes from `mirilla`.
            // * `mirilla_map_peephole_id_t` is identical ABI-wise to `Option<PeepholeId>`.
            // * `OwnedFd/RawFd` is identical ABI-wise to a host file descriptor.
            unsafe { binding::catalejo_mirilla_peephole(fd.as_raw_fd(), target_id.get(), start_address, end_address, initialize_word, ptr::from_mut(&mut peephole_id).cast::<binding::mirilla_map_target_id_t>(), ptr::from_mut(&mut peephole_fd).cast::<RawFd>()) };

        match (target_outcome, (peephole_id, peephole_fd)) {
            (binding::MIRILLA_COMMAND_OK, (Some(target_left), Some(target_right))) => {
                Ok((target_left, target_right))
            }
            (binding::MIRILLA_COMMAND_OK, (..)) => Err(io::Error::from(ErrorKind::InvalidInput)),
            (
                target_errno @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK,
                (..),
            ) => Err(io::Error::from_raw_os_error(target_errno.abs())),
            // NOTE: This is impossible, hence unreachable.
            #[cfg(not(feature = "stealth-mode"))]
            _ => unreachable!(),

            #[cfg(feature = "stealth-mode")]
            _ => std::process::abort(),
        }
    }

    /// For an engaged target process, retrieve the full address space layout, the kernel-resident
    /// auxiliary vector, and the argument/environment metadata.
    ///
    /// The target can change between sizing the buffers and filling them. If a count
    /// grows, the operation retries with a larger buffer. If it shrinks, only the
    /// populated prefix is returned.
    ///
    /// Retries are bounded, so repeated layout changes can cause this operation
    /// to fail.
    ///
    /// # Notes
    ///
    /// The returned vectors own their entries, but the sizing and population
    /// passes do not form an atomic snapshot of the target.
    ///
    /// # Failure
    ///
    /// The query fails in the following cases.
    ///
    /// * The target identifier does not belong to this session.
    /// * The kernel reports a count too large to allocate.
    /// * The population pass cannot converge within the retry bound.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    #[inline]
    pub unsafe fn address_space_layout(
        fd: BorrowedFd,
        target_id: TargetId,
    ) -> io::Result<(
        crate::ffi::lower::AddressSpaceMetadata,
        Vec<crate::ffi::lower::AddressSpaceLayout>,
        Vec<crate::ffi::lower::AuxiliaryVectorEntry>,
    )> {
        const RETRY_BOUND: u32 = 8;

        let mut retry_count = 0;

        loop {
            // Sizing pass: ask for no population so the kernel only reports the counts.
            let mut layout_descriptor = binding::mirilla_outside_list {
                list_address: 0,
                list_size: 0,
                element_size: core::mem::size_of::<crate::ffi::lower::AddressSpaceLayout>() as u32,
                list_attribute: binding::MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
            };
            let mut auxiliary_vector_descriptor = binding::mirilla_outside_list {
                list_address: 0,
                list_size: 0,
                element_size: core::mem::size_of::<crate::ffi::lower::AuxiliaryVectorEntry>()
                    as u32,
                list_attribute: binding::MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE,
            };

            let sizing_outcome =
                // SAFETY: The caller has asserted that the provided file descriptor comes from `mirilla`.
                // Both lists carry `DO_NOT_POPULATE`, so no backing buffer is dereferenced.
                unsafe {
                    crate::ffi::lower::address_space_layout(
                        fd,
                        target_id,
                        &mut layout_descriptor,
                        &mut auxiliary_vector_descriptor,
                    )
                }?;

            let layout_capacity = sizing_outcome.layout_total_count;
            let auxiliary_vector_capacity = sizing_outcome.auxiliary_vector_total_count;

            let mut layout_buffer: Vec<crate::ffi::lower::AddressSpaceLayout> =
                Vec::with_capacity(usize::try_from(layout_capacity).unwrap_or(0));
            let mut auxiliary_vector_buffer: Vec<crate::ffi::lower::AuxiliaryVectorEntry> =
                Vec::with_capacity(usize::try_from(auxiliary_vector_capacity).unwrap_or(0));

            // Population pass: supply the buffers and let the kernel fill them.
            let mut layout_descriptor = binding::mirilla_outside_list {
                list_address: if layout_capacity != 0 {
                    layout_buffer.as_mut_ptr().expose_provenance() as u64
                } else {
                    0
                },
                list_size: layout_capacity,
                element_size: core::mem::size_of::<crate::ffi::lower::AddressSpaceLayout>() as u32,
                list_attribute: 0,
            };
            let mut auxiliary_vector_descriptor = binding::mirilla_outside_list {
                list_address: if auxiliary_vector_capacity != 0 {
                    auxiliary_vector_buffer.as_mut_ptr().expose_provenance() as u64
                } else {
                    0
                },
                list_size: auxiliary_vector_capacity,
                element_size: core::mem::size_of::<crate::ffi::lower::AuxiliaryVectorEntry>()
                    as u32,
                list_attribute: 0,
            };

            let population_outcome =
                // SAFETY:
                //
                // * The caller has asserted that the provided file descriptor comes from `mirilla`.
                // * Each backing buffer is a valid `Vec` allocation of the matching element type
                //   and capacity, held for the duration of the call.
                unsafe {
                    crate::ffi::lower::address_space_layout(
                        fd,
                        target_id,
                        &mut layout_descriptor,
                        &mut auxiliary_vector_descriptor,
                    )
                }?;

            // Convergence: the counts must not have grown beyond the allocated capacity. A shrink
            // is safe: the populated prefix is valid and the trailing slots are uninitialized. A
            // grow means the kernel reported more entries than the buffer can hold, so retry with
            // the new count.
            if population_outcome.layout_total_count <= layout_capacity
                && population_outcome.auxiliary_vector_total_count <= auxiliary_vector_capacity
            {
                // SAFETY: The kernel populated exactly `population_outcome.*_total_count` entries,
                // each of the matching element type, into the buffer.
                unsafe {
                    layout_buffer.set_len(population_outcome.layout_total_count as usize);

                    auxiliary_vector_buffer
                        .set_len(population_outcome.auxiliary_vector_total_count as usize);
                }

                return Ok((
                    population_outcome.metadata,
                    layout_buffer,
                    auxiliary_vector_buffer,
                ));
            }

            retry_count += 1;
            if retry_count >= RETRY_BOUND {
                #[cfg(feature = "stealth-mode")]
                return Err(io::Error::from(ErrorKind::ResourceBusy));

                #[cfg(not(feature = "stealth-mode"))]
                return Err(io::Error::new(
                    ErrorKind::ResourceBusy,
                    "address space layout count did not converge within the retry bound",
                ));
            }
        }
    }
}

pub mod lower {
    //! Layout queries using caller-provided output buffers.

    use std::{io, os::fd::AsRawFd, os::fd::BorrowedFd};

    use crate::{ffi::binding, id::TargetId};

    /// The metadata of an address space, as returned by a layout query.
    pub type AddressSpaceMetadata = binding::mirilla_map_address_space_metadata;

    /// A single address space layout entry, as returned by a layout query.
    pub type AddressSpaceLayout = binding::mirilla_map_address_space_layout;

    /// A single auxiliary vector entry, as returned by a layout query.
    pub type AuxiliaryVectorEntry = binding::mirilla_auxiliary_vector_entry;

    /// Metadata and entry counts reported by a layout query.
    ///
    /// # Notes
    ///
    /// Counts can exceed the supplied buffer capacities. Only the populated
    /// prefixes can be read from those buffers.
    #[derive(Debug)]
    pub struct AddressSpaceLayoutOutcome {
        /// Kernel-resident metadata of the address space whose layout was requested.
        pub metadata: AddressSpaceMetadata,

        /// The full kernel-resident count of address space layout entries.
        pub layout_total_count: u32,

        /// The full kernel-resident count of auxiliary vector entries.
        pub auxiliary_vector_total_count: u32,
    }

    /// Query layout metadata and populate caller-provided entry buffers.
    ///
    /// The caller describes each output buffer through a `mirilla_outside_list`,
    /// supplying its address, capacity, and element size. The kernel fills as
    /// many entries as fit, then reports the total available count through the
    /// matching outcome. Because that count can exceed capacity, callers must
    /// read only the populated prefix and resize before requesting more entries.
    ///
    /// # Safety
    ///
    /// For soundness purposes, the following must be satisfied:
    ///
    /// * The provided file descriptor must be a valid `mirilla`-created one.
    /// * Each output buffer must remain writable for its declared capacity
    ///   multiplied by its element size throughout the call.
    /// * A zero output address is permitted only when the corresponding list
    ///   has `MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE` set.
    #[inline]
    pub unsafe fn address_space_layout(
        fd: BorrowedFd,
        target_id: TargetId,
        layout_list: &mut binding::mirilla_outside_list,
        auxiliary_vector_list: &mut binding::mirilla_outside_list,
    ) -> io::Result<AddressSpaceLayoutOutcome> {
        let mut metadata = core::mem::MaybeUninit::<AddressSpaceMetadata>::uninit();
        let mut layout_outcome =
            core::mem::MaybeUninit::<binding::mirilla_outside_list_outcome>::uninit();
        let mut auxiliary_vector_outcome =
            core::mem::MaybeUninit::<binding::mirilla_outside_list_outcome>::uninit();

        let target_outcome =
            // SAFETY: The safety concerns of the foreign call have been satisfied by the caller.
            unsafe {
                binding::catalejo_mirilla_address_space_layout(
                    fd.as_raw_fd(),
                    target_id.get(),
                    layout_list,
                    auxiliary_vector_list,
                    metadata.as_mut_ptr(),
                    layout_outcome.as_mut_ptr(),
                    auxiliary_vector_outcome.as_mut_ptr(),
                )
            };

        match target_outcome {
            binding::MIRILLA_COMMAND_OK => Ok(AddressSpaceLayoutOutcome {
                // SAFETY: The kernel wrote the metadata on success.
                metadata: unsafe { metadata.assume_init() },
                // SAFETY: The kernel wrote the address space layout on success.
                layout_total_count: unsafe { layout_outcome.assume_init() }.total_count,
                // SAFETY: The kernel wrote the auxiliary vector outcome on success.
                auxiliary_vector_total_count: unsafe { auxiliary_vector_outcome.assume_init() }
                    .total_count,
            }),
            target_errno @ binding::mirilla_command_status_t::MIN..binding::MIRILLA_COMMAND_OK => {
                Err(io::Error::from_raw_os_error(target_errno.abs()))
            }
            // NOTE: This is impossible, hence unreachable.
            #[cfg(not(feature = "stealth-mode"))]
            _ => unreachable!(),

            #[cfg(feature = "stealth-mode")]
            _ => std::process::abort(),
        }
    }
}
