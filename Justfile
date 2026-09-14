# Catalejo task runner.
#
# Testing has three entry points: host-safe checks, KUnit, and the complete VM
# integration path. The VM path runs the Mirilla C ABI coverage and Rust device
# tests under the same loaded module. Private recipes only bridge host/guest
# execution and are intentionally hidden from the public task surface.
#
# Run each recipe line under a strict shell so a failing command aborts the recipe.

set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

# The kernel tree the module is built against and the VM boots. Defaults to the
# running kernel's build directory, and CI points it at the matrix kernel checkout.

kdir := env("KDIR", "/lib/modules/" + `uname -r` + "/build")

# The built module object the guest loads.

module := justfile_directory() / "mirilla" / "mirilla.ko"
kunit_module := justfile_directory() / "mirilla" / "mirilla-kunit.ko"

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

    # Every tracked C source across the kernel module and the system userspace mechanism.
    sources=(
        mirilla/src/*.c mirilla/include/*.h
        mirilla/test/*.c mirilla/test/*.h mirilla/test/kunit/*.c
        catalejo-sys/c/src/*.c catalejo-sys/c/include/*.h
    )

    echo "==> clang-format (${#sources[@]} files)"
    clang-format --dry-run --Werror "${sources[@]}"

    # clangd needs a compile database. The userspace test suite and the system
    # mechanism build without the kernel, so `bear` captures their commands on a
    # bare host. The in-kernel module sources need kernel headers and are checked
    # when the module is built, so they are left out of this pass.
    echo "==> clangd --check (userspace translation units)"
    database=$(mktemp -d)
    trap 'rm -rf "$database"' EXIT

    bear --output "$database/compile_commands.json" -- make -C mirilla test >/dev/null

    status=0
    for unit in mirilla/test/*.c catalejo-sys/c/src/*.c; do
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

# --- Testing ---

# Build the mirilla kernel module against the kernel tree at {{kdir}}.
[group('test')]
mirilla-module:
    make -C mirilla KDIR='{{ kdir }}' module

# Run host-side checks that do not require a live Mirilla device.
[group('test')]
test: lint _test-build
    cargo test --workspace

# Run the userspace ABI and Rust integration tests under the same loaded module.
[group('test')]
test-vm: mirilla-module _test-build
    cd '{{ kdir }}' && vng --user root --memory '{{ vm_memory }}' --cpu '{{ vm_cpus }}' -- \
        {{ guest_env }} --justfile '{{ justfile() }}' _test-guest

# Build and run the kernel-side KUnit suite.
[group('test')]
kunit:
    #!/usr/bin/env bash
    set -euo pipefail

    grep -qx 'CONFIG_KUNIT=y' '{{ kdir }}/.config' || {
        echo 'error: target kernel must enable CONFIG_KUNIT=y' >&2
        exit 1
    }

    test -x '{{ kdir }}/tools/testing/kunit/kunit.py' || {
        echo 'error: target kernel tree does not provide tools/testing/kunit/kunit.py' >&2
        exit 1
    }

    make -C mirilla KDIR='{{ kdir }}' MIRILLA_KUNIT=1 MIRILLA_MODULE_NAME=mirilla-kunit module
    cd '{{ kdir }}'
    vng --user root --memory '{{ vm_memory }}' --cpu '{{ vm_cpus }}' -- \
        {{ guest_env }} --justfile '{{ justfile() }}' _kunit-guest

# --- Shared internals ---

# Build the userspace integration binaries without requiring a device.
[private]
_test-build:
    make -C mirilla test
    cargo test --workspace --no-run

# Run all device-backed userspace tests under the same module instance. Guest-side, root.
[private]
_test-guest: _mirilla-load
    #!/usr/bin/env bash
    set -euo pipefail
    trap 'code=$?; [ "$code" -eq 0 ] || dmesg | tail -n 100 >&2; rmmod mirilla || true; exit "$code"' EXIT

    mirilla/test/mirilla-test
    cargo test --workspace --offline -- --include-ignored --test-threads=1

# Load the KUnit build and let the kernel KUnit parser determine success. Guest-side, root.
[private]
_kunit-guest:
    #!/usr/bin/env bash
    set -euo pipefail

    name=$(modinfo -F name '{{ kunit_module }}')
    trap 'code=$?; rmmod "$name" 2>/dev/null || true; exit "$code"' EXIT

    dmesg -C
    insmod '{{ kunit_module }}'
    dmesg | python3 '{{ kdir }}/tools/testing/kunit/kunit.py' parse

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
