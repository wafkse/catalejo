//! Construction, registration and description of the immutable userspace exception image.
//!
//! The exception image is the userspace half of Mirilla fault recovery. Its rollback region holds
//! the relocated protected accessors and their recovery paths. Its exception-table region records
//! the protected instruction ranges, accepted architectural exceptions and rollback destinations
//! consumed by Mirilla.
//!
//! The C runtime constructs the image, registers it for the calling address space and retains the
//! Mirilla session that owns the registration. Rust only exposes an [`Image`] after that complete
//! sequence succeeds.

#[cfg(feature = "stealth-mode")]
use std::process;
use std::{
    io,
    os::fd::{AsRawFd, BorrowedFd},
};

use crate::ffi::binding;

/// A virtual-memory region described by an exception image.
///
/// The value carries the base address and complete byte size used by the Mirilla exception-image
/// ABI. It does not own the mapping. The mapping and registration guarantees come from possession
/// of the enclosing [`Image`].
#[derive(Debug, Copy, Clone)]
pub struct ImageRegion {
    /// The virtual address of the first byte described by the region.
    base_address: binding::virtual_address_t,

    /// The complete byte size of the described region.
    region_size: binding::virtual_size_t,
}

impl ImageRegion {
    /// Lift a bare Mirilla region descriptor into the Rust representation.
    ///
    /// This preserves the address and size exactly as supplied by the ABI. No independent mapping
    /// or registration claim is created by this conversion.
    #[inline]
    const fn lift(target_value: binding::mirilla_except_region) -> Self {
        let binding::mirilla_except_region {
            region_address: base_address,
            region_size,
        } = target_value;

        Self {
            base_address,
            region_size,
        }
    }

    /// Return the virtual address of the first byte described by this region.
    #[inline]
    pub const fn address(&self) -> binding::virtual_address_t {
        let &Self { base_address, .. } = self;

        base_address
    }

    /// Return the complete byte size described by this region.
    #[inline]
    pub const fn size(&self) -> binding::virtual_size_t {
        let &Self { region_size, .. } = self;

        region_size
    }
}

/// The process-global immutable image registered with Mirilla.
///
/// The C layer owns the runtime, its one-time state machine and a dedicated Mirilla session that
/// owns the registration. It publishes the runtime only after the mappings are immutable and
/// registration for the calling address space succeeds. Possession of an `Image` therefore proves
/// both image construction and Mirilla registration for the process that retrieved it.
///
/// A process created through `fork` inherits the mappings and Rust references but receives a new
/// address space. Inherited references must not be used in the child. Call [`Image::register`]
/// through `Target::register_current_address_space` before reacquiring the image in that process.
#[repr(transparent)]
#[derive(Debug)]
// NOTE(invariant): C owns the only allocation with this representation. It publishes the pointer
// with release ordering only after immutable construction and Mirilla registration succeed. Rust
// acquires the C state before lifting that process-lifetime allocation into this opaque type.
pub struct Image(binding::catalejo_image_runtime);

impl Image {
    /// Register and publish the exception image through a Mirilla session.
    ///
    /// The first successful call constructs the sealed mappings, registers them for the calling
    /// address space and retains a dedicated registration session opened through the supplied
    /// descriptor. Later calls in the same process return the published image. A child created
    /// through `fork` uses this operation to register the inherited immutable image for its new
    /// address space.
    ///
    /// # Safety
    ///
    /// `target_device` must be a descriptor created by Mirilla.
    ///
    /// # Errors
    ///
    /// This returns the operating-system error reported while constructing or registering the
    /// image.
    #[inline]
    pub unsafe fn register(target_device: BorrowedFd<'_>) -> io::Result<&'static Self> {
        let mut target_runtime = core::ptr::null();

        // SAFETY: The caller guarantees that the descriptor belongs to Mirilla. C initializes the
        // output pointer only after construction and registration succeed.
        let target_status = unsafe {
            binding::catalejo_fault_image_initialize(
                target_device.as_raw_fd(),
                core::ptr::from_mut(&mut target_runtime),
            )
        };

        Self::lift_registered(target_status, target_runtime)
    }

    /// Retrieve the image registered for the calling address space.
    ///
    /// This operation never constructs or registers an image. It succeeds only after
    /// [`Self::register`] has published the process-global runtime for the current process. A child
    /// created through `fork` receives an error until it registers its distinct address space.
    ///
    /// # Errors
    ///
    /// This returns an operating-system error when no image is registered for the calling address
    /// space.
    #[inline]
    pub fn retrieve() -> io::Result<&'static Self> {
        let mut target_runtime = core::ptr::null();

        // SAFETY: C either leaves the pointer null and returns an error or publishes its
        // process-lifetime immutable runtime through the output pointer.
        let target_status = unsafe {
            binding::catalejo_fault_image_retrieve(core::ptr::from_mut(&mut target_runtime))
        };

        Self::lift_registered(target_status, target_runtime)
    }

