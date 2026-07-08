//! Behavior and guarantees provided by types to be in and out of memory.

use core::mem::MaybeUninit;

/// A marker trait that describes an "immortal" type.
///
/// By "immortal", this denotes a type that is fixed in size and has no lifetime dependencies.
pub trait Immortal: Sized + 'static {}

/// Blanket implementation for all "immortal" types.
impl<I> Immortal for I where I: Sized + 'static {}

/// A trait that models an unassociated (i.e., no attached lifetime, immortal), trivially-copiable type that can be read from memory.
///
/// # Hardware Coherence Model
///
/// When dealing with concurrent, un-synchronized memory reads, understanding the physical hardware access model is critical:
///
/// * **Machine-Word Coherence:** The hardware guarantee that a CPU can fetch data up to its native word size (e.g., 64 bits on x86_64) in a single, indivisible memory bus transaction.
/// * **Snapshot Coherence:** A read resulting from a single, indivisible transaction. The resulting value represents the exact state of the memory at an isolated point in time.
/// * **Mixed Coherence (Tearing):** A read assembled from multiple hardware transactions. If another thread mutates the memory between these fetches, the resulting value is a temporal mix of old and new bytes.
///
/// Mixed coherence inevitably occurs under two conditions:
/// 1. The data type exceeds the hardware machine word (e.g., attempting a concurrent read on a 256-byte `struct`).
/// 2. The memory access is **unaligned**. For example, reading an 8-byte `u64` that physically straddles two distinct CPU cachelines forces the hardware to split the read into two distinct fetches, immediately destroying snapshot coherence.
///
/// # Safety
///
/// To be able to implement this trait safely, all the following requirements must have been satisfied:
///
/// * The type is valid and presents no broken safety-related invariants in all possible bit-permutations, as well as be able to survive a round-trip through both bitstream and whole-type states.
/// * The type must be appropriate for snapshot and mixed-coherence reads. Particularly, for a single-component type, it must be safe to read and write from a
///   concurrently mutated memory region where tearing is inherently permitted, without resulting in type-level undefined behavior. For a multi-component type, it may present inter-component tearing, but the individual components still correspond to a valid observed value.
/// * The type is safe to transmute back-and-forth to a byte-level representation.
///
/// As a general rule, do not implement this for types or any primitive that possesses an illegal bit-pattern (e.g., `bool`, or an enum with a non-explicit **repr** with non-exhaustive discriminants).
///
/// # Data Validity
///
/// As a global concern, validate that:
///
/// - The alignment for the type you're reading is correct.
/// - The type itself or individual primitive components do not cross cacheline boundaries.
///
/// If these concerns are not satified, the data remains safe to read, but may prove of little use due to low validity.
pub unsafe trait Unassociated: Immortal + Copy {}

// SAFETY: If `T` implements `Unassociated`, `MaybeUninit` does too.
unsafe impl<T> Unassociated for MaybeUninit<T> where T: Unassociated {}

// SAFETY: Unsigned 8-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for u8 {}

// SAFETY: Signed 8-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for i8 {}

// SAFETY: Unsigned 16-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for u16 {}

// SAFETY: Signed 16-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for i16 {}

// SAFETY: Unsigned 32-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for u32 {}

// SAFETY: Signed 32-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for i32 {}

// SAFETY: Unsigned 64-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for u64 {}

// SAFETY: Signed 64-bit integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for i64 {}

// SAFETY: Pointer-sized unsigned integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for usize {}

// SAFETY: Pointer-sized signed integers possess no safety-related invariants and are valid for all possible bit-permutations.
unsafe impl Unassociated for isize {}

// SAFETY: 32-bit floats possess no safety-related invariants. NaN bit patterns are mathematically valid states.
unsafe impl Unassociated for f32 {}

// SAFETY: 64-bit floats possess no safety-related invariants. NaN bit patterns are mathematically valid states.
unsafe impl Unassociated for f64 {}

// SAFETY: The element type `U` implements `Unassociated`, therefore, a const-generic array does too.
unsafe impl<U, const N: usize> Unassociated for [U; N] where U: Unassociated {}
