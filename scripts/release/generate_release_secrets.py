#!/usr/bin/env python3
"""Generate a fresh Ed25519 release seed and public-key rotation outputs."""

from __future__ import annotations

import argparse
import base64
import hashlib
import pathlib

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    private_key = Ed25519PrivateKey.generate()
    seed = private_key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
    public_key = private_key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    (args.output_dir / "ed25519-seed.b64").write_text(base64.b64encode(seed).decode("ascii"), encoding="ascii")
    (args.output_dir / "ed25519-public.b64").write_text(base64.b64encode(public_key).decode("ascii"), encoding="ascii")
    (args.output_dir / "ed25519-key-id.txt").write_text(hashlib.sha256(public_key).hexdigest(), encoding="ascii")
    print("Release key generated; update every embedded public-key consumer before storing the seed secret.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
