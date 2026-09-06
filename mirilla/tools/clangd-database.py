#!/usr/bin/env python3
"""Project a Bear compilation database into one clangd source tree."""

import argparse
import json
from pathlib import Path

KERNEL_UNSUPPORTED = {
    "-fconserve-stack",
    "-fno-allow-store-data-races",
    "-fzero-init-padding-bits=all",
    "-mindirect-branch-register",
    "-mrecord-mcount",
}

KERNEL_UNSUPPORTED_PREFIXES = (
    "-falign-jumps=",
    "-falign-loops=",
    "-fdiagnostics-show-context=",
    "-fmin-function-alignment=",
    "-mindirect-branch=",
    "-mfunction-return=",
    "-mpreferred-stack-boundary=",
)


def parse_arguments() -> argparse.Namespace:
    """Parse the database projection arguments."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--sanitize-kernel", action="store_true")

    return parser.parse_args()


def entry_path(target_entry: dict[str, object]) -> Path:
    """Resolve the translation unit named by one database entry."""
    target_file = Path(str(target_entry["file"]))

    if not target_file.is_absolute():
        target_file = Path(str(target_entry["directory"])) / target_file

    return target_file.resolve()


def argument_is_supported(target_argument: str) -> bool:
    """Determine whether clangd accepts one captured Kbuild argument."""
    if target_argument in KERNEL_UNSUPPORTED:
        return False

    return not target_argument.startswith(KERNEL_UNSUPPORTED_PREFIXES)


def sanitize_kernel(target_entry: dict[str, object]) -> dict[str, object]:
    """Remove GCC-only Kbuild flags from one clangd command."""
    target_arguments = target_entry.get("arguments")

    if not isinstance(target_arguments, list):
        raise ValueError("Bear database entry does not contain an argument vector")

    target_entry = dict(target_entry)
    target_entry["arguments"] = [
        str(target_argument)
        for target_argument in target_arguments
        if argument_is_supported(str(target_argument))
    ]

    return target_entry


def main() -> None:
    """Write the entries rooted in one source tree to their clangd database."""
    target_arguments = parse_arguments()
    target_root = target_arguments.root.resolve()
    target_database = json.loads(target_arguments.input.read_text())

    target_entries = [
        target_entry
        for target_entry in target_database
        if entry_path(target_entry).is_relative_to(target_root)
    ]

    if target_arguments.sanitize_kernel:
        target_entries = [sanitize_kernel(target_entry) for target_entry in target_entries]

    if not target_entries:
        raise ValueError(f"no compilation entries found under {target_root}")

    target_arguments.output.write_text(json.dumps(target_entries, indent=2) + "\n")


if __name__ == "__main__":
    main()
