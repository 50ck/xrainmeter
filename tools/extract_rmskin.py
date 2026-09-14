#!/usr/bin/env python3
"""Safely extract a Rainmeter .rmskin package without changing its contents."""

import argparse
import json
import stat
import zipfile
from pathlib import Path, PurePosixPath


def extract_package(package: Path, destination: Path) -> dict[str, object]:
    configs: list[str] = []
    total_size = 0
    entry_count = 0
    with zipfile.ZipFile(package) as archive:
        entry_count = len(archive.infolist())
        if entry_count > 10_000:
            raise SystemExit("refusing package with more than 10,000 entries")
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename.replace("\\", "/"))
            mode = entry.external_attr >> 16
            total_size += entry.file_size
            if not path.parts or path.is_absolute() or ".." in path.parts or ":" in path.parts[0] or stat.S_ISLNK(mode):
                raise SystemExit(f"unsafe package entry: {entry.filename!r}")
            if entry.flag_bits & 1:
                raise SystemExit(f"encrypted package entry is unsupported: {entry.filename!r}")
            if entry.file_size > 512 * 1024 * 1024 or total_size > 1024 * 1024 * 1024:
                raise SystemExit("package exceeds extraction size limit")
            if path.suffix.lower() == ".ini" and "Skins" in path.parts:
                configs.append(path.as_posix())
        archive.extractall(destination)

    return {
        "package": str(package),
        "destination": str(destination),
        "configs": sorted(configs),
        "entryCount": entry_count,
        "uncompressedBytes": total_size,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    print(json.dumps(extract_package(args.package, args.destination), indent=2))


if __name__ == "__main__":
    main()
