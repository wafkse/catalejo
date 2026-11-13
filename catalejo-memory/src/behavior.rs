//! Value validity requirements for potentially torn memory observations.

/// A fixed-size type with no borrowed lifetime dependencies.
pub trait Immortal: Sized + 'static {}

/// Fixed-size static types satisfy the lifetime requirement.
impl<I> Immortal for I where I: Sized + 'static {}

/// A value that remains valid when observed as arbitrary, potentially torn bytes.
///
/// When another process changes memory during a copy, the result can combine
/// bytes from different updates. Implementing this trait asserts that even
/// those combinations are valid values, allowing access routines to construct
/// the result without validating its bit pattern.
///
/// # Notes
///
/// Value validity does not establish that fields agree or that the copy was
/// atomic. Those guarantees depend on the access routine and hardware, including
/// the access width and alignment, and must be established separately.
///
/// # Safety
///
/// For soundness purposes, the following must be satisfied:
///
/// * Every bit pattern in value-bearing fields must represent a valid value
///   without violating a safety invariant, including patterns assembled by torn reads.
/// * Conversion to bytes and back must preserve value validity.
/// * Value-bearing fields must be initialized. Padding is permitted and need not
///   be initialized. This trait does not permit reading padding as initialized bytes.
///
/// Types with restricted bit patterns, such as `bool` and enums with invalid
/// discriminants, cannot satisfy this contract. A representation attribute on
/// an enum does not by itself make every discriminant valid.
///
/// # Interpreting observations
///
/// Alignment and access width affect whether a read can tear. Even if individual
/// fields are read without tearing, their values may come from different updates.
/// Validate any relationships required by the application before using the data.
///
/// Uninitialized value-bearing storage does not satisfy this contract even when
/// its inner type does.
///
/// ```compile_fail
/// use core::mem::MaybeUninit;
/// use catalejo_memory::behavior::Unassociated;
///
/// fn require_unassociated<T: Unassociated>() {}
///
/// require_unassociated::<MaybeUninit<u64>>();
/// ```
pub unsafe trait Unassociated: Immortal + Copy {}

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

// SAFETY: 32-bit floats possess no safety-related invariants. All NaN encodings are valid floating-point values.
unsafe impl Unassociated for f32 {}

// SAFETY: 64-bit floats possess no safety-related invariants. All NaN encodings are valid floating-point values.
unsafe impl Unassociated for f64 {}

// SAFETY: The element type `U` implements `Unassociated`, therefore, a const-generic array does too.
unsafe impl<U, const N: usize> Unassociated for [U; N] where U: Unassociated {}

// SAFETY: A thin raw pointer has no illegal bit patterns, as every bit pattern is a valid
// pointer value, it is byte-transmutable, and a torn read still yields a valid (if mangled and
// meaningless) address that is never dereferenced. The `P: Immortal` bound supplies
// `Sized` (the pointer is thin, a single machine word) and `'static` (satisfying the
// `Immortal` supertrait of `Unassociated`).
// NOTE(warning): This is to be used as an escape hatch to model pointers in a foreign address space.
unsafe impl<P> Unassociated for *const P where P: Immortal {}

// SAFETY: A thin raw pointer has no illegal bit patterns, as every bit pattern is a valid
// pointer value, it is byte-transmutable, and a torn read still yields a valid (if mangled and
// meaningless) address that is never dereferenced. The `P: Immortal` bound supplies
// `Sized` (the pointer is thin, a single machine word) and `'static` (satisfying the
// `Immortal` supertrait of `Unassociated`).
// NOTE(warning): This is to be used as an escape hatch to model pointers in a foreign address space.
unsafe impl<P> Unassociated for *mut P where P: Immortal {}
