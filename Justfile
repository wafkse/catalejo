# Catalejo task runner.
#
# One recipe per unit of the "test ordeal", grouped by the sub-module it drives.
# Those are the Rust workspace, the `mirilla` kernel module, and the `catalejo`
# crate. The recipes replace the former `ci/*.sh` drivers, so continuous
# integration and a local checkout run the exact same commands.
#
# The suites are split the way the code is. `mirilla` carries the C test suite
# and `catalejo` carries the Rust integration tests and benchmarks. Both read a
# live `/dev/mirilla`, so their run recipes are guest-side and expect to execute
# as root inside a `virtme-ng` VM that boots a mirilla-powered kernel. The
# matching `*-vm` recipes are host-side, building what the guest needs before
# launching the VM against it.
# Run each recipe line under a strict shell so a failing command aborts the recipe.

set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

# The kernel tree the module is built against and the VM boots. Defaults to the
# running kernel's build directory, and CI points it at the matrix kernel checkout.

kdir := env("KDIR", "/lib/modules/" + `uname -r` + "/build")

# The built module object the guest loads.

module := justfile_directory() / "mirilla" / "mirilla.ko"

# VM sizing for the guest-side runs. Two gigabytes and four CPUs mirror CI.

vm_memory := env("CATALEJO_VM_MEMORY", "2G")
vm_cpus := env("CATALEJO_VM_CPUS", "4")

# The environment handed to the guest. virtme-ng runs the guest command as root
# with a reset environment, so the host toolchain has to be threaded back in by
# hand. The shared filesystem makes every host path resolve unchanged inside the
# guest, so forwarding the caller's home and cargo locations lets the guest find
# cargo, its rustup toolchain, and the registry cache. The PATH also restores the
# sbin directories a reset root PATH can omit, where insmod and rmmod live.

home := env("HOME")
cargo_home := env("CARGO_HOME", home + "/.cargo")
rustup_home := env("RUSTUP_HOME", home + "/.rustup")
guest_path := cargo_home + "/bin:" + home + "/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# The env prefix that carries that environment across the guest boundary. just is
# invoked by absolute path so it does not depend on the forwarded PATH itself.

guest_env := "env 'HOME=" + home + "' 'CARGO_HOME=" + cargo_home + "' 'RUSTUP_HOME=" + rustup_home + "' 'PATH=" + guest_path + "' '" + just_executable() + "'"

# List the available recipes.
default:
    @just --list

# --- Formatting ---

# Format the entire Rust workspace.
[group('format')]
format-rust:
    cargo fmt --all

# Format every C source and header in the checkout.
[group('format')]
format-c:
    #!/usr/bin/env bash
    set -euo pipefail

    mapfile -d '' sources < <(
        find . -type f \
            \( -name '*.c' -o -name '*.h' \) \
            -not -path './.git/*' \
            -not -path './target/*' \
            -print0 | sort -z
    )

    if [ "${#sources[@]}" -eq 0 ]; then
        exit 0
    fi

    clang-format -i "${sources[@]}"

# Format every language in the checkout.
[group('format')]
format: format-rust format-c

# --- Linting ---

# Check formatting and lint the entire Rust workspace.
[group('lint')]
lint-rust:
    cargo fmt --all --check
    cargo clippy --workspace --all-targets -- -D warnings

