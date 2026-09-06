//! Construction and description of the immutable userspace exception image.
//!
//! The exception image is the userspace half of Mirilla fault recovery. Its rollback region holds
//! the relocated protected accessors and their recovery paths. Its exception-table region records
//! the protected instruction ranges, accepted architectural exceptions, and rollback destinations
//! consumed by Mirilla.
//!
//! [`Image::setup`] constructs these regions once, seals their common backing object, applies the
//! final virtual-memory permissions, and seals the mappings themselves. Rust owns the one-time
//! initialization state through [`IMAGE`]. Possession of an [`Image`] therefore proves image
//! construction, but not Mirilla registration. Protected execution additionally requires the same
//! image to be registered for the current address space through a live Mirilla session.

use crate::ffi::binding;
use core::mem::MaybeUninit;
#[cfg(feature = "stealth-mode")]
use std::process;
use std::sync::OnceLock;

/// A virtual-memory region described by an exception image.
///
/// The value carries the base address and complete byte size used by the Mirilla exception-image
/// ABI. It does not own the mapping and does not by itself prove that the mapping exists, has the
/// required permissions, is sealed, or belongs to a successfully constructed [`Image`]. Those
/// guarantees come from possession of the enclosing [`Image`].
#[derive(Debug, Copy, Clone)]
// NOTE(invariant): The representation preserves the exact address and size supplied by the Mirilla
// region ABI. `ImageRegion::lift` performs representation lifting only and intentionally does not
// validate the described mapping.
pub struct ImageRegion {
    /// The virtual address of the first byte described by the region.
    base_address: binding::virtual_address_t,

    /// The complete byte size of the described region.
    region_size: binding::virtual_size_t,
}

impl ImageRegion {
    /// Lift a bare Mirilla region descriptor into the Rust representation.
    ///
    /// This preserves the address and size exactly as supplied by the ABI. No mapping lookup,
    /// permission check, sealing check, or relationship to an [`Image`] is established here.
    #[inline]
    pub const fn lift(target_value: binding::mirilla_except_region) -> Self {
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
        let &Self {
            base_address: address,
            ..
        } = self;

        address
    }

    /// Return the complete byte size described by this region.
    #[inline]
    pub const fn size(&self) -> binding::virtual_size_t {
        let &Self {
            region_size: length,
            ..
        } = self;

        length
    }
}

/// The process-global immutable image that defines Catalejo protected execution to Mirilla.
///
/// An image describes the two userspace mappings that participate in architectural exception
/// recovery. The rollback region contains the relocated protected accessors together with the code
/// reached after a protected instruction faults. The exception-table region contains the
/// field-relative records that identify each protected instruction range, its rollback destination,
/// and the architectural exception vectors accepted for that range. Mirilla consumes both regions
/// as one image when the current address space is registered.
///
/// The image is constructed from the retained Catalejo accessor and exception-table sections. The C
/// constructor copies those sections into one dedicated backing object, rebases the exception
/// records against their runtime addresses, maps the rollback region read-execute and the table
/// read-only, seals the backing object against writes and resizing, then seals both VMAs against
/// later virtual-memory changes. [`Image::setup`] publishes the Rust value only after that complete
/// sequence succeeds. The mappings therefore remain immutable at stable addresses for the lifetime
/// of the process.
///
/// Possession of an `Image` proves that this userspace construction completed. It does not prove
/// that Mirilla is currently prepared to recover exceptions from the image. Protected access also
/// requires registration through [`crate::ffi::command::register_exception_image`] for the current
/// address space, with the Mirilla session that owns that registration kept alive. A process created
/// through `fork` inherits the immutable mappings but has a distinct address space and therefore
/// requires its own registration before using protected accessors.
#[derive(Debug)]
// NOTE(invariant): `Image::setup` is the only constructor of `Image`. It publishes a value only
// after the complete C runtime, both final mappings, and their shared backing object have been
// constructed and sealed. The private fields prevent callers from assembling a trusted image from
// independently lifted `ImageRegion` descriptors or raw runtime state.
pub struct Image {
    /// The C runtime containing the sealed-image descriptor and relocated callable entry points.
    image_runtime: binding::catalejo_image_runtime,

