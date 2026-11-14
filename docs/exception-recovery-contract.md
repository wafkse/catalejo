# Exception recovery: internal contract and caller footguns

Catalejo uses Mirilla to recover selected userspace architectural exceptions without a userspace signal-handler round trip. This document records the intentional constraints of the internal Catalejo–Mirilla ABI and the mistakes a caller can make when using the lower-level exception-table API. These are **caller contracts**, not necessarily kernel defects.

The primary consumer is Catalejo. This is not a general-purpose, stable, multi-library exception-registration API.

## How recovery is installed

1. Protected naked assembly routines emit field-relative entries in the `catalejo_fault_fixup` ELF section. Each entry identifies a half-open instruction interval, a rollback address, and an exception-vector mask.
2. The userspace backend resolves those entries into fixed-size `mirilla_except_record` records in a writable slab and sorts them.
3. A **whole-slab** `mprotect(PROT_READ)` transition makes Mirilla invalidate writable PTEs, copy and validate the slab, and publish an immutable kernel-owned snapshot. Publication failures leave the slab editable.
4. The matching `mprotect(PROT_READ | PROT_WRITE)` transition **revokes** the published table before editable access resumes. Unmapping a slab also revokes its publication.
5. On a supported userspace trap, Mirilla intercepts the kernel’s `fixup_vdso_exception()` path, matches the faulting RIP against published records for the current `mm_struct`, applies the action, or falls through to ordinary Linux handling if no action handles the fault.

The C backend is initialized once per process and keeps its built-in publication alive for the process lifetime. Fork children inherit independent slab storage and immutable publications when fork-side allocation succeeds.

## Caller errors (PEBKAC cases)

### 1. Executing protected instructions before publication, or after revocation

The presence of linker metadata does not register a recovery handler. A protected access requires the corresponding table to have been **successfully published in the executing address space**. Calling a protected routine before initialization, or allowing threads to continue using it after `Slab::edit()`, `munmap()`, or dropping its owning slab, can result in ordinary Linux exception delivery.

**Caller rule:** Publish before making the protected routines reachable, and keep the publication alive for as long as any thread might execute those routines. Quiesce users before revocation or replacement. This is a lifetime contract, not a missing atomic table-replacement feature.

### 2. Treating overlapping predicates as a validation failure or an automatically defined priority

Within one slab, used records must form a sorted prefix; distinct instruction intervals cannot overlap. Up to 16 records may have **exactly the same interval**. Across published slabs, even partially overlapping intervals are rejected, independent of the exception predicates.

For exact-boundary duplicates, lookup walks records **backward** and selects the first matching predicate in that traversal (the last matching record in stored order). The validator does not require predicates in a group to be disjoint. In particular, a matching `NONE` action stops selection and delegates to native handling; it does not try an earlier record.

**Caller rule:** Prefer mutually exclusive predicates for the same interval. If overlapping predicates are intentional, control and document the resulting record order; do not assume the kernel will infer the most specific match. The built-in loader sorts by boundary, size, and vector mask, not by predicate specificity.

### 3. Retrying a fault that cannot make progress

`MIRILLA_EXCEPT_ACTION_RETRY` leaves the saved RIP unchanged and reports the exception as handled. It does **not** resolve a page fault, change permissions, advance execution, or impose a retry limit. If the cause persists, the instruction may fault indefinitely.

**Caller rule:** Use `RETRY` only when some independently established condition makes progress possible. Otherwise use an `IP` rollback action or leave the exception to normal Linux handling. Catalejo’s built-in read/write recovery uses rollback IPs.

### 4. Assuming record validation proves the recovery code is correct

Mirilla validates record shape, supported vectors, address ranges, action encoding, ordering, and overlaps. For an `IP` action it checks that the target is a nonzero userspace address via `access_ok()`, **not** that it is executable, a valid instruction boundary, within the expected function, or ABI-safe. Redirecting into arbitrary code may corrupt register state or crash the process.

**Caller rule:** Keep protected instructions and recovery labels in carefully controlled assembly, preserve the calling convention, and ensure a recovery path can be entered with the machine state left by any instruction in the protected interval. Avoid broad protected ranges whose individual instructions have incompatible partial effects.

### 5. Assuming every exception is recoverable

The ABI supports only x86 `#UD` (6), `#GP` (13), `#PF` (14), and `#AC` (17). Each record handles only the vectors in its mask. Error-code filtering is supported only on `#PF`-only predicates and uses the defined page-fault error-code bits. Built-in ordinary memory reads/writes intentionally do not catch `#UD`.

