# `catalejo-sys`

The system-level bindings to the `mirilla` kernel module.

Exception recovery uses one fd-owned context per process address space. Mapping the fd allocates a
fixed-size read-write slab. Userspace fills a sorted prefix of 48-byte records and leaves the unused
suffix zero. Changing the whole mapping to read-only publishes an immutable kernel snapshot.
Changing it back to read-write removes that snapshot before editing resumes.

Mirilla implements the fd-backed mapping and publication lifecycle in a reusable slab subsystem.
The exception consumer supplies owner-retention, snapshot-publication, and publication-revocation
operations, while exception-specific validation and lookup remain outside the generic VMA code.

The Rust `exception` module exposes this lifecycle through `Context` and `Slab`. A slab tracks its
current protection state and preserves ownership when a publication or edit transition fails.
Catalejo's own linked fault routines use a PID-aware process `Backend` with one 12 KiB slab. A
forked child receives stale descriptor state and must initialize a fresh backend before protected
access.
