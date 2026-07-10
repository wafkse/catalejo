# catalejo

Catalejo observes the memory of another process through the `mirilla` kernel module.

A privileged process engages a target, opens peepholes over ranges of the target's address space, and reads that memory through a local mapping. The reads are machine-word coherent and fault-protected. When the target unmaps or remaps the range the peephole goes dead and the access returns nothing rather than crashing the observer.

This is a work in progress. It runs on Linux and `x86_64`.

## Why it exists

Reading another live process' memory is normally done with `ptrace` or `process_vm_readv`. Both copy bytes across the kernel boundary on every access. A single read is a system call and a memcpy, and observing a region at a high rate means paying that toll again for every word. The observer never sees the target's memory directly. It sees a snapshot the kernel assembled for it a moment ago.

Catalejo removes the copy and the per-read call. Rather than asking the kernel to fetch bytes, it asks `mirilla` to alias the target's physical frames into the observer's own address space. Once a page is aliased, reading it is a plain load instruction against the observer's page table, and that page table points at the very same physical memory the target is using. There is no buffer in between and no call per read. The cost is paid once when a window opens and once more when a page is first touched, and every read after that is a bare, cache-resident load.

Aliasing live memory raises two hazards, and the design answers each one.

The first hazard is tearing. The target keeps running and may mutate a value while it is read. Catalejo does not lock the target, because locking a foreign address space to read it defeats the purpose. Instead it leans on the hardware. A read of a naturally aligned machine word is serviced by the memory subsystem as one indivisible transaction, so the observed value is the exact state before or after a concurrent write and never a hybrid of the two. `catalejo-memory` formalizes this into a coherence model, and only types that are valid for every bit pattern, marked `Unassociated`, may cross a peephole.

The second hazard is disappearance. The target may unmap the range, remap it elsewhere, or exit while the observer holds an alias to it. A stray load against freed memory would fault the observer. Catalejo makes that fault survivable. Every read runs through a naked assembly routine whose faults are caught by a chaining signal handler and reported as an absent read, so a dead peephole yields nothing instead of a crash. The kernel side closes the same gap from below. An MMU notifier registered on the target tears the alias down the instant the target changes its mapping, so the window is marked dead before a torn or stale frame can be observed.

## How a read works

A peephole is a window the kernel opens from the observer onto a frame of the target's address space. A frame is a target address quantized by the granule size, analogous to a page frame number, and it names the granule-sized window an address falls into. Reading through a peephole is three layered costs, each paid lazily and each paid once.

- **Resolution.** An observed address is quantized to its frame and resolved against the observer's window table, a sharded map keyed by frame. The lookup is a shift, a mask, and a map hit, and it is memoized, so a repeated read of the same granule never re-resolves and never calls into the kernel.
- **Opening.** The first read of a fresh frame pays one `ioctl` to register the window and one `mmap` to place it in the observer's address space. A granule spans many pages, so this cost is amortized across every page the window covers. Subsequent reads of that granule reuse the mapping.
- **Fault-in.** The first read of a window page installs its alias. Every later read of that page is resident and faults nothing.

Two addresses in the same window share a frame and reuse a single peephole. Because address space layout randomization places a structure at an arbitrary offset within the granule tiling, a structure can straddle a granule boundary and fall across two windows. A `Rebased` manager therefore keeps a second grid shifted by a half granule, so a straddling access is served whole from the shifted window rather than from two halves.

## How pages are initialized

Opening a peephole does not pin any memory. The `mmap` produces a `VM_MIXEDMAP` VMA with no pages behind it, so the window costs address space and a little bookkeeping rather than resident frames. Pages are installed one at a time, on demand, the first time the observer reads them.

The install happens in the module's page-fault handler.

- The handler turns the faulting page offset back into a target virtual address and rejects the fault early when the peephole is already dead or the address is out of the window's bounds.
- It samples the target's MMU interval notifier so it can detect an invalidation that races the fault before a page is installed.
- It pins the target's page with `get_user_pages_remote` against the target's `mm_struct`. This walks the target's own page tables and faults the target's page in if the target had not touched it yet, so the observer never invents a frame the target does not have. A self-peephole reuses the `mmap_lock` the outer fault already holds, and a foreign peephole takes the target's `mmap_lock` under an `mmget` so the address space cannot be torn down mid-fault.
- Under an install lock it re-checks the interval notifier. If the target invalidated the range while the page was being pinned, the fresh pin is already stale, so the handler drops it and re-faults rather than install a frame the target has moved on from.
- Otherwise it installs the target frame's page number into the observer's page table with `vmf_insert_mixed`. The observer's page-table entry now aliases the target's physical frame, and the transient reference taken to read the frame number is dropped because the entry itself keeps the frame reachable.

After the entry is installed, the observer reads that page as ordinary memory. The load resolves through the resident entry straight into the shared frame, with no fault, no call, and no copy.

