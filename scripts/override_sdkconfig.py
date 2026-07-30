"""
Override arduino-esp32's NimBLE HCI buffer pool sizes for the Cardputer
ADV (ESP32-S3, no PSRAM, 512 KB internal SRAM).

WHY THIS EXISTS
---------------
The framework's defaults in
~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/qio_qspi/include/sdkconfig.h
total ~78 KB of internal SRAM for the BLE HCI mbuf pools:

  CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT             = 12  (12 * 256 * 4 = 12 KB)
  CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE              = 256
  CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT             = 24  (24 * 320 * 4 = 30 KB)
  CONFIG_BT_NIMBLE_MSYS_2_BLOCK_SIZE              = 320
  CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT            = 30  (30 *  70 * 4 =  8 KB)
  CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE             = 70
  CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT    = 8   ( 8 *  70 * 4 =  2 KB)
  CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT    = 24  (24 * 255 * 4 = 24 KB)
  CONFIG_BT_NIMBLE_TRANSPORT_ACL_SIZE             = 255

That single 78 KB calloc() fails under heap pressure on a no-PSRAM
ESP32-S3 with arduino-esp32 3.3.5 + NimBLE-Arduino 2.5.0, surfacing as
the well-known error:

  esp_nimble_hci_init() failed; err=257  (ESP_ERR_NO_MEM)

The call path is:
  NimBLEDevice::init()  -> esp_bt_controller_init() -> esp_bt_controller_enable()
                         -> esp_nimble_hci_init() -> ble_buf_alloc()
                         -> os_msys_buf_alloc()  -> nimble_platform_mem_calloc(...)

For a NUS peripheral with one connection (A1Keyer's use case), the
default pool sizes are wildly over-provisioned. Shrinking the EVT,
DISCARD, and ACL pools to the minimum that still works for one peer
frees ~25 KB of internal SRAM and clears the OOM.

WHAT THIS DOES
--------------
1. Locates the framework's pre-built ``sdkconfig.h`` for the ESP32-S3
   (qio_qspi memory type — the default for the Cardputer ADV).
2. Backs it up next to the file (sdkconfig.h.bak) on first run.
3. Rewrites the five BLE pool keys to the smaller values.
4. The build picks up the modified header immediately on the next
   compile. No Kconfig regeneration is needed (the Arduino framework
   uses the pre-built header directly).
5. The restore script (restore_sdkconfig.py, wired as post:) restores
   the original so subsequent projects are unaffected.

The backup is shared across all projects built with this env, so the
restore MUST run. If you delete the .bak file after a partial build,
the next run will see an already-modified sdkconfig and re-modify it
idempotently.

NOTE on the menuconfig-style ``sdkconfig`` (the parent of the
qio_qspi/include/ dir): the Arduino framework's prebuild.8.pattern
copies it to the build dir, but the Arduino framework does NOT
re-run Kconfig — the pre-built ``sdkconfig.h`` is what gets compiled.
So we have to edit the header directly.
"""
import os
import re

Import("env")  # noqa: F821  (Scons injects this)

# -----------------------------------------------------------------------------
# Override values — chosen for a NUS peripheral with one connection.
# -----------------------------------------------------------------------------
SDKCONFIG_OVERRIDES = {
    # Smaller ACL pool — one connection only.
    "CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT": "6",   # was 24
    # Smaller event pool — 30 events is enough for a central+peripheral
    # storm; one peripheral + one host needs ~4-6.
    "CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT":          "8",  # was 30
    # Smaller discardable event pool — used for ADV reports only.
    "CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT":  "4",  # was 8
    # Smaller MSYS_1 pool — general mbuf allocation for GATT writes
    # and notifications. 8 blocks × 256 B = 8 mbufs is generous for
    # one concurrent peer. Was 12 × 256 = 12 mbufs.
    "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT":          "8",  # was 12
    # Smaller MSYS_2 pool — larger mbufs for L2CAP / CoC. 16 blocks
    # × 320 B covers the one connection's worth of coc / ATT buffers.
    # Was 24 × 320 = 24 mbufs.
    "CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT":          "16", # was 24
}

# Any other macros that derive from the keys above in sdkconfig.h
# (CONFIG_BT_NIMBLE_HCI_EVT_HI_BUF_COUNT, CONFIG_BT_NIMBLE_HCI_EVT_LO_BUF_COUNT,
# CONFIG_BT_NIMBLE_ACL_BUF_COUNT, CONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT) follow
# the same pattern in the file, so editing the source keys propagates.

SDKCONFIG_HEADER_BASENAME = "sdkconfig.h"
BACKUP_SUFFIX = ".bak"

# All memory type variants for ESP32-S3. The build picks one based on
# build.memory_type (default qio_qspi). Editing the same values in all
# variants keeps the override consistent if the build picks a different
# memory type.
ESP32S3_MEMORY_TYPES = [
    "qio_qspi",
    "qio_opi",
    "opi_opi",
    "dio_opi",
    "dio_qspi",
]


def find_sdkconfig_headers():
    """Locate the framework's pre-built ``sdkconfig.h`` for every ESP32-S3
    memory type variant."""
    arduino_libs_dir = os.path.join(
        os.path.expanduser("~/.platformio/packages"),
        "framework-arduinoespressif32-libs",
        "esp32s3",
    )
    headers = []
    for mem_type in ESP32S3_MEMORY_TYPES:
        path = os.path.join(arduino_libs_dir, mem_type, "include",
                            SDKCONFIG_HEADER_BASENAME)
        if os.path.isfile(path):
            headers.append(path)
    if not headers:
        raise RuntimeError(
            "override_sdkconfig: no ESP32-S3 sdkconfig.h found under "
            f"{arduino_libs_dir}/{ESP32S3_MEMORY_TYPES[0]}/include/")
    return headers


def apply_overrides(sdkconfig_path):
    """Edit the pre-built sdkconfig.h in place. Idempotent."""
    with open(sdkconfig_path, "r", encoding="utf-8") as f:
        original = f.read()

    modified = original
    for key, new_value in SDKCONFIG_OVERRIDES.items():
        # Match the framework's `#define KEY VALUE` lines. They have no
        # guarding #ifndef so we have to be exact.
        pattern = re.compile(rf"^(#define\s+{re.escape(key)}\s+)\S+$",
                             re.MULTILINE)
        replacement = rf"\g<1>{new_value}"
        new_modified, n_subs = pattern.subn(replacement, modified, count=1)
        if n_subs != 1:
            raise RuntimeError(
                f"override_sdkconfig: expected exactly one match for "
                f"{key!r} in {sdkconfig_path}, got {n_subs}")
        modified = new_modified

    if modified == original:
        # Already overridden — likely a previous run that didn't get
        # restored, or a re-run. Leave the file alone.
        return False

    # Backup once before the first write.
    backup_path = sdkconfig_path + BACKUP_SUFFIX
    if not os.path.exists(backup_path):
        with open(backup_path, "w", encoding="utf-8") as f:
            f.write(original)

    with open(sdkconfig_path, "w", encoding="utf-8") as f:
        f.write(modified)
    return True


def main():
    for sdkconfig_path in find_sdkconfig_headers():
        changed = apply_overrides(sdkconfig_path)
        if changed:
            print(f"[override_sdkconfig] shrank BLE pools in {sdkconfig_path}")
            for key, val in SDKCONFIG_OVERRIDES.items():
                print(f"[override_sdkconfig]   {key}={val}")
        else:
            print(f"[override_sdkconfig] {sdkconfig_path} already overridden")


main()
