# `catalejo-memory`

`catalejo-memory` defines which values can be copied from memory that may change
during observation. It supplies the type contracts used by Catalejo's access
routines and can be used without the standard library.

## Value validity

[`Unassociated`](crate::behavior::Unassociated) permits observation of arbitrary
bit patterns, including bytes assembled from different moments. Implementing it
requires a fixed-size, `Copy`, lifetime-independent type with no invalid bit
patterns or safety invariants that concurrent mutation could break.

Value-bearing fields must be initialized, but padding is permitted and need not
be initialized. This does not permit reading padding as initialized bytes.
`MaybeUninit` storage does not satisfy the field initialization requirement.
References, owning pointers, `bool`, and enums with invalid discriminants fail
the validity requirements and are unsuitable for the same observation model.

Integers and floating-point primitives satisfy these requirements, as do arrays
of `Unassociated` elements. Thin raw pointers are supported for representing
foreign addresses, but observing their bits gives no assurance that the address
can be dereferenced locally.

## Consistency

A valid value can still be inconsistent with the target's state. For example,
two counters copied together may come from different updates. Applications must
validate relationships between observed fields or use a protocol supplied by
the target.

This distinction is why `Unassociated` supplies no atomicity or synchronization
guarantee. Coherence depends on the access routine, its width, and the address
alignment. For protected primitive accesses, use
[`catalejo-fault`](../catalejo-fault). To apply those accesses to typed target
mappings, use [`catalejo`](../catalejo).
