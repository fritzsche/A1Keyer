"""
Restore the framework's pre-built ``sdkconfig.h`` from the backup created
by ``override_sdkconfig.py``.

Run as a PlatformIO post: extra script — after the build, before the
next build (or a different project's build) sees the modified file.

Idempotent: if no backup is present for a file, the script leaves it
alone.
"""
import os
import shutil

Import("env")  # noqa: F821  (Scons injects this)

BACKUP_SUFFIX = ".bak"
ESP32S3_MEMORY_TYPES = [
    "qio_qspi",
    "qio_opi",
    "opi_opi",
    "dio_opi",
    "dio_qspi",
]


def find_sdkconfig_headers():
    arduino_libs_dir = os.path.join(
        os.path.expanduser("~/.platformio/packages"),
        "framework-arduinoespressif32-libs",
        "esp32s3",
    )
    return [
        os.path.join(arduino_libs_dir, mem_type, "include", "sdkconfig.h")
        for mem_type in ESP32S3_MEMORY_TYPES
    ]


def main():
    for sdkconfig_path in find_sdkconfig_headers():
        if not os.path.isfile(sdkconfig_path):
            continue
        backup_path = sdkconfig_path + BACKUP_SUFFIX
        if not os.path.exists(backup_path):
            # No backup means we never modified this file (or it was
            # already restored). Either way, nothing to do.
            continue
        shutil.copyfile(backup_path, sdkconfig_path)
        os.remove(backup_path)
        print(f"[restore_sdkconfig] restored {sdkconfig_path} from {backup_path}")


main()
