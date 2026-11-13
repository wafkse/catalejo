# `catalejo-fault`

`catalejo-fault` lets a Linux `x86_64` process attempt primitive memory accesses
that may fault. Its linked routines use Mirilla recovery records to return
`None` for a failed read or `false` for a failed write, allowing the caller to
handle inaccessible memory without terminating the observer.

These routines accept [`Faultable`](crate::behavior::Faultable) values, whose
representations support naturally aligned, machine-word-coherent accesses.
Unlike `Unassociated`, `Faultable` requires a padding-free representation with
every byte initialized because `equal()` compares raw object representations.
Although an individual access does not tear, successive reads can observe
different target states because the routines do not synchronize with the writer.

Recovery applies to the linked access routines, so using this crate does not
install a general exception handler for application code. Applications observing
another process normally use [`catalejo`](../catalejo), which manages the target
mappings and initializes the fault backend on their behalf.

See the [workspace README](../README.md) for Mirilla setup and permissions.
