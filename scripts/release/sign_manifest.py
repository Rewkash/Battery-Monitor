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

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat


EXPECTED_PUBLIC_KEY_B64 = "DPivxj7wPutUTEtyUOwg411VVXHH0FCIle7Y6IVB9Xg="


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--bundle", required=True, type=pathlib.Path)
    parser.add_argument("--artifact-url", required=True)
    parser.add_argument("--release-notes", default="")
    parser.add_argument("--release-notes-url", default="")
    parser.add_argument("--mandatory", action="store_true")
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()

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
        "releaseNotes": args.release_notes,
        "releaseNotesUrl": args.release_notes_url,
        "schemaVersion": 1,
        "sequence": args.sequence,
        "version": args.version,
    }
    manifest = (json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
    signature = private_key.sign(manifest)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "update-manifest.json").write_bytes(manifest)
    (args.output_dir / "update-manifest.json.sig").write_text(base64.b64encode(signature).decode("ascii") + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