    /// Return the registered image or terminate on an invariant violation.
    ///
    /// Low-level callers use this after their owning context has established the registration. A
    /// regular build treats a missing registration as unreachable. A stealth-mode build aborts
    /// without formatting a diagnostic.
    ///
    /// # Panics
    ///
    /// Without `stealth-mode`, this panics when the image is not registered for the calling process.
    ///
    /// # Aborts
    ///
    /// With `stealth-mode`, this aborts when the image is not registered for the calling process.
    #[inline]
    pub fn infallible() -> &'static Self {
        match Self::retrieve() {
            Ok(target_image) => target_image,
            #[cfg(not(feature = "stealth-mode"))]
            Err(..) => unreachable!(),
            #[cfg(feature = "stealth-mode")]
            Err(..) => process::abort(),
        }
    }

    /// Return the registered image using the configured failure policy.
    ///
    /// Regular builds retain a descriptive failure for invariant violations. Stealth-mode builds
    /// delegate to [`Self::infallible`] so diagnostic text is excluded at compile time.
    ///
    /// # Panics
    ///
    /// Without `stealth-mode`, this panics when the image is not registered for the calling process.
    ///
    /// # Aborts
    ///
    /// With `stealth-mode`, this aborts when the image is not registered for the calling process.
    #[inline]
    pub fn preferred() -> &'static Self {
        #[cfg(not(feature = "stealth-mode"))]
        {
            Self::retrieve().expect("exception image is not registered for this process")
        }

        #[cfg(feature = "stealth-mode")]
        {
            Self::infallible()
        }
    }

    /// Lift a successful C publication into the opaque Rust proof type.
    fn lift_registered(
        target_status: core::ffi::c_int,
        target_runtime: *const binding::catalejo_image_runtime,
    ) -> io::Result<&'static Self> {
        if target_status < 0 {
            return Err(io::Error::from_raw_os_error(target_status.saturating_abs()));
        }

        if target_status != 0 || target_runtime.is_null() {
            #[cfg(not(feature = "stealth-mode"))]
            return Err(io::Error::from(io::ErrorKind::Other));

            #[cfg(feature = "stealth-mode")]
            process::abort();
        }

        // SAFETY: A zero status means C published a non-null pointer to its process-lifetime,
        // immutable `catalejo_image_runtime`. `Image` is transparent over that exact type.
        Ok(unsafe { &*target_runtime.cast::<Self>() })
    }

    /// Return the C runtime backing the protected accessor shims.
    #[inline]
    pub const fn runtime(&self) -> &binding::catalejo_image_runtime {
        let &Self(ref target_runtime) = self;

        target_runtime
    }

    /// Return the immutable region containing relocated protected accessors and rollback code.
    #[inline]
    pub const fn rollback_region(&self) -> ImageRegion {
        let &Self(binding::catalejo_image_runtime {
            image: binding::mirilla_except_image {
                rollback_region, ..
            },
            ..
        }) = self;

        ImageRegion::lift(rollback_region)
    }

    /// Return the immutable region containing the architectural exception table.
    #[inline]
    pub const fn except_table(&self) -> ImageRegion {
        let &Self(binding::catalejo_image_runtime {
            image: binding::mirilla_except_image { except_table, .. },
            ..
        }) = self;

        ImageRegion::lift(except_table)
    }
}
