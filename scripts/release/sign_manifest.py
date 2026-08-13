#!/usr/bin/env python3
"""Generate and sign the exact update-manifest.json bytes using an environment-only Ed25519 seed."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
import pathlib
import urllib.parse

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


EXPECTED_PUBLIC_KEY_B64 = "bfwjZfSItKzGrGJ8TXHWilvU8QvJgJZtj+yM/DIatRc="


def require_release_url(value: str, label: str) -> None:
    parsed = urllib.parse.urlparse(value)
    if parsed.scheme != "https" or parsed.hostname not in {"github.com", "objects.githubusercontent.com"}:
        raise SystemExit(f"{label} must be an HTTPS GitHub release URL")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--bundle", required=True, type=pathlib.Path)
    parser.add_argument("--artifact-url", required=True)
    parser.add_argument("--msi", type=pathlib.Path)
    parser.add_argument("--msi-url")
    parser.add_argument("--release-notes", default="")
    parser.add_argument("--release-notes-file", type=pathlib.Path)
    parser.add_argument("--release-notes-url", default="")
    parser.add_argument("--mandatory", action="store_true")
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    require_release_url(args.artifact_url, "artifact URL")

    seed_b64 = os.environ.get("BATTERY_MONITOR_ED25519_PRIVATE_KEY_B64", "")
    try:
        seed = base64.b64decode(seed_b64, validate=True)
    except Exception as exc:
        raise SystemExit(f"invalid BATTERY_MONITOR_ED25519_PRIVATE_KEY_B64: {exc}") from exc
    if len(seed) != 32:
        raise SystemExit("Ed25519 secret must decode to exactly 32 bytes")
    private_key = Ed25519PrivateKey.from_private_bytes(seed)
    public_key = private_key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    if base64.b64encode(public_key).decode("ascii") != EXPECTED_PUBLIC_KEY_B64:
        raise SystemExit("secret does not match the public key embedded in the application")

    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    expires = now + dt.timedelta(days=14)
    release_notes = args.release_notes
    if args.release_notes_file is not None:
        if args.release_notes:
            raise SystemExit("use either --release-notes or --release-notes-file")
        release_notes = args.release_notes_file.read_text(encoding="utf-8")
    if len(release_notes.encode("utf-8")) > 48 * 1024:
        raise SystemExit("release notes exceed 48 KiB")

    payload = {
        "artifact": {
            "format": "bmup-1",
            "sha256": hashlib.sha256(args.bundle.read_bytes()).hexdigest(),
            "size": args.bundle.stat().st_size,
            "url": args.artifact_url,
        },
        "channel": "stable",
        "expiresAt": expires.isoformat().replace("+00:00", "Z"),
        "keyId": hashlib.sha256(public_key).hexdigest(),
        "mandatory": args.mandatory,
        "publishedAt": now.isoformat().replace("+00:00", "Z"),
        "releaseNotes": release_notes,
        "releaseNotesUrl": args.release_notes_url,
        "schemaVersion": 1,
        "sequence": args.sequence,
        "version": args.version,
    }
    if args.msi is not None or args.msi_url is not None:
        if args.msi is None or not args.msi_url:
            raise SystemExit("--msi and --msi-url must be used together")
        require_release_url(args.msi_url, "MSI URL")
        payload["msiArtifact"] = {
            "format": "msi",
            "sha256": hashlib.sha256(args.msi.read_bytes()).hexdigest(),
            "size": args.msi.stat().st_size,
            "url": args.msi_url,
        }
    manifest = (json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
    signature = private_key.sign(manifest)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "update-manifest.json").write_bytes(manifest)
    (args.output_dir / "update-manifest.json.sig").write_text(base64.b64encode(signature).decode("ascii") + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
