#!/usr/bin/env python3
"""Print the fork tag: <upstream release>-<positive build number>."""
import re
from datetime import datetime
from pathlib import Path

def read_version(path: Path) -> str:
    value = path.read_text().strip()
    match = re.fullmatch(r"(\d{2}-\d{2}-\d{2})-([1-9]\d*)", value)
    if not match:
        raise ValueError("FORK_VERSION must be <upstream YY-MM-DD>-<positive build number>")
    datetime.strptime(match[1], "%y-%m-%d")
    return value

if __name__ == "__main__":
    print(read_version(Path(__file__).resolve().parents[1] / "FORK_VERSION"))
