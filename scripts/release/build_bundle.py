#!/usr/bin/env python3
"""Build the deterministic, uncompressed bmup-1 update bundle."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct


MAGIC = b"BMUP0001"
MAX_FILES = 4096
MAX_FILE_SIZE = 512 * 1024 * 1024
RESERVED_NAMES = {"con", "prn", "aux", "nul", "clock$"} | {
    f"{prefix}{number}" for prefix in ("com", "lpt") for number in range(1, 10)
}


def validate_path(relative: str) -> None:
    if not relative or len(relative) > 240 or "\\" in relative or ":" in relative:
        raise SystemExit(f"unsafe bundle path: {relative!r}")
    for part in relative.split("/"):
        if (not part or part in (".", "..") or part.endswith((".", " ")) or
                any(ord(character) < 32 or character in '<>"|?*' for character in part) or
                part.split(".", 1)[0].casefold() in RESERVED_NAMES):
            raise SystemExit(f"unsafe bundle path: {relative!r}")


def iter_files(root: pathlib.Path):
    return sorted((p for p in root.rglob("*") if p.is_file()), key=lambda p: p.relative_to(root).as_posix().casefold())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    root = args.input.resolve()
    files = list(iter_files(root))
    if not files:
        raise SystemExit("input directory is empty")
    if len(files) > MAX_FILES:
        raise SystemExit("too many files in bundle")
    seen = set()
    file_paths = set()
    for file_path in files:
        relative_text = file_path.relative_to(root).as_posix()
        validate_path(relative_text)
        folded = relative_text.casefold()
        if folded in seen or any(parent in file_paths for parent in pathlib.PurePosixPath(folded).parents if str(parent) != "."):
            raise SystemExit(f"duplicate or colliding bundle path: {relative_text}")
        if any(existing.startswith(folded + "/") for existing in file_paths):
            raise SystemExit(f"file/directory collision: {relative_text}")
        seen.add(folded)
        file_paths.add(folded)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as bundle:
        bundle.write(MAGIC)
        bundle.write(struct.pack("<I", len(files)))
        for file_path in files:
            relative = file_path.relative_to(root).as_posix().encode("utf-8")
            if len(relative) > 240:
                raise SystemExit(f"path too long: {relative!r}")
            data = file_path.read_bytes()
            if len(data) > MAX_FILE_SIZE:
                raise SystemExit(f"file too large: {relative!r}")
            bundle.write(struct.pack("<H", len(relative)))
            bundle.write(relative)
            bundle.write(struct.pack("<Q", len(data)))
            bundle.write(hashlib.sha256(data).digest())
            bundle.write(data)
    print(f"{args.output} sha256={hashlib.sha256(args.output.read_bytes()).hexdigest()} size={args.output.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
