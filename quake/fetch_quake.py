#!/usr/bin/env python3
# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Fetch id Software's Quake source (GPL-2.0-or-later) and apply this port's patches.

The Quake source is not part of this repository. This script clones
https://github.com/id-Software/Quake at a pinned commit into quake/upstream/
(ignored by git) and applies quake/patches/*.patch in name order. Run it once
before the first build of the quake project, and again after the patches
changed:

    python quake/fetch_quake.py            # clone if needed, reset to the pin, apply patches
    python quake/fetch_quake.py --make-patch NAME
                                           # write the current edits in upstream/ to patches/NAME.patch

The game data is not fetched: supply the shareware id1/pak0.pak yourself
(see documentation/npu-quake.md).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
UPSTREAM = HERE / "upstream"
PATCHES = HERE / "patches"
URL = "https://github.com/id-Software/Quake.git"
COMMIT = "bf4ac424ce754894ac8f1dae6a3981954bc9852d"  # master, 2012-01-31: the GPL source release


def git(*args: str, cwd: Path = UPSTREAM) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, text=True, capture_output=True).stdout


def fetch() -> None:
    if not (UPSTREAM / ".git").is_dir():
        UPSTREAM.mkdir(parents=True, exist_ok=True)
        git("init", "--quiet")
        git("remote", "add", "origin", URL)
    if not _has(COMMIT):
        print(f"[quake] fetching {URL} @ {COMMIT[:12]}")
        git("fetch", "--quiet", "--depth", "1", "origin", COMMIT)
    git("checkout", "--quiet", "--force", COMMIT)
    git("clean", "--quiet", "-fdx")
    patches = sorted(PATCHES.glob("*.patch"))
    for patch in patches:
        print(f"[quake] applying {patch.name}")
        git("apply", "--whitespace=nowarn", str(patch))
    print(f"[quake] {UPSTREAM.relative_to(HERE.parent)} at {COMMIT[:12]} + {len(patches)} patch(es)")


def _has(commit: str) -> bool:
    return subprocess.run(["git", "cat-file", "-e", f"{commit}^{{commit}}"], cwd=UPSTREAM, capture_output=True).returncode == 0


def make_patch(name: str) -> None:
    """Write the working tree's whole diff against the pinned commit as one patch."""
    diff = git("diff", "--no-color", COMMIT, "--", "WinQuake")
    if not diff.strip():
        sys.exit("[quake] no edits in upstream/WinQuake")
    target = PATCHES / f"{name}.patch"
    target.write_text(diff)
    print(f"[quake] wrote {target.relative_to(HERE.parent)} ({len(diff.splitlines())} lines): the whole diff against the pin;")
    print("[quake] remove the older patches it replaces, or split it by hand")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--make-patch", metavar="NAME", help="write the edits in upstream/ to patches/NAME.patch")
    args = parser.parse_args()
    if args.make_patch:
        make_patch(args.make_patch)
    else:
        fetch()


if __name__ == "__main__":
    main()
