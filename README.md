# Catalejo

[![license](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=flat-square)](#license)
[![github actions](https://img.shields.io/github/actions/workflow/status/wafkse/catalejo/ci.yml?branch=development&style=flat-square&logo=github)](https://github.com/wafkse/catalejo/actions)

Fault-protected, zero-copy process memory access for Linux.

Catalejo provides a Rust interface backed by Mirilla, its bundled Linux kernel module, to map target process memory directly into an observer's address space.

It provides:

- Zero-copy access to process memory through locally mapped pages, without repeated cross-process copies.
- Fault-protected reads and writes that report inaccessible memory instead of terminating the observer.
- Automatic mapping invalidation using Linux MMU interval notifiers.
- Typed memory access with explicit validity and coherence guarantees.
- Memoized, rebased, and LRU-managed memory windows.
- Live process inspection without suspending the target.

**Table of Contents:**

- [Usage](#usage)
  - [Basic usage](#basic-usage)
  - [Inspect another process](#inspect-another-process)
  - [Memory windows](#memory-windows)
  - [Memory access guarantees](#memory-access-guarantees)
  - [Performance](#performance)
  - [Testing](#testing)
- [Installation](#installation)
  - [Requirements](#requirements)
  - [From source](#from-source)
- [Known limitations](#known-limitations)
- [Related Projects](#related-projects)
- [License](#license)

## Usage

### Basic usage

Read a `u64` from the current process through a memoized memory window:

```rust
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

`Target::engage` establishes access to the target process. The memory manager resolves and caches windows, while `Foreign::read` performs fault-protected access to the mapped memory.

### Inspect another process

To inspect a different process, pass its PID to `Target::engage` and use addresses from its virtual address space.

Accessing another process requires `CAP_SYS_PTRACE` and permission to open the Catalejo kernel device.

Self-observation does not require `CAP_SYS_PTRACE`.

The target continues running during observation. Catalejo does not suspend the process or provide a consistent snapshot.

### Memory windows

Catalejo exposes target memory through revocable mappings called *peepholes*.

Each peephole maps a page-aligned region of a target process into the observer's address space. The mapping initially contains no resident aliases.

On first access, the in-tree kernel module resolves the target address and maps the corresponding physical frame into the observer. Subsequent resident accesses use local memory operations.

When the target changes its mappings, MMU interval notifiers trigger invalidation of the observer's aliases.

Catalejo provides three window managers:

| Manager | Behavior |
| --- | --- |
| `Memoize` | Caches page-aligned windows for reuse. |
| `Rebased` | Uses fixed-size windows and a second offset grid to support accesses crossing granule boundaries. |
| `Lru` | Extends rebased placement with bounded window caching. |

`Rebased` and `Lru` default to 2 MiB granules.

### Memory access guarantees

Catalejo exposes typed memory operations with different guarantees:

| Operation | Contract |
| --- | --- |
| `Foreign::read` | Fault-protected, machine-word-coherent reads of naturally aligned `Faultable` values. |
| `Foreign::write` | Fault-protected writes that report whether the store completed. |
| `Foreign::copy` | Copies `Unassociated` values, potentially observing torn data during concurrent writes. |

`Foreign::write` is enabled by the default `write` feature.

Typed access is constrained by validity requirements. `Unassociated` excludes types that cannot safely be reconstructed from arbitrary bit patterns, including references and owning pointers.

No operation establishes synchronization with the target process.

### Performance

Cached window resolution remains in userspace. Opening a new window requires an `ioctl` and `mmap`, followed by an initial page fault when the mapping is accessed.

A reference benchmark on a Ryzen 7 7700X with DDR5-6000 CL30 and Linux 7.1.3 produced these medians:

| Operation | Self target | Foreign target |
| --- | ---: | ---: |
| Cached resolution | 12.98 ns | 12.97 ns |
| Resident protected read | 4.12 ns | 4.12 ns |
| Cached resolution + read | 15.39 ns | 15.34 ns |
| Fresh window open | 3.55 µs | 4.01 µs |
| Fresh window + first read | 8.37 µs | 8.28 µs |

Resident-read timings exclude the initial cost of opening and faulting in a window.

Measurements depend on hardware, kernel version, and workload. Full Criterion results are available in [docs/benchmarks](docs/benchmarks).

### Testing

Catalejo includes Rust tests, userspace C tests, kernel KUnit tests, and device-backed integration tests.

```sh
just test       # Host-side checks
just kunit      # Kernel KUnit tests in a VM
just test-vm    # Kernel ABI and Rust integration tests
```

The VM recipes use `virtme-ng` and a prepared Linux kernel tree. KUnit requires `CONFIG_KUNIT=y`.

[GitHub Actions](https://github.com/wafkse/catalejo/actions) runs Rust and C checks alongside kernel and userspace integration tests.

## Installation

### Requirements

Catalejo currently targets Linux `x86_64`.

Building the project requires:

- Stable Rust and Cargo.
- A C toolchain, Make, binutils, and kmod.
- `libclang`.
- Linux kernel headers matching the target kernel.
- A kernel supporting loadable modules.

Running Catalejo requires permission to access the device exposed by its kernel module.

### From source

Clone the repository:

```sh
git clone https://github.com/wafkse/catalejo.git
cd catalejo
```

Build the included kernel module and userspace components:

```sh
export KDIR="/lib/modules/$(uname -r)/build"

make -C mirilla KDIR="$KDIR" module test
cargo build --workspace
```

Load the module:

```sh
sudo insmod mirilla/mirilla.ko
```

The module normally exposes `/dev/mirilla` through devtmpfs.

To unload it:

```sh
sudo rmmod mirilla
```

Secure Boot or kernel module signature enforcement may require signing the module.

## Known limitations

- Only Linux `x86_64` is currently supported.
- The in-tree kernel module must be built and loaded before device-backed access.
- Catalejo does not create atomic snapshots of target processes.
- Concurrent writes can produce inconsistent or torn observations.
- Access does not establish synchronization with the target process.
- Foreign-process inspection requires appropriate tracing and device permissions.

## Related Projects

- [Ganymede](https://github.com/wafkse/ganymede) — Process memory and binary analysis toolkit using Catalejo for live memory inspection.

## License

Copyright (C) 2026 W. Frakchi.

Catalejo is free software. You can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version.

Catalejo is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY,
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Catalejo.
See [`LICENSE`](LICENSE) for the full text. If you did not receive a copy, see the
[GNU licenses page](https://www.gnu.org/licenses/).
