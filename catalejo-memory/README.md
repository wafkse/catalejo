# `catalejo-memory`

This crate aims to model the underlying memory accesses performed through a kernel-provided peephole. 

It provides a formalized, zero-overhead framework for asynchronous, un-synchronized observation of shared memory regions. By deliberately eschewing locks, mutexes, and sequential consistency barriers, `catalejo-memory` allows software to perform "dirty reads" directly from mapped memory. To achieve this without triggering compiler-level Undefined Behavior (UB) via data races, all accesses are rigorously modeled through relaxed atomic intrinsics, compiling down to raw, barrier-free machine-code instructions.

Because this crate inherently permits concurrent mutation during read operations, callers must possess a comprehensive understanding of hardware-level memory coherence, alignment, and type-level safety boundaries.

## The Coherence Models

When observing memory concurrently without software synchronization, the resulting data is entirely dictated by the physical characteristics of the CPU's memory controller and cache hierarchy. This crate exposes a set of coherence models that must be carefully understood.

### Machine-Word Coherence

Machine-word coherence describes a physical hardware capability. It dictates that the CPU can fetch a specific primitive from the memory bus in a single, un-segmented hardware transaction. 

An access is considered machine-word coherent if and only if:
1. The requested data size is less than or equal to the CPU's native machine word (e.g., 64-bits on `x86_64` or `AArch64`).
2. The memory address is at least aligned to its natural boundary.

### Snapshot Coherence

Snapshot coherence is a stronger, logical guarantee derived from machine-word coherence. It asserts that the resulting value represents the precise, mathematically pure state of the memory at a singular, indivisible point in time, entirely free from temporal tearing. 

While machine-word coherence describes *how* the hardware fetches the data, snapshot coherence describes the *integrity* of the observation. A flawless, instantaneous observation of state is only guaranteed if the physical fetch is indivisible.

### Mixed Coherence (Tearing)

Mixed coherence occurs when a read operation is physically assembled from multiple, sequential hardware bus transactions. If a concurrent writer mutates the shared memory region during these sequential fetches, the resulting value will be a "temporal chimera", a hybrid sequence of bytes where some segments reflect the "old" state of the memory, and others reflect the "new" state.

This crate is explicitly designed to tolerate mixed coherence. Rather than preventing tearing, `catalejo-memory` provides the traits and abstractions necessary to survive it safely.

## The Alignment Trap & Cache-Line Boundaries

Even if a requested primitive is exactly the size of a machine word, snapshot coherence is instantly destroyed if the access is **unaligned**.

Modern CPUs fetch memory into hardware cache lines (typically 64 bytes wide). If software attempts to read a machine-word primitive (e.g., an 8-byte `u64`) from an address that physically straddles the boundary between two adjacent cache lines (e.g., the last 4 bytes of Cache Line A and the first 4 bytes of Cache Line B), the CPU is forced to split the read into two distinct fetches. 

Because time passes between fetching Cache Line A and Cache Line B, a concurrent mutation will result in silent, intra-primitive tearing. Different architectures react to this differently:
* **`x86_64`**: The hardware silently resolves the unaligned access, heavily penalizing performance and yielding a torn, mixed-coherence read.
* **`ARM / RISC`**: The hardware may refuse to split the read entirely, triggering a bus error or alignment fault, resulting in an immediate process crash.

## Type Topology: Single and Multi-Component Types

To properly model accesses through an asynchronous memory peephole, types must be classified by their structural topology. The coherence guarantees provided by the hardware apply drastically differently depending on whether you are observing a single component or a composite structure.

### Single-Component Types
A **single-component type** is a fundamental scalar primitive (e.g., `u32`, `i64`, `f32`). Assuming it meets the strict requirements of machine-word coherence (proper size and natural alignment), a single-component type intrinsically guarantees automatic snapshot coherence. The hardware guarantees you will observe either the exact state before a mutation, or the exact state after it.

### Multi-Component Types
A **multi-component type** is a composite data structure (e.g., a `struct`, a tuple, or an array) comprised of multiple discrete fields. In this model, every primitive field within the composite structure is evaluated as an isolated, independently fetched component.

**Crucially, when observing a multi-component type concurrently without locks, each individual component guarantees automatic snapshot coherence, but the type as a whole does not.**

Fetching a multi-component type fundamentally requires iterating over the structure and performing multiple hardware transactions. Therefore, **whole-type snapshot coherence is physically impossible** under this access model. The caller must explicitly anticipate temporal inconsistencies between components (e.g., `field_A` may be observed in state $T_0$, while `field_B` is observed in state $T_1$). 

## Safety and Type-Level Undefined Behavior

By utilizing relaxed atomics, `catalejo-memory` successfully eliminates **Memory-Level Undefined Behavior** (data races). However, because mixed coherence (tearing) is permitted, the caller is now exposed to **Type-Level Undefined Behavior**.

When a read tears, the resulting permutation of bytes is mathematically arbitrary. In Rust, instantiating certain types with arbitrary bit patterns is immediate, catastrophic UB.
* A torn `bool` may materialize as `0x4A`. Branching on a non `0x00`/`0x01` boolean is UB.
* A torn pointer (`&T`, `Box<T>`) will point to unmapped memory, resulting in a segmentation fault upon dereference.
* A torn `enum` may materialize an illegal discriminant.

To enforce type-level safety, this crate provides the [`Unassociated`] marker trait.

### The `Unassociated` Contract

Types passed through the peephole must implement the [`Unassociated`] trait. This trait mathematically proves that the type is an unassociated (immortal, devoid of lifetimes), trivially-copiable structure that possesses **no restricted safety invariants**. It must be mathematically valid for all possible bit-permutations, ensuring that an arbitrary, mixed-coherence chimera can materialize in memory without violating the Rust compiler's type-safety guarantees.