Teardown runs from the same notifier. When the target unmaps, remaps, or frees the range, the kernel invokes the interval notifier's invalidate callback, which marks the peephole dead, advances the notifier sequence under the install lock, and zaps the installed aliases. A later fault against a dead peephole returns a bus error, which is exactly the fault `catalejo-fault` catches and reports as an absent read.

## Speed

The layered costs are arranged so that the expensive ones are rare and the common one is trivial. Resolution is a memoized frame lookup with no system call. Opening is one `ioctl` and one `mmap` amortized across a whole granule. A fault-in is a single minor page fault that installs a shared frame. A hot read is then a load through a resident entry that aliases the target's frame, so it runs at memory speed rather than syscall speed.

The default granule is a two-megabyte huge frame. A large granule keeps the window table small and amortizes the open across the many pages it covers, and the granule size is a power of two, so quantizing an address to its frame is a single shift. The `self_peephole` benchmark suite isolates each of these costs by engaging the benchmarking process as its own target, so no second process is needed. It reads a machine word out of a peephole that points back into the reader.

| Benchmark          | Group     | Isolates                                                          |
| ------------------ | --------- | ---------------------------------------------------------------- |
| `resolve-only`     | resolve   | The memoized window lookup, no read.                             |
| `open-cold`        | resolve   | A fresh window's `ioctl` and `mmap`, no read.                    |
| `read-hot`         | read      | The fault-protected read alone, over a resident page.            |
| `resolve-read-hot` | read      | The whole hot path, a memoized lookup then a resident read.      |
| `source-read-warm` | read      | The find-or-open path over an already-open, resident window.     |
| `open-read-cold`   | read      | A cold open plus the first read that faults the new page in.     |

The `read` group declares a per-iteration throughput of one machine word, so criterion reports its figures as read speeds in GiB/s alongside the latencies.

![self-peephole read speeds](docs/benchmarks/self-peephole-read.svg)

The plot above is criterion's violin of the read group. It is refreshed from `target/criterion` by `just catalejo-bench-plot` after a benchmark run, and the benchmark CI job uploads the full HTML report as an artifact per kernel.

## Architecture

The kernel module lives in `mirilla`. The userspace side is a Rust workspace of five crates.

| Crate             | Responsibility                                                                                     |
| ----------------- | -------------------------------------------------------------------------------------------------- |
| `catalejo`        | The high-level API of `Target`, `Peephole`, `Foreign`, and the memoizing `Manage` window table.    |
| `catalejo-memory` | The hardware coherence model, machine-word, snapshot, and mixed coherence, and the `Unassociated` contract. |
| `catalejo-fault`  | Fault-protected reads, on a naked assembly routine and a chaining `SIGSEGV`/`SIGBUS` handler.       |
| `catalejo-sys`    | The bindings to the `mirilla` ioctl ABI.                                                            |
| `catalejo-macro`  | Derive macros that reify a type's fields for offset resolution and emit its `Unassociated` proof.   |

## Requirements

- Linux on `x86_64`, with the `mirilla` module loaded.
- `CAP_SYS_PTRACE` to engage a target. This is the same capability that already grants `process_vm_readv` over arbitrary processes, so it grants no new access scope.
- A Rust toolchain for the workspace, and kernel headers plus `clang`/`libclang` for the `catalejo-sys` bindgen build.

## Building

Build the workspace with Cargo and the module with the kernel build system.

```sh
cargo build --workspace
make -C mirilla module
```

## Testing

The suites read a live `/dev/mirilla`, so they run inside a [`virtme-ng`](https://github.com/arighi/virtme-ng) VM that boots a mirilla-powered kernel. Every task is a [`just`](https://just.systems) recipe, and `just --list` shows them grouped by sub-module.

```sh
just lint              # rustfmt + clippy, and clang-format + clangd
just mirilla-test-vm   # build the module and run the C test suite in a VM
just catalejo-test-vm  # build the module and run the Rust test suite in a VM
just catalejo-bench-vm # build the module and run the benchmarks in a VM
```

The `*-vm` recipes are host-side, and build what the guest needs before launching the VM against the kernel tree named by `KDIR` (defaulting to the running kernel's build directory). The plain `mirilla-test`, `catalejo-test`, and `catalejo-bench` recipes are the guest-side halves, run as root inside the VM against a loaded module.

## Continuous integration

Five jobs run on every push and pull request.

- **Automated Linting (Rust)** runs `cargo fmt` and `clippy`.
- **Automated Linting (C)** runs `clang-format` and `clangd`.
- **Mirilla-powered Kernel Test Suite** runs the C suite, once per kernel series.
- **Mirilla-powered Kernel Integration Tests** runs the Rust test suite, once per kernel series.
- **Mirilla-powered Kernel Benchmark** runs the self-peephole benchmarks, once per kernel series.

The mirilla-powered jobs run against the latest longterm and latest stable kernel series.

## License

GPL-3.0-or-later. See the LICENSE file.
