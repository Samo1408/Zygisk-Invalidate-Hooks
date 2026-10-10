#!/usr/bin/env python3
"""Accept only the release module produced by this workflow run."""

from pathlib import Path
import sys
from zipfile import ZipFile, BadZipFile


def main() -> None:
    marker = Path(sys.argv[1])
    output = Path(sys.argv[2])
    archives = [
        path for path in Path("module/release").glob("*-release.zip")
        if path.is_file() and path.stat().st_mtime > marker.stat().st_mtime
    ]
    if len(archives) != 1:
        raise SystemExit(
            f"Expected exactly one freshly built release module ZIP; found {len(archives)}"
        )

    required = {
        "module.prop",
        "customize.sh",
        "webroot/index.html",
        "webroot/ui-enhancements.js",
        *(
            f"lib/{abi}/libinline_hook_spoof.so"
            for abi in ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
        ),
    }
    try:
        with ZipFile(archives[0]) as archive:
            missing = required - set(archive.namelist())
            if missing:
                raise SystemExit(f"Module ZIP is missing: {', '.join(sorted(missing))}")
            if corrupt := archive.testzip():
                raise SystemExit(f"Module ZIP has a corrupt member: {corrupt}")
    except BadZipFile as error:
        raise SystemExit(f"Invalid module ZIP: {error}") from error

    with output.open("a", encoding="utf-8") as file:
        file.write(f"path={archives[0]}\n")
    print(f"Verified module ZIP: {archives[0]}")


if __name__ == "__main__":
    main()