**Caller rule:** Treat a table miss, an unsupported vector, a non-matching error code, or an unhandled action as normal Linux exception handling. An exception table is not a blanket crash suppressor.

### 6. Creating independent exception families in the same address space

Mirilla intentionally allows one exception context family per `mm_struct`. Creating a second one fails with `-EEXIST`. This is a constraint of the **internal** ABI rather than a demand that Mirilla itself become a composable public registration service.

**Caller rule:** Catalejo-owned consumers should share the existing context/backend and arrange any additional slab allocation at the userspace library layer. Do not independently call the context-creation ioctl from unrelated components of the same process.

### 7. Forking while a different thread is initializing the singleton

The built-in C backend has `UNINITIALIZED`, `INITIALIZING`, and `INITIALIZED` states. A competing initializer spins with `sched_yield()` while the state is `INITIALIZING`. If a multithreaded process forks during another thread’s initialization, the child can inherit `INITIALIZING` without inheriting the thread that would complete it. If the child subsequently calls initialization, it can spin forever.

This does **not** mean the initialization code calls `fork()`; it is a caller-created fork/initialization race, and is outside the intended initialized-before-fork lifecycle. It also intersects with the usual restrictions on work performed in the child of a multithreaded `fork()` before `exec()`.

**Caller rule:** Complete backend initialization before forking, or serialize fork against initialization. Once initialized, the built-in publication is explicitly designed to be inherited by fork children without reinitialization. Do not treat this scenario as an observed failure of normal fork inheritance.

## Intentional kernel and deployment constraints

- **Linux x86-64 only.** Exception vector IDs and `pt_regs` handling are architecture-specific.
- **Kernel-internal interception is deliberate.** Mirilla relies on a working ftrace `IPMODIFY` hook on `fixup_vdso_exception()`. This is essential to the chosen design, but makes kernel-version compatibility and conflicts with other IP-modifying instrumentation deployment concerns. It is not, by itself, a correctness defect.
- **Bounded resource model.** Records are 48 bytes each; the default 12 KiB slab holds 256 records. Slabs are capped at 2 MiB, with at most 16 per address space. Duplicate-boundary groups are limited to 16 records.
- **No automatic repair of faulting memory.** The extable layer only chooses whether and where execution resumes. It does not guarantee that an access completes or that target-process memory is coherent.

## Operational invariants worth testing

- Publication never exposes a partially copied, writable, or unvalidated table.
- Revocation prevents new readers from discovering the table, while readers already holding references can finish safely.
- Concurrent publications cannot expose overlapping instruction intervals.
- Lookups are isolated by `mm_struct`, and a miss preserves native exception handling.
- A child never inherits PTEs into the parent’s private slab storage; failed child allocation leaves a fail-closed VMA (`SIGBUS` on access).
- A successfully initialized parent’s built-in backend remains usable in the child after fork.

These invariants are more relevant to a kernel correctness review than demanding atomic hot-swaps or independently created exception families from an intentionally internal interface.

## Source map

- Built-in assembly and rollback records: [`catalejo-sys/c/src/catalejo-fault.c`](../catalejo-sys/c/src/catalejo-fault.c), [`catalejo-sys/c/include/catalejo-fixup.h`](../catalejo-sys/c/include/catalejo-fixup.h).
- Loader and singleton state: [`catalejo-sys/c/src/catalejo-except.c`](../catalejo-sys/c/src/catalejo-except.c).
- ABI declarations: [`mirilla/include/mirilla-except.h`](../mirilla/include/mirilla-except.h).
- Table validation, conflicts, registry, and lookup: [`mirilla/src/mirilla-except-table.c`](../mirilla/src/mirilla-except-table.c).
- Exception interception: [`mirilla/src/mirilla-except.c`](../mirilla/src/mirilla-except.c).
- Slab publication and fork lifecycle: [`mirilla/src/mirilla-slab.c`](../mirilla/src/mirilla-slab.c).
- Rust wrapper: [`catalejo-sys/src/exception.rs`](../catalejo-sys/src/exception.rs).
- Tests: [`mirilla/test/test-exception.c`](../mirilla/test/test-exception.c), [`mirilla/test/kunit/test-exception.c`](../mirilla/test/kunit/test-exception.c).

