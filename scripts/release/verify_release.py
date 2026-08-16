#!/usr/bin/env python3
"""Independently verify release metadata and bundle integrity."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import re
import struct

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


PUBLIC_KEY = base64.b64decode("bfwjZfSItKzGrGJ8TXHWilvU8QvJgJZtj+yM/DIatRc=")
DEFAULT_IDENTITY_FILE = pathlib.Path(__file__).resolve().parents[2] / "CMakeLists.txt"
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+$")
GUID_PATTERN = re.compile(r"^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                          r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$")
MAX_FILES = 4096
MAX_FILE_SIZE = 512 * 1024 * 1024
RESERVED_NAMES = {"con", "prn", "aux", "nul", "clock$"} | {
    f"{prefix}{number}" for prefix in ("com", "lpt") for number in range(1, 10)
}


def read_upgrade_code(identity_file: pathlib.Path) -> str:
    try:
        text = identity_file.read_text(encoding="utf-8")
    except OSError as exc:
        raise SystemExit(f"cannot read identity file {identity_file}: {exc}") from exc
    match = re.search(
        r'set\s*\(\s*BATTERY_MONITOR_MSI_UPGRADE_CODE\s+"\{([0-9A-Fa-f-]{36})\}"\s*\)',
        text,
    )
    if match is None:
        raise SystemExit("BATTERY_MONITOR_MSI_UPGRADE_CODE is missing or invalid")
    return "{" + match.group(1).upper() + "}"


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
    parser.add_argument("--expected-version",
                        help="release version (tag, numeric X.Y.Z) the manifest must carry")
    parser.add_argument("--msi-info", type=pathlib.Path,
                        help="JSON file with MSI product identity (e.g. from Get-MsiInfo.ps1)")
    parser.add_argument("--identity-file", default=DEFAULT_IDENTITY_FILE, type=pathlib.Path,
                        help="CMakeLists.txt used to look up BATTERY_MONITOR_MSI_UPGRADE_CODE")
    args = parser.parse_args()
    manifest_bytes = args.manifest.read_bytes()
    signature = base64.b64decode(args.signature.read_text(encoding="ascii").strip(), validate=True)
    Ed25519PublicKey.from_public_bytes(PUBLIC_KEY).verify(signature, manifest_bytes)
    manifest = json.loads(manifest_bytes)
    artifact = manifest["artifact"]
    if artifact.get("format") != "bmup-1":
        raise SystemExit("manifest artifact format is not bmup-1")
    version = manifest["version"]
    if not VERSION_PATTERN.match(version):
        raise SystemExit(f"manifest version is not numeric X.Y.Z: {version!r}")
    if args.expected_version is not None and args.expected_version != version:
        raise SystemExit(f"manifest version {version} does not match expected version "
                         f"{args.expected_version}")
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
    msi_info = None
    if args.msi_info is not None:
        if args.msi is None:
            raise SystemExit("--msi-info requires --msi")
        msi_info = json.loads(args.msi_info.read_text(encoding="utf-8-sig"))
        for key in ("productName", "productVersion", "productCode", "upgradeCode", "manufacturer"):
            if not isinstance(msi_info.get(key), str) or not msi_info[key]:
                raise SystemExit(f"MSI identity file lacks a valid {key!r} entry")
        if not VERSION_PATTERN.match(msi_info["productVersion"]):
            raise SystemExit(f"MSI ProductVersion is not numeric X.Y.Z: {msi_info['productVersion']!r}")
        if msi_info["productVersion"] != version:
            raise SystemExit(f"MSI ProductVersion {msi_info['productVersion']} does not match "
                             f"manifest version {version}")
        for key in ("productCode", "upgradeCode"):
            if not GUID_PATTERN.match(msi_info[key]):
                raise SystemExit(f"MSI {key} is not a GUID: {msi_info[key]!r}")
        upgrade_code = read_upgrade_code(args.identity_file)
        if msi_info["upgradeCode"].upper() != upgrade_code:
            raise SystemExit(f"MSI UpgradeCode {msi_info['upgradeCode']} does not match "
                             f"BATTERY_MONITOR_MSI_UPGRADE_CODE {upgrade_code}")
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
    suffix = ""
    if msi_info is not None:
        suffix = (f" msiProductVersion={msi_info['productVersion']}"
                  f" msiUpgradeCode={msi_info['upgradeCode']}")
    print(f"verified version={manifest['version']} files={count}{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
