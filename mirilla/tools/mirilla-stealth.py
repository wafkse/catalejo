#!/usr/bin/env python3
"""Generate deterministic, seed-keyed Mirilla symbol aliases from a curated list."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

SYMBOL_PATTERN = re.compile(r"mirilla_[A-Za-z0-9_]+")


def symbols(list_path: Path) -> list[str]:
    """Read unique Mirilla identifiers from the checked-in symbol list."""
    target_symbols: set[str] = set()

    for target_line in list_path.read_text(encoding="utf-8").splitlines():
        target_symbol = target_line.strip()
        if not target_symbol or target_symbol.startswith("#"):
            continue

        if SYMBOL_PATTERN.fullmatch(target_symbol) is None:
            raise ValueError(f"invalid Mirilla symbol in {list_path}: {target_symbol}")

        if target_symbol in target_symbols:
            raise ValueError(f"duplicate Mirilla symbol in {list_path}: {target_symbol}")

        target_symbols.add(target_symbol)

    return sorted(target_symbols)


def alias(symbol: str, seed: str) -> str:
    """Derive a valid and deterministic C identifier for one source symbol."""
    target_key = hashlib.sha256(seed.encode("utf-8")).digest()
    target_digest = hashlib.blake2s(
        symbol.encode("utf-8"),
        key=target_key,
        digest_size=16,
    ).hexdigest()

    return f"symbol{target_digest}"


def render(symbols: list[str], seed: str) -> str:
    """Render the generated header for the complete curated symbol set."""
    target_aliases = {symbol: alias(symbol, seed) for symbol in symbols}
    if len(set(target_aliases.values())) != len(target_aliases):
        raise ValueError("seed-derived symbol aliases collided")

    target_metadata = {
        "author": alias("metadata_author", seed),
        "description": alias("metadata_description", seed),
        "device_parameter": alias("metadata_device_parameter", seed),
        "peephole_inode_name": alias("metadata_peephole_inode_name", seed),
    }

    target_lines = [
        "/* Automatically generated. Do not edit. */",
        "#ifndef _MIRILLA_SYMBOL_GENERATED_H",
        "#define _MIRILLA_SYMBOL_GENERATED_H",
        "",
        f"#define MIRILLA_STEALTH_AUTHOR \"{target_metadata['author']}\"",
        f"#define MIRILLA_STEALTH_DESCRIPTION \"{target_metadata['description']}\"",
        f"#define MIRILLA_STEALTH_DEVICE_PARAMETER {target_metadata['device_parameter']}",
        f"#define MIRILLA_STEALTH_PEEPHOLE_INODE_NAME \"[{target_metadata['peephole_inode_name']}]\"",
        "",
    ]

    for symbol, target_alias in target_aliases.items():
        target_lines.extend(
            (
                f"#define SYM_{symbol} {target_alias}",
                f"#define {symbol} STEALTH_SYMBOL({symbol}, SYM_{symbol})",
            )
        )

    target_lines.extend(("", "#endif", ""))

    return "\n".join(target_lines)


def main() -> None:
    """Generate a header from a seed and a checked-in Mirilla symbol list."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", required=True)
    parser.add_argument("--list", type=Path)
    parser.add_argument("--module-name", action="store_true")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    if not arguments.seed:
        parser.error("--seed must not be empty")

    if arguments.module_name:
        print(alias("metadata_module_name", arguments.seed))

        return

    if arguments.list is None or arguments.output is None:
        parser.error("--list and --output are required when generating a header")

    target_output = render(symbols(arguments.list), arguments.seed)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)

    if arguments.output.exists() and arguments.output.read_text(encoding="utf-8") == target_output:
        return

    arguments.output.write_text(target_output, encoding="utf-8")


if __name__ == "__main__":
    main()