# Check C formatting (clang-format) and analyze the userspace TUs (clangd).
[group('lint')]
lint-c:
    #!/usr/bin/env bash
    set -euo pipefail

    # Every tracked C source across the kernel module and the fault subsystem.
    sources=(
        mirilla/src/*.c mirilla/include/*.h
        mirilla/test/*.c mirilla/test/*.h
        catalejo-fault/c/src/*.c catalejo-fault/c/include/*.h
    )

    echo "==> clang-format (${#sources[@]} files)"
    clang-format --dry-run --Werror "${sources[@]}"

    # clangd needs a compile database. The userspace test suite and the fault
    # subsystem build without the kernel, so `bear` captures their commands on a
    # bare host. The in-kernel module sources need kernel headers and are checked
    # when the module is built, so they are left out of this pass.
    echo "==> clangd --check (userspace translation units)"
    database=$(mktemp -d)
    trap 'rm -rf "$database"' EXIT

    bear --output "$database/compile_commands.json" -- make -C mirilla tests >/dev/null

    status=0
    for unit in mirilla/test/*.c catalejo-fault/c/src/*.c; do
        # clangd --check reports its own tweak self-tests as errors, so gate only
        # on genuine clang diagnostics carrying a source position.
        diagnostics=$(clangd --compile-commands-dir="$database" --check="$unit" 2>&1 |
            grep -aE ':[0-9]+:[0-9]+: (error|warning|fatal error):' || true)

        if [ -n "$diagnostics" ]; then
            echo "clangd flagged $unit:" >&2
            printf '%s\n' "$diagnostics" >&2
            status=1
        fi
    done

    exit "$status"

# Run every lint check.
[group('lint')]
lint: lint-rust lint-c

# --- mirilla: the kernel module and its C test suite ---

# Build the mirilla kernel module against the kernel tree at {{kdir}}.
[group('mirilla')]
mirilla-module:
    make -C mirilla KDIR='{{ kdir }}' module

# Build the C test suites. Userspace only, so no kernel tree is needed.
[group('mirilla')]
mirilla-suite:
    make -C mirilla tests

# Run the C test suites against a loaded module. Guest-side, root.
[group('mirilla')]
mirilla-test: _mirilla-load
    #!/usr/bin/env bash
    set -uo pipefail

    # Unload on the way out, dumping the kernel log on failure so a red run
    # carries the module's own view of the fault.
    trap 'code=$?; [ "$code" -eq 0 ] || dmesg | tail -n 100 >&2; rmmod mirilla || true; exit "$code"' EXIT

    echo "Running the mirilla C suites under $(uname -r)"

    suites=(test-suite test-self test-concurrency test-invariants test-ioctl test-layout)
    failed=()

    for suite in "${suites[@]}"; do
        echo
        echo "==> $suite"
        "mirilla/test/$suite" || failed+=("$suite")
    done

    echo
    if [ "${#failed[@]}" -ne 0 ]; then
        echo "FAILED:${failed[*]/#/ }" >&2
        exit 1
    fi

    echo "All mirilla C suites passed."

# Build the module and run the C suites inside a mirilla-powered VM. Host-side.
[group('mirilla')]
mirilla-test-vm: mirilla-module mirilla-suite
    cd '{{ kdir }}' && vng --user root --memory '{{ vm_memory }}' --cpu '{{ vm_cpus }}' -- \
        {{ guest_env }} --justfile '{{ justfile() }}' mirilla-test

# --- catalejo: the Rust integration tests and benchmarks ---

# Build the workspace test binaries and the benchmark binary into the shared target dir.
[group('catalejo')]
catalejo-build:
    cargo test --workspace --no-run
    cargo bench --all --no-run

# Run the whole Rust test suite against a loaded module. Guest-side, root.
[group('catalejo')]
catalejo-test: _mirilla-load
    #!/usr/bin/env bash
    set -uo pipefail
    trap 'code=$?; [ "$code" -eq 0 ] || dmesg | tail -n 100 >&2; rmmod mirilla || true; exit "$code"' EXIT

    echo "Running the catalejo Rust test suite under $(uname -r)"

    # The whole workspace runs under one live module. That is the unit tests and,
    # with --include-ignored, the device-backed integration tests a device-less
    # host skips. Serialize so the self-targeting suites do not contend for the device.
    cargo test --workspace --offline -- --include-ignored --test-threads=1

# Build the module and run the integration tests inside a mirilla-powered VM. Host-side.
[group('catalejo')]
catalejo-test-vm: mirilla-module catalejo-build
    cd '{{ kdir }}' && vng --user root --memory '{{ vm_memory }}' --cpu '{{ vm_cpus }}' -- \
        {{ guest_env }} --justfile '{{ justfile() }}' catalejo-test

# --- Shared internals ---

# Load the mirilla module and ensure /dev/mirilla exists. Guest-side, root.
[private]
_mirilla-load:
    #!/usr/bin/env bash
    set -euo pipefail

    if ! insmod '{{ module }}'; then
        echo "error: failed to load mirilla.ko" >&2
        dmesg | tail -n 50 >&2
        exit 1
    fi

    # devtmpfs usually materializes the node, so fall back to mknod when it did not.
    if [ ! -c /dev/mirilla ]; then
        major=$(awk '$2 == "mirilla" { print $1 }' /proc/devices)

        if [ -z "$major" ]; then
            echo "error: mirilla character device is absent" >&2
            exit 1
        fi

        mknod /dev/mirilla c "$major" 0
    fi
