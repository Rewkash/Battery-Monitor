#!/usr/bin/env python3
"""Independently verify release metadata and bundle integrity."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import struct

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


PUBLIC_KEY = base64.b64decode("bfwjZfSItKzGrGJ8TXHWilvU8QvJgJZtj+yM/DIatRc=")
MAX_FILES = 4096
MAX_FILE_SIZE = 512 * 1024 * 1024
RESERVED_NAMES = {"con", "prn", "aux", "nul", "clock$"} | {
    f"{prefix}{number}" for prefix in ("com", "lpt") for number in range(1, 10)
}


def read_exact(stream, size: int) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise SystemExit("bundle is truncated")
    return data


def validate_path(path: str) -> None:
    encoded = path.encode("utf-8")
    if not path or len(path) > 240 or len(encoded) > 240 or "\\" in path or ":" in path:
        raise SystemExit(f"unsafe bundle path: {path!r}")
    for part in path.split("/"):
        if (not part or part in (".", "..") or part.endswith((".", " ")) or
                any(ord(character) < 32 or character in '<>"|?*' for character in part) or
                part.split(".", 1)[0].casefold() in RESERVED_NAMES):
            raise SystemExit(f"unsafe bundle path: {path!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--signature", required=True, type=pathlib.Path)
    parser.add_argument("--bundle", required=True, type=pathlib.Path)
    parser.add_argument("--msi", type=pathlib.Path)
    args = parser.parse_args()
    manifest_bytes = args.manifest.read_bytes()
    signature = base64.b64decode(args.signature.read_text(encoding="ascii").strip(), validate=True)
    Ed25519PublicKey.from_public_bytes(PUBLIC_KEY).verify(signature, manifest_bytes)
    manifest = json.loads(manifest_bytes)
    artifact = manifest["artifact"]
    if args.bundle.stat().st_size != artifact["size"] or hashlib.sha256(args.bundle.read_bytes()).hexdigest() != artifact["sha256"]:
        raise SystemExit("bundle does not match signed manifest")
    msi_artifact = manifest.get("msiArtifact")
    if args.msi is not None:
        if (not isinstance(msi_artifact, dict) or msi_artifact.get("format") != "msi" or
                args.msi.stat().st_size != msi_artifact.get("size") or
                hashlib.sha256(args.msi.read_bytes()).hexdigest() != msi_artifact.get("sha256")):
            raise SystemExit("MSI does not match signed manifest")
    elif msi_artifact is not None:
        raise SystemExit("signed manifest contains an MSI that was not supplied for verification")
    with args.bundle.open("rb") as stream:
        if stream.read(8) != b"BMUP0001":
            raise SystemExit("invalid bundle magic")
        count = struct.unpack("<I", read_exact(stream, 4))[0]
        if count == 0 or count > MAX_FILES:
            raise SystemExit("invalid bundle file count")
        seen = set()
        file_paths = set()
        for _ in range(count):
            path_size = struct.unpack("<H", read_exact(stream, 2))[0]
            path_bytes = read_exact(stream, path_size)
            try:
                path = path_bytes.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise SystemExit("bundle path is not UTF-8") from exc
            validate_path(path)
            size = struct.unpack("<Q", read_exact(stream, 8))[0]
            if size > MAX_FILE_SIZE:
                raise SystemExit(f"bundle entry is too large: {path}")
            expected = read_exact(stream, 32)
            content = read_exact(stream, size)
            folded = path.casefold()
            parents = {str(parent) for parent in pathlib.PurePosixPath(folded).parents if str(parent) != "."}
            if (folded in seen or parents.intersection(file_paths) or
                    any(existing.startswith(folded + "/") for existing in file_paths) or
                    hashlib.sha256(content).digest() != expected):
                raise SystemExit(f"invalid bundle entry: {path}")
            seen.add(folded)
            file_paths.add(folded)
        if stream.read(1):
            raise SystemExit("bundle has trailing data")
    print(f"verified version={manifest['version']} files={count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
