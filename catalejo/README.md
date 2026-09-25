# `catalejo`

`catalejo` is the high-level userspace interface to the Mirilla kernel module.
It lets a Linux `x86_64` process observe another process through revocable
peephole mappings while keeping memory access fault protected.

The target keeps running while it is observed. Catalejo therefore does not
provide a process snapshot or make related reads atomic. Callers should choose
an access strategy whose consistency guarantees match the data being observed.

## Core model

A [`Target`](crate::target::Target) represents an engaged process. It owns the
Mirilla session state needed to create peepholes and retains the fault backend
used by protected accesses.

A [`Peephole`](crate::peephole::Peephole) maps a fixed target virtual address
range into the observer. The mapping can become invalid when the target unmaps
or replaces that range. [`Foreign`](crate::peephole::Foreign) carries a
peephole together with a checked offset and type so later accesses preserve the
range and alignment established when the handle was created.

Most callers create peepholes through a [`Manage`](crate::manage::Manage)
implementation.

- [`Memoize`](crate::manage::Memoize) opens tight page-aligned windows around
  requested spans. It is the preferred manager when a process observes itself.
- [`Rebased`](crate::manage::Rebased) uses fixed granule-sized windows and a
  second shifted grid for values that would otherwise straddle a granule
  boundary.
- [`Lru`](crate::manage::lru::Lru) uses the same placement model as `Rebased`
  while bounding the manager-owned set of strongly retained peepholes.

## Example

The following example reads a `u64` from the current process through a
`Memoize` manager. Running it requires a loaded Mirilla module and access to
the configured device.

```no_run
use catalejo::{
    address::ViAddr,
    manage::{Manage, Memoize},
    target::Target,
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let value = Box::new(0xDEAD_BEEF_CAFE_BABE_u64);

    let process_id = std::process::id().try_into()?;
    let target = Target::engage(process_id)?;
    let manager = Memoize::new(target);

    let address = core::ptr::from_ref(value.as_ref()).addr();
    let address = ViAddr::new(address.try_into()?);

    let access = manager
        .source::<u64>(address)?
        .ok_or_else(|| std::io::Error::other("manager cannot serve this address"))?;
    let foreign = access
        .foreign()
        .ok_or_else(|| std::io::Error::other("peephole was invalidated"))?;

    assert_eq!(foreign.read(), Some(*value));

    Ok(())
}
```

For a foreign process, pass its PID to
[`Target::engage`](crate::target::Target::engage) and use virtual addresses from
that process.

## Access semantics

[`Foreign::read`](crate::peephole::Foreign::read) is available for
[`Faultable`](catalejo_fault::behavior::Faultable) values. It performs a
fault-protected, machine-word-coherent read and returns `None` when the access
faults.

[`Foreign::copy`](crate::peephole::Foreign::copy) supports larger
[`Unassociated`](crate::offset::Unassociated) values. A concurrent writer may
tear the copied value across fields or bytes, so this operation does not create
a coherent snapshot.

The `write` feature adds
[`Foreign::write`](crate::peephole::Foreign::write). The store is fault
protected and reports whether it completed.

Peephole invalidation prevents later protected operations from silently reading
a stale mapping. It does not suspend the target or establish synchronization
with target threads.

## Typed traversal

[`Pointer`](crate::pointer::Pointer) models an address stored in the target
without turning it into a local Rust pointer. [`Field`](crate::offset::Field)
and [`Foreign::project`](crate::peephole::Foreign::project) provide typed
in-structure traversal while preserving the bounds and alignment of the
original foreign handle.

The derive macro re-exported as [`Field`](crate::offset::Field) can generate
field marker types for ordinary named structures. Sparse layouts can instead
use [`Sparse`](crate::offset::Sparse) and resolve the resulting access through
the active manager.

## Features

- `default-device-path` is enabled by default and lets
  [`Target::engage`](crate::target::Target::engage) use the configured Mirilla
  device path. Without it, use
  [`Target::engage_at`](crate::target::Target::engage_at) or
  [`Target::engage_with`](crate::target::Target::engage_with).
- `write` is enabled by default and enables protected writes through
  [`Foreign::write`](crate::peephole::Foreign::write).
- `stealth-mode` selects the matching stealth behavior in the lower-level
  Catalejo crates.

## Platform requirements

The crate currently targets Linux on `x86_64` and requires the Mirilla kernel
module. Observing another process also requires the permissions needed by
Mirilla, including `CAP_SYS_PTRACE` where applicable. Self-observation does
not require that capability.

The workspace README contains module setup, kernel build, testing, and benchmark
instructions.
