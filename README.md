# Catalejo

Catalejo gives a Linux process a fault-protected, zero-copy view into another process' memory. The
Mirilla kernel module aliases target pages into the observer, so resident reads are local loads rather
than a syscall and copy per access.

The target keeps running while it is observed. Catalejo does not provide a process snapshot or make
multi-field reads atomic.

Catalejo is under active development and currently targets Linux on `x86_64`.

## Requirements

- stable Rust
- a Linux `x86_64` kernel with module support and matching build headers
- a C toolchain, `libclang`, Make, `binutils`, and `kmod`
- permission to open the Mirilla device
- `CAP_SYS_PTRACE` when observing another process

Self-observation does not require `CAP_SYS_PTRACE`.

## Setup

Build against the running kernel unless `KDIR` points at another prepared kernel tree.

```sh
export KDIR="/lib/modules/$(uname -r)/build"

make -C mirilla KDIR="$KDIR" module test
cargo build --workspace
sudo insmod mirilla/mirilla.ko
```

Mirilla normally appears as `/dev/mirilla` through devtmpfs. The observer needs access to that device.
For foreign targets, run it with `CAP_SYS_PTRACE`; during development that usually means running as
root or assigning the capability to the built executable.

```sh
sudo setcap cap_sys_ptrace=ep target/debug/observer
```

Secure Boot or module-signature enforcement may require signing `mirilla.ko`. To unload the module:

```sh
sudo rmmod mirilla
```

## Example

This reads a `u64` from the current process through a memoized peephole.

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

For a foreign process, pass its PID to `Target::engage` and use addresses from that process' address
space.

## How it works

A peephole is a revocable mapping of a fixed, page-aligned range in the target. Mirilla exposes the
range through an instance file descriptor, and Catalejo maps that descriptor into the observer.

The mapping starts empty. On first touch, the module resolves the corresponding target address and
installs an alias for the same physical frame. Once resident, reads go through the observer's own page
table.

```text
target virtual range       shared physical frames       observer peephole VMA
[foreign addresses]  --->  [page][page][page]  <---     [local addresses]
```

Mirilla tracks the target mapping with an MMU interval notifier. If the target unmaps, remaps, or
exits, the peephole is invalidated and installed aliases are removed. Catalejo performs accesses
through fault-recovery routines, so invalidation becomes an operation failure instead of a crash in
the observer.

`Foreign<F>` holds the peephole, a checked offset, and the accessed type. Managers choose and reuse
peepholes so repeated reads do not reopen the same target range.

## Memory model

| Operation | Contract |
| --- | --- |
| `Foreign::read` | Fault-protected, machine-word-coherent read of a naturally aligned `Faultable` value. |
| `Foreign::write` | Fault-protected store. Returns whether the store completed. Available through the default `write` feature. |
| `Foreign::copy` | Copies an `Unassociated` value. A concurrent writer may tear the result across fields or bytes. |
| Invalidation | Unmap, remap, and target exit invalidate affected peepholes. Protected operations report failure rather than reading a stale frame. |
| Synchronization | No target suspension and no happens-before relationship with the target. |

`Unassociated` is the type boundary for values that may be observed from arbitrary or torn bytes. It
excludes references, owning pointers, booleans, and enums with invalid bit patterns.

## Window managers

| Manager | Intended use |
| --- | --- |
| `Memoize` | Tight page-aligned windows around arbitrary spans. Windows stay cached for the manager lifetime. |
| `Rebased` | Fixed-granule windows with a second offset grid for values that straddle a granule boundary. |
| `Lru` | `Rebased` placement with bounded strong peephole retention. |

`Rebased` and `Lru` default to a 2 MiB granule. Values served by their two-grid placement must fit
within half of the selected granule.

## Performance

The hot path is intentionally small: cached resolution stays in userspace, a fresh window costs an
`ioctl` plus `mmap`, first touch faults in the alias, and a resident read is a protected local load.

A reference run on a Ryzen 7 7700X with DDR5-6000 CL30 and Linux 7.1.3 produced these medians:

| Operation | Self target | Foreign target |
| --- | ---: | ---: |
| Cached resolution | 12.98 ns | 12.97 ns |
| Resident protected read | 4.12 ns | 4.12 ns |
| Cached resolution + read | 15.39 ns | 15.34 ns |
| Fresh window open | 3.55 µs | 4.01 µs |
| Fresh window + first read | 8.37 µs | 8.28 µs |

These numbers are workload- and machine-dependent. Full Criterion output is under
[`docs/benchmarks`](docs/benchmarks).

## Testing

```sh
just test       # host-safe Rust/C checks
just kunit      # kernel KUnit suite in a VM
just test-vm    # Mirilla ABI + Rust integration tests with the module loaded
```

`KDIR` selects the kernel tree for module and VM builds. `just kunit` needs `CONFIG_KUNIT=y`; the VM
recipes use `virtme-ng`.

To exercise a module already loaded on the host:

```sh
sudo mirilla/test/mirilla-test
cargo test --workspace -- --include-ignored --test-threads=1
```

Foreign-target tests need `CAP_SYS_PTRACE`.

## Stealth mode

Stealth mode builds a seeded release module without project logging, debug metadata, BTF, or stable
project-owned symbol names. The seed determines the module name, parameter name, metadata, and
internal C aliases.

```sh
seed=replace-with-a-release-seed
device_name=example-device
module_name=$(python3 mirilla/tools/mirilla-stealth.py --seed "$seed" --module-name)
module_path="mirilla/$module_name.ko"

make -C mirilla MIRILLA_STEALTH_MODE=1 MIRILLA_STEALTH_SEED="$seed" module

parameter_name=$(modinfo -F parm "$module_path" | cut -d: -f1)
sudo insmod "$module_path" "$parameter_name=$device_name"

cargo build -p catalejo --features stealth-mode --no-default-features
```

Set `MIRILLA_DEVICE_NAME` at module build time to embed a default device name. Without the default
Rust device-path feature, use `Target::engage_at` or `Target::engage_with`.

## Workspace

| Crate | Responsibility |
| --- | --- |
| [`catalejo`](catalejo) | Target engagement, peepholes, managers, typed access, copying, and monitoring. |
| [`catalejo-memory`](catalejo-memory) | Bit-pattern validity and coherence contracts. |
| [`catalejo-fault`](catalejo-fault) | Fault-protected linked access routines. |
| [`catalejo-sys`](catalejo-sys) | Mirilla ABI bindings and low-level system calls. |
| [`catalejo-macro`](catalejo-macro) | Derive support for typed field projection. |
| [`mirilla`](mirilla) | Kernel-side target engagement, page aliasing, and invalidation. |

## License

Licensed under GPL-3.0-or-later. See [`LICENSE`](LICENSE).
