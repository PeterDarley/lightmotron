#!/usr/bin/env python3
"""Build the deployment package for Lightmotron.

Creates ``deployment/lightmotron.bin`` by merging the MicroPython ESP32-S3
firmware with a LittleFS filesystem image containing all project source files.
The resulting binary can be flashed to a bare ESP32-S3 via the web installer
at ``deployment/index.html`` using the Web Serial API (Chrome / Edge).

Requirements
------------
    pip install esptool littlefs-python

Usage
-----
    python tools/build_deployment.py [--firmware PATH] [--fs-size KB]

    --firmware PATH   Path to the MicroPython .bin (default: auto-detected
                      ESP32_GENERIC_S3-SPIRAM_OCT-*.bin in the project root).
    --fs-size KB      Filesystem image size in KB (default: 1024).
"""

import argparse
import glob
import os
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

ROOT: Path = Path(__file__).parent.parent
DEPLOYMENT_DIR: Path = ROOT / "deployment"

# VFS partition offset for ESP32-S3 MicroPython with 16MB flash.
# Matches the standard partitions-16MiB.csv shipped with MicroPython.
VFS_OFFSET: int = 0x200000

# Files/directories to copy into the device filesystem.
INCLUDE_DIRS: list = ["lib", "www", "templates", "web"]
INCLUDE_ROOT_FILES: list = ["boot.py", "main.py", "settings.py"]

# Relative paths (forward-slash) to exclude even if they are inside INCLUDE_DIRS.
EXCLUDE_PATHS: set = {
    "lib/licence",
    "lib/README.md",
}

# LittleFS parameters that match MicroPython's defaults for ESP32.
FS_BLOCK_SIZE: int = 4096
FS_READ_SIZE: int = 256
FS_PROG_SIZE: int = 256


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _find_firmware() -> Path:
    """Auto-detect the MicroPython firmware binary in the project root."""

    candidates = sorted(glob.glob(str(ROOT / "ESP32_GENERIC_S3-SPIRAM_OCT-*.bin")))
    if not candidates:
        print("ERROR: No MicroPython firmware found in project root.")
        print("Download from: https://micropython.org/download/ESP32_GENERIC_S3/")
        print("Expected filename pattern: ESP32_GENERIC_S3-SPIRAM_OCT-*.bin")
        sys.exit(1)

    if len(candidates) > 1:
        print("WARNING: Multiple firmware files found, using the most recent:")
        for path in candidates:
            print(" ", path)

    return Path(candidates[-1])


def _build_filesystem(fs_size_kb: int) -> bytes:
    """Create a LittleFS image containing all project files.

    Args:
        fs_size_kb: Filesystem image size in kilobytes.

    Returns:
        Raw bytes of the LittleFS image.
    """

    try:
        from littlefs import LittleFS  # type: ignore
    except ImportError:
        print("ERROR: littlefs-python not installed.")
        print("Active Python:", sys.executable)
        print("Run:", '"{}" -m pip install littlefs-python'.format(sys.executable))
        sys.exit(1)

    block_count: int = (fs_size_kb * 1024) // FS_BLOCK_SIZE
    fs = LittleFS(
        block_size=FS_BLOCK_SIZE,
        block_count=block_count,
        read_size=FS_READ_SIZE,
        prog_size=FS_PROG_SIZE,
    )

    files_added: int = 0
    total_bytes: int = 0

    def _add_file(remote_path: str, local_path: Path) -> None:
        """Add a single file into the filesystem image."""

        nonlocal files_added, total_bytes

        # Ensure parent directories exist in the image.
        parts = remote_path.split("/")
        for depth in range(1, len(parts)):
            directory = "/".join(parts[:depth])
            try:
                fs.mkdir(directory)
            except Exception:
                pass  # Already exists.

        content: bytes = local_path.read_bytes()
        with fs.open(remote_path, "wb") as image_file:
            image_file.write(content)

        print("  + {} ({} bytes)".format(remote_path, len(content)))
        files_added += 1
        total_bytes += len(content)

    # Root .py files.
    for filename in INCLUDE_ROOT_FILES:
        src: Path = ROOT / filename
        if src.exists():
            _add_file(filename, src)

    # Subdirectories.
    for dir_name in INCLUDE_DIRS:
        dir_path: Path = ROOT / dir_name
        if not dir_path.exists():
            continue

        for file_path in sorted(dir_path.rglob("*")):
            if not file_path.is_file():
                continue

            rel: str = file_path.relative_to(ROOT).as_posix()
            if rel in EXCLUDE_PATHS:
                print("  - {} (excluded)".format(rel))
                continue

            _add_file(rel, file_path)

    print(
        "\nFilesystem image: {} files, {:.1f} KB content, {} KB image".format(
            files_added, total_bytes / 1024, fs_size_kb
        )
    )
    return bytes(fs.context.buffer)


def build(firmware_path: Path, fs_size_kb: int) -> None:
    """Build the merged firmware + filesystem binary.

    Args:
        firmware_path: Path to the MicroPython firmware .bin file.
        fs_size_kb: Size of the LittleFS image in kilobytes.
    """

    print("Firmware: {}".format(firmware_path))
    print("Building filesystem image ({} KB)...".format(fs_size_kb))
    fs_image: bytes = _build_filesystem(fs_size_kb)

    print("\nMerging firmware + filesystem at offset 0x{:x}...".format(VFS_OFFSET))
    firmware: bytes = firmware_path.read_bytes()

    if len(firmware) > VFS_OFFSET:
        print("ERROR: Firmware ({} bytes) overlaps VFS offset (0x{:x}).".format(len(firmware), VFS_OFFSET))
        sys.exit(1)

    # Build merged binary: firmware at 0x0, 0xFF padding, filesystem at VFS_OFFSET.
    merged_size: int = VFS_OFFSET + len(fs_image)
    merged: bytearray = bytearray(b"\xff" * merged_size)
    merged[0 : len(firmware)] = firmware
    merged[VFS_OFFSET : VFS_OFFSET + len(fs_image)] = fs_image

    DEPLOYMENT_DIR.mkdir(exist_ok=True)
    output_path: Path = DEPLOYMENT_DIR / "lightmotron.bin"
    output_path.write_bytes(merged)

    size_mb: float = len(merged) / (1024 * 1024)
    print("Output: {} ({:.2f} MB)".format(output_path, size_mb))
    print(
        "\nDone.  Copy the deployment/ directory to a web server and users can\n"
        "flash Lightmotron directly from their browser via USB."
    )


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> None:
    """Parse arguments and run the build."""

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware", type=Path, default=None, help="Path to MicroPython firmware .bin")
    parser.add_argument(
        "--fs-size", type=int, default=1024, metavar="KB", help="Filesystem image size in KB (default: 1024)"
    )
    args = parser.parse_args()

    firmware_path: Path = args.firmware if args.firmware else _find_firmware()

    if not firmware_path.exists():
        print("ERROR: Firmware not found:", firmware_path)
        sys.exit(1)

    build(firmware_path, args.fs_size)


if __name__ == "__main__":
    main()
