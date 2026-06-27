# catalejo

Catalejo observes the memory of another process through the `mirilla` kernel module.

A privileged process engages a target, opens peepholes over ranges of the target's address space, and reads that memory through a local mapping. The reads are machine-word coherent and fault-protected: when the target unmaps or remaps the range the peephole goes dead and the access returns nothing rather than crashing the observer.

The kernel module lives in `mirilla`. The userspace side is split into four crates. `catalejo-memory` formalizes the hardware coherence model. `catalejo-fault` performs the fault-protected reads on top of a naked assembly routine and a chaining signal handler. `catalejo-sys` binds the `mirilla` ioctl ABI. `catalejo` is the high-level API of `Target`, `Peephole` and `Foreign`.

It runs on Linux and x86_64, needs the `mirilla` module loaded, and `CAP_SYS_PTRACE` to engage a target. Build the workspace with `cargo build` and the module with `make -C mirilla`.

This is a work in progress.

## License

GPL-3.0-or-later. See the LICENSE file.