    /// The read-execute mapping containing relocated protected accessors and their rollback paths.
    rollback_region: ImageRegion,

    /// The read-only mapping containing the exception records consumed by Mirilla.
    except_table: ImageRegion,
}

impl Image {
    /// Construct the exception image once and return its process-global proof value.
    ///
    /// The first call invokes the C constructor to relocate the retained accessors and exception
    /// records into their final VMAs, make their shared backing object immutable, seal both
    /// mappings, and return the resulting Mirilla region descriptors. Rust stores that construction
    /// result in [`IMAGE`]. Later calls return the same result without invoking the C constructor
    /// again.
    ///
    /// This operation does not register the image with Mirilla. Registration is a separate
    /// address-space operation performed through [`crate::ffi::command::register_exception_image`].
    /// Callers that execute protected accessors must keep the Mirilla session owning that
    /// registration alive for the duration of those accesses.
    ///
    /// # Failure
    ///
    /// This returns [`binding::CATALEJO_OUTCOME_ERROR`] when the C constructor cannot complete the
    /// immutable runtime image. The process-global cell retains that failure result, so construction
    /// is never attempted a second time.
    #[inline]
    pub fn retrieve() -> Result<&'static Self, binding::catalejo_outcome_t> {
        match IMAGE.get_or_init(Self::construct) {
            Ok(image) => Ok(image),
            Err(target_outcome) => Err(*target_outcome),
        }
    }

    ///
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

    /// Construct the sealed runtime image through the C image builder.
    fn construct() -> Result<Self, binding::catalejo_outcome_t> {
        let mut target_runtime = MaybeUninit::<binding::catalejo_image_runtime>::uninit();

        // SAFETY:
        // The C constructor receives writable storage and initializes the complete runtime on success.
        let initialize_status =
            unsafe { binding::catalejo_fault_image_initialize(target_runtime.as_mut_ptr()) };

        match initialize_status {
            binding::CATALEJO_OUTCOME_SUCCESS => {
                // SAFETY: A successful outcome guarantees that the C constructor wrote the complete runtime.
                let image_runtime = unsafe { MaybeUninit::assume_init(target_runtime) };

                let rollback_region = ImageRegion::lift(image_runtime.image.rollback_region);
                let except_table = ImageRegion::lift(image_runtime.image.except_table);

                Ok(Self {
                    image_runtime,
                    rollback_region,
                    except_table,
                })
            }
            binding::CATALEJO_OUTCOME_ERROR => Err(initialize_status),
            #[cfg(not(feature = "stealth-mode"))]
            _ => unreachable!(),

            #[cfg(feature = "stealth-mode")]
            _ => std::process::abort(),
        }
    }

    /// Return the C runtime backing the protected accessor shims.
    ///
    /// The runtime contains the same Mirilla image descriptor represented by this [`Image`] together
    /// with the relocated callable entry points projected into the executable rollback mapping.
    #[inline]
    pub const fn runtime(&self) -> &binding::catalejo_image_runtime {
        let Self {
            image_runtime: runtime,
            ..
        } = self;

        runtime
    }

    /// Return the immutable region containing relocated protected accessors and rollback code.
    ///
    /// The region is mapped read-execute. Mirilla validates it as the only address range from which
    /// protected instruction ranges and rollback destinations may be resolved.
    #[inline]
    pub const fn rollback_region(&self) -> &ImageRegion {
        let Self {
            rollback_region, ..
        } = self;

        rollback_region
    }

    /// Return the immutable region containing the architectural exception table.
    ///
    /// The table is mapped read-only and contains field-relative [`binding::mirilla_except_record`]
    /// entries. Each populated record relates a protected instruction range to one rollback address
    /// and the set of architectural exception vectors accepted for that range.
    #[inline]
    pub const fn except_table(&self) -> &ImageRegion {
        let Self { except_table, .. } = self;

        except_table
    }
}

/// The process-global result of constructing the exception image.
///
/// [`Image::setup`] is the only initialization path. The first construction result is retained for
/// the lifetime of the process, so the C constructor is invoked exactly once.
static IMAGE: OnceLock<Result<Image, binding::catalejo_outcome_t>> = OnceLock::new();
