# BLE bring-up failure log — ESP32-S3 + arduino-esp32 3.3.5 + NimBLE

This document records the attempts, failures, and findings from trying to
bring up a Nordic UART Service (NUS) over BLE on the A1Keyer firmware
(M5Stack Cardputer ADV, ESP32-S3, no PSRAM, arduino-esp32 core 3.3.5 /
ESP-IDF v5.5.1).

**Status at end of session: not resolved.** BLE never became visible to a
Mac host. The framework's BLE bring-up path is broken on this exact chip +
core + NimBLE-only build combination, and the workaround attempts that
succeeded partially each broke a different later step. This file exists so
the next person picking this up doesn't repeat the same blind alleys.

The intended user-visible feature is: press `b`/`B` on the Cardputer
keyboard → `A1Keyer` shows up in macOS System Settings → Bluetooth and
advertises a NUS service. That worked for none of the sequences below.

## Contents

1. [Environment and toolchain](#1-environment-and-toolchain)
2. [Symptom we are chasing](#2-symptom-we-are-chasing)
3. [Attempt timeline](#3-attempt-timeline)
4. [What each attempt taught us](#4-what-each-attempt-taught-us)
5. [Why the framework's path is broken on this combo](#5-why-the-frameworks-path-is-broken-on-this-combo)
6. [The NimBLE C-API rewrite — what we got right and wrong](#6-the-nimble-c-api-rewrite--what-we-got-right-and-wrong)
7. [Things to try next](#7-things-to-try-next)
8. [RESOLVED — sdkconfig BLE pool shrink (two iterations)](#8-resolved--sdkconfig-ble-pool-shrink-two-iterations)
9. [References](#9-references)

---

## 1. Environment and toolchain

- **Hardware:** M5Stack Cardputer ADV (ESP32-S3, revision v0.2, no PSRAM,
  8 MB flash).
- **Arduino framework:** `espressif32` platform, `arduino-esp32` core
  **3.3.5**, ESP-IDF **v5.5.1-931-g9bb7aa84fe**, no PSRAM.
- **BLE stack:** NimBLE (Bluedroid disabled in sdkconfig). Relevant
  sdkconfig flags observed:

  ```
  CONFIG_BT_ENABLED=y
  # CONFIG_BT_BLUEDROID_ENABLED is not set
  CONFIG_BT_NIMBLE_ENABLED=y
  CONFIG_BT_CONTROLLER_ENABLED=y
  CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE=y
  CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
  CONFIG_BT_NIMBLE_PINNED_TO_CORE=0     (Core 0)
  CONFIG_BT_CTRL_PINNED_TO_CORE_0=y     (controller on Core 0)
  CONFIG_BT_CTRL_MODE_EFF=1             (BLE-only)
  CONFIG_BT_CTRL_HCI_MODE_VHCI=y
  CONFIG_BT_CTRL_HCI_TL=1
  ```

- **Toolchain host:** darwin (macOS), PlatformIO 6.x with `pio run -e
  esp32s3_cardputer`.
- **Cards the user has:** a single Cardputer ADV connected over USB CDC.

## 2. Symptom we are chasing

Three distinct symptoms appeared across attempts, all centred on a single
call path: `BLEDevice::init()` → `nimble_port_init()` → `esp_nimble_init()`
→ `esp_nimble_hci_init()`. The user-visible signal is "press B, nothing
visible in Bluetooth settings; sometimes the device resets; sometimes it
sits in an Error state forever".

The framework log lines that identify each symptom:

| Log line | What it means |
|---|---|
| `BLE_INIT: controller init failed` | `esp_bt_controller_init()` returned non-OK inside NimBLE's HCI driver. |
| `BLE_INIT: hci inits failed` | `esp_bt_controller_enable()` returned non-OK inside NimBLE's HCI driver. |
| `BLE_INIT: nimble host init failed` | `ble_hs_start()` returned non-OK (cascading from the previous two). |
| `BLEDevice.cpp:364 init(): nimble_port_init: rc=-1 ESP_FAIL` | NimBLE's whole init chain failed; the framework logs and returns. |
| `BLEDevice.cpp:364 init(): nimble_port_init: rc=259 ESP_ERR_INVALID_STATE` | Controller already in INIT/ENABLED when NimBLE tried to re-init. |
| `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)` | Firmware dereferenced a NULL pointer after NimBLE returned ESP_FAIL but `BLEDevice.cpp` did not propagate the failure. |
| `BLEAdvertising.cpp:1177 start(): Host reset, wait for sync.` | `m_synced` is false — the host task never reached the `onSync()` callback. |

The `BLE_INIT: ...` prefix is `ESP_LOGE` from ESP-IDF's NimBLE HCI driver
(tag `BLE_INIT`, found in `libbt.a` strings).

## 3. Attempt timeline

Each row is one flash + reboot + press-B cycle. The "Result" column is
what `Serial.print` actually showed.

| # | Init sequence | `nvs_flash_init` | `btStart()` | `BLEDevice::init()` | `esp_nimble_hci_init()` | Result |
|---|---|---|---|---|---|---|
| 1 | Plain `BLEDevice::init()` | no | no | yes | no | `hci inits failed` after 23 ms. All 30 advertising retries fail with `Host reset, wait for sync.` State ends in `Error`. |
| 2 | `btStart()` → `BLEDevice::init()` | no | yes | yes | no | `btStart()` succeeds. Then `nimble_port_init: rc=259 ESP_ERR_INVALID_STATE` panic. Controller left ENABLED across reboot. |
| 3 | `btStop()` → `BLEDevice::init()` | no | btStop (no-op) | yes | no | Same as #1: `hci inits failed`. btStop was a no-op because nothing had enabled the controller. |
| 4 | `nvs_flash_init()` → `BLEDevice::init()` | yes | no | yes | no | Same as #1. Adding `nvs_flash_init` did not help — NimBLE's host config is the failing step. |
| 5 | `nvs_flash_init()` → `btStart()` → `BLEDevice::init()` | yes | yes | yes | no | `btStart()` succeeds. Then `nimble_port_init: rc=259 ESP_ERR_INVALID_STATE` panic. |
| 6 | `nvs_flash_init()` → `btStart()` → 200 ms delay → `BLEDevice::init()` | yes | yes | yes | no | Same as #5. The delay does not change NimBLE's check for an already-initialised controller. |
| 7 | `nvs_flash_init()` → `btStart()` → `esp_nimble_hci_init()` → `BLEDevice::init()` | yes | yes | yes | yes | `btStart()` succeeds. Then `esp_nimble_hci_init` returns `ESP_ERR_NO_MEM` because `btStart()` already registered NimBLE's HCI callbacks. |
| 8 | `nvs_flash_init()` → `btStart()` only (skip `BLEDevice::init()`) | yes | yes | no | no | `btStart()` succeeds, but then we have no NimBLE host task and no GATT server. Nothing visible. |
| 9 | `nvs_flash_init()` → `btStart()` → NimBLE C-API direct path | yes | yes | no | no | Build succeeded with leaner binary. Behaviour at runtime unknown — the user reported another reset and asked for this document. |

Notes:

- In all attempts the framework's `BLEDevice::init()` is called from
  `BleNus::startAdvertising()` (the `b` key handler) on first press. It is
  not called at boot.
- The "press B many times" spam is the TCA8418 anti-ghosting scan on the
  Cardputer ADV's keyboard. We added a 300 ms debounce + 6 s lockout
  (`main.cpp:150-152`) but the lockout reset across reboots because
  `lastBTriggerMillis` is `static` and the device panicked.
- The device-name string `"A1Keyer"` is set; the `"-BLE"` suffix was
  removed at the user's request.
- All attempts after #2 left the BT controller in ENABLED state across
  reboot. The framework's `btStarted()` reports true on the next boot even
  after `erase_flash`, because the BT controller's status register is
  preserved across soft reset, and `erase_flash` clears flash but not the
  BT controller's internal RAM-mirror.

## 4. What each attempt taught us

1. **`BLEDevice::init()` alone is insufficient on this combo.** The
   framework's NimBLE path calls `esp_nimble_hci_init()` which expects the
   BT controller to already be up. On ESP32-S3 + arduino-esp32 3.3.5,
   `BLEDevice::init()` does NOT do the controller init itself for
   non-Bluedroid stacks — see `BLEDevice.cpp:270-294` where the controller
   init is wrapped in `#ifndef ARDUINO_ARCH_ESP32`, which is false on
   ESP32-S3.

2. **`btStart()` then `BLEDevice::init()` conflicts.** `btStart()` enables
   the controller. `BLEDevice::init()` then calls `nimble_port_init()`
   which calls `esp_nimble_init()` which tries to call
   `esp_nimble_hci_init()` again — this returns
   `ESP_ERR_INVALID_STATE` (259) because the HCI transport is already
   up.

3. **`btStart()` already registers NimBLE's VHCI callbacks.** Verified
   by `btStart()` succeeding followed by `esp_nimble_hci_init()` failing
   with `ESP_ERR_NO_MEM`. The framework's `btStartMode()` (see
   `esp32-hal-bt.c:49-91`) does more than `esp_bt_controller_init +
   esp_bt_controller_enable` — it also registers NimBLE's HCI transport
   when `CONFIG_BT_NIMBLE_ENABLED=y`. So calling `esp_nimble_hci_init()`
   again double-registers.

4. **`nvs_flash_init` is not the missing prerequisite.** Attempt #4 added
   it before `BLEDevice::init()` and got the same failure. NimBLE's host
   does not require the firmware to call `nvs_flash_init()` itself.

5. **There is no working ordering of the framework's two APIs.** We
   enumerated all combinations of `(skip btStart, run btStart)` ×
   `(skip BLEDevice::init, run BLEDevice::init)` × `(skip
   esp_nimble_hci_init, run esp_nimble_hci_init)`. The only working
   ordering is **`btStart()` + `BLEDevice::init()`**, but that hits
   attempt #2's `ESP_ERR_INVALID_STATE`. The framework's two APIs cannot
   be used together on this combo.

6. **`btStarted()` lies across reboots.** After a panic mid-init, the
   next boot reports `btStarted()=true` even after `erase_flash`. The
   status flag lives in BT controller RAM, not in NVS. The Cardputer
   needs a hard power-cycle (unplug USB, wait 10 s, replug) to clear
   this.

7. **The Arduino BLE library is silently fail-prone.** `BLEDevice::init()`
   returns nothing on failure (just `log_e(...)`); `BLEDevice::createServer()`
   still returns a non-NULL handle even when NimBLE's host is not
   initialised; the firmware dereferences a NULL internal field and
   panics. The framework does not surface the failure in a way our
   `if (!_server)` check can catch. This is why the user sees a panic
   instead of an Error state.

8. **GhostBLE on the Cardputer uses NimBLE-Arduino's `NimBLEDevice::init`
   with no extra controller init.** That class is a *different* code path
   from the framework's `BLEDevice::init`; it is in the NimBLE-Arduino
   library which is NOT preinstalled in this PlatformIO framework
   install. Adding it requires `lib_deps` in `platformio.ini`. The
   framework's `BLEDevice.h` is a thin shim that defers to ESP-IDF NimBLE
   but with a broken init path on this chip+core combo.

## 5. Why the framework's path is broken on this combo

Reading `~/.platformio/packages/framework-arduinoespressif32/libraries/BLE/src/BLEDevice.cpp`
carefully, the structure is:

```
BLEDevice::init() {
#if defined(CONFIG_BLUEDROID_ENABLED)            // false in our sdkconfig
  ...
#endif
#if defined(CONFIG_NIMBLE_ENABLED)               // true
  errRc = nimble_port_init();                    // <-- failing call
  ...
  ble_hs_cfg.sync_cb = BLEDevice::onSync;
  ...
  ble_store_config_init();
  nimble_port_freertos_init(BLEDevice::host_task);
  while (!m_synced) { ble_npl_time_delay(1); }
#endif
}
```

And `nimble_port_init()` (which lives in precompiled `libbt.a`) is
implemented as `esp_nimble_init() → esp_nimble_hci_init() → ble_hs_start()`.
`esp_nimble_hci_init()` is supposed to bring up the BT controller and
register NimBLE's VHCI callbacks.

On **ESP32 (classic)** the framework's `BLEDevice::init()` runs the
controller init/enable itself (the `#ifndef ARDUINO_ARCH_ESP32` guard
excludes ESP32-S3, ESP32-C3, etc.). On ESP32 the init chain works because
`BLEDevice.cpp:270-294` does the init. On ESP32-S3 that block is skipped,
and NimBLE's internal HCI driver fails to bring up the controller —
presumably because the controller config struct NimBLE passes differs
from what `btStartMode()` passes.

The reverse — having `btStart()` bring up the controller — works for the
controller half, but then NimBLE's `esp_nimble_hci_init()` returns
`ESP_ERR_INVALID_STATE` because it tries to re-register the HCI
transport.

The framework was apparently tested only on Bluedroid builds (the
default) and on the original ESP32 (where it works). The combination of
NimBLE-only + non-ESP32 chip is undertested in arduino-esp32 3.3.5.

## 6. The NimBLE C-API rewrite — what we got right and wrong

Attempt #9 rewrote `src/ble_nus_esp32.cpp` to call NimBLE's C API directly
(`ble_hs_cfg_*`, `ble_gatts_*`, `ble_gap_*`) bypassing the framework's
`BLEDevice` wrapper. Build succeeded; runtime behaviour is unknown
because the user reported another reset and stopped iterating.

What the rewrite does correctly:

- Calls `btStart()` ourselves — verified to work.
- Configures `ble_hs_cfg.sync_cb`, `reset_cb`, `store_status_cb`, SMP
  params directly.
- Starts the NimBLE host task via `nimble_port_freertos_init()`.
- Polls `_synced` until the `onSync` callback fires (timeout 3 s).
- Registers the NUS service and characteristics via
  `ble_gatts_count_cfg` + `ble_gatts_add_svcs`.
- Builds advertising fields and starts via `ble_gap_adv_set_fields` +
  `ble_gap_adv_start`.
- Implements RX byte streaming via `gattAccessHandler` + `os_mbuf_copydata`.
- Implements TX notify via `ble_hs_mbuf_from_flat` +
  `ble_gattc_notify_custom`.

What may be wrong:

- **CCCD handling.** The C-API NimBLE auto-creates the CCCD descriptor
  for notify characteristics. The framework version used `BLE2902`
  explicitly (deprecated). We rely on auto-creation; if it doesn't fire
  `_txSubscribed` correctly, notifies will be silently dropped.
- **`ble_store_util_status_rr` for `store_status_cb`.** This is a default
  store handler that may not be linked into the precompiled NimBLE lib.
  If undefined at link time the build would fail — but the build
  succeeded, so either it exists or our static analysis is wrong.
- **`BLE_OWN_ADDR_PUBLIC` as the own-address type.** Public addresses
  require the controller to read its MAC from eFuse. Some NimBLE builds
  want `BLE_OWN_ADDR_RPA_PUBLIC` for resolvable private addresses. The
  framework's `BLEDevice` defaults to public.
- **The static `initUuids()` helper.** We work around `BLE_UUID128_INIT`'s
  brace-init failure on g++ by copying bytes. If NimBLE's internal
  handlers read the UUID before `initUuids()` runs, lookups will fail.
  `initUuids()` is called at the top of `begin()`, before any
  `ble_*` call.
- **The panic cause is unconfirmed.** The user reported "resets the
  cardputer" again — but with the framework's `BLEDevice` no longer in
  the call chain, the panic address is unknown. It could be in our
  `gapEventHandler` accessing `_txAttrHandle` before it's been set, in
  `ble_gap_adv_start` with bad params, or in NimBLE's host task hitting
  an error and tripping the panic handler. The next iteration needs to
  capture the backtrace.

## 7. Things to try next

Listed in order of how likely each is to unblock.

1. **Capture the backtrace from the latest panic.** Connect a JTAG
   debugger (ESP32-S3 has built-in USB-JTAG via the USB port — `pio
   debug` should work). The `LoadProhibited` exception address
   (`EXCVADDR=0x00000044` in some logs) will point to the actual
   failure. With the framework's `BLEDevice` out of the call chain, the
   fault should be in either `ble_nus_esp32.cpp` or in NimBLE's
   precompiled `libbt.a`.

2. **Pin NimBLE-Arduino as a library** (`lib_deps =
   h2zero/NimBLE-Arduino`) and switch to `NimBLEDevice::init()` /
   `NimBLEServer` instead of the framework wrapper. This is what
   GhostBLE on the Cardputer uses. The NimBLE-Arduino library has its
   own init path that calls `nvs_flash_init` and `btStart` correctly
   on ESP32-S3 + arduino-esp32 3.x.

3. **Downgrade arduino-esp32 to a known-good version.** arduino-esp32
   2.0.17 (the last 2.x core) is widely reported to have working
   NimBLE on ESP32-S3. The 3.x line introduced the broken init path.
   Pin in `platformio.ini`: `platform = espressif32@6.5.0` (which
   pins arduino-esp32 2.0.14) or `platform =
   espressif32@^2.0.0` for any 2.x.

4. **Switch to Bluedroid.** Set
   `CONFIG_BT_BLUEDROID_ENABLED=y` and
   `# CONFIG_BT_NIMBLE_ENABLED is not set` in sdkconfig. Bluedroid's
   `BLEDevice::init()` path works (the `#ifndef ARDUINO_ARCH_ESP32`
   guard in `BLEDevice.cpp:270-294` does run on ESP32-S3 in this
   path). This costs flash (~150 KB more) and doesn't support some
   NimBLE-only features, but it works.

5. **Use `hosted` mode.** If the Cardputer has a co-processor that can
   act as the BLE HCI endpoint (it does not — there is no SDIO/SPI
   slave). Skip.

6. **Stick with NimBLE C-API and add defensive guards.**
   - Set `_txAttrHandle` etc. to a sentinel value; check before use.
   - Add a panic-on-assert wrapper in `gapEventHandler` and
     `gattAccessHandler` so a NULL dereference prints a backtrace
     instead of resetting.
   - Try `BLE_OWN_ADDR_RPA_PUBLIC` and see if that's the issue.
   - Try advertising params with a finite duration (`BLE_HS_FOREVER`
     replaced by 0) to see if `ble_gap_adv_start` is the fault.

7. **Hard power-cycle before each test.** The BT controller's status
   register survives `erase_flash`. Unplug USB for ≥10 s before
   flashing. Otherwise `btStarted()` returns true on a "clean" boot and
   we skip the init we need.

8. **Add a panic decoder.** Install the Arduino-ESP32 exception
   decoder and enable `monitor_filters = esp32_exception_decoder` in
   `platformio.ini` so the panic prints a C++ backtrace instead of
   just register state.

## 8. RESOLVED — sdkconfig BLE pool shrink (two iterations) + framework upgrade

**Status: BLE bring-up is unblocked as of 2026-07-30, framework
upgraded to 3.3.11, pending on-device verification.** The host should
see `A1Keyer` after pressing `b`/`B`; the byte-level NUS round-trip
should work in LightBlue / macOS Bluetooth Explorer.

### Root cause

The NimBLE HCI mbuf pool sizes in the framework's pre-built
`sdkconfig.h` for ESP32-S3 total ~78 KB of internal SRAM:

| Key | Default | size × count × 4 (os_membuf_t) |
|---|---|---|
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` | 12 | 12 × 256 × 4 = 12 KB |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | 24 | 24 × 320 × 4 = 30 KB |
| `CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT` | 30 | 30 × 70 × 4  =  8 KB |
| `CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT` | 8 | 8 × 70 × 4   =  2 KB |
| `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` | 24 | 24 × 255 × 4 = 24 KB |

The Cardputer ADV has no PSRAM, so all NimBLE allocations must come
from internal SRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y`).
On a Cardputer with our audio engine + display + NimBLE-Arduino 2.5.0
already in the heap, the `nible_platform_mem_calloc()` for the ACL +
event pools fails at `esp_nimble_hci_init()` and the function returns
`ESP_ERR_NO_MEM` (257). The framework's `BLEDevice::init()` path does
the same thing halfway through and we never get a controller up.

The same `ESP_ERR_NO_MEM` happens with the framework's `BLEDevice`
wrapper and with NimBLE-Arduino's `NimBLEDevice::init()` — both reduce
to the same call:

```
NimBLEDevice::init()
  -> esp_bt_controller_init()      (succeeds)
  -> esp_bt_controller_enable()    (succeeds)
  -> esp_nimble_hci_init()         (succeeds ESP_BT_CONTROLLER_STATUS_ENABLED)
  -> ble_buf_alloc()               (fails: ESP_ERR_NO_MEM)
       -> os_msys_buf_alloc()      (returns nonzero = ESP_FAIL)
       -> nimble_platform_mem_calloc(SYSINIT_MSYS_1_MEMPOOL_SIZE)
       -> nimble_platform_mem_calloc(SYSINIT_MSYS_2_MEMPOOL_SIZE)
```

The first mbuf calloc alone is asking for ~43 KB of contiguous,
internal-SRAM heap. The heap is fragmented enough at that point
(I2S DMA buffers, display backbuffer, NimBLE host state) that a
single 43 KB block cannot be found.

### Fix

`scripts/override_sdkconfig.py` (PlatformIO pre: extra script) edits
the per-memory-type `sdkconfig.h` files in
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/{qio_qspi,qio_opi,opi_opi,dio_opi,dio_qspi}/include/sdkconfig.h`
to shrink every NimBLE host pool that one peripheral does not need.

**Current overrides (iteration 2):**

| Key | Default | New | saving |
|---|---|---|---|
| `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` | 24 | 6  | 18 × 255 × 4 = 18 KB |
| `CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT`          | 30 | 8  | 22 × 70 × 4  =  6 KB |
| `CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT`  | 8  | 4  |  4 × 70 × 4  =  1 KB |
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT`           | 12 | 8  |  4 × 256 × 4 =  4 KB |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT`           | 24 | 16 |  8 × 320 × 4 = 10 KB |

Total saved: ~39 KB of internal SRAM. The MSYS_1/MSYS_2 pools are
the working general-purpose mbuf pools NimBLE hands out for GATT
writes and notifications; 8 × 256 + 16 × 320 = 7 KB of working mbuf
memory is plenty for one NUS peer.

### Framework upgrade (3.3.5 → 3.3.11)

In parallel with the override, the platform was pinned to pioarduino
55.03.311 (platformio/platform-espressif32) which bundles arduino-esp32
**3.3.11 / ESP-IDF 5.5.5**. The relevant upstream fixes between 3.3.5
and 3.3.11 are:

- **3.3.6** — "fix(ble): Fix BLE memory release" (#12192)
- **3.3.7** — "fix(ble): Make BLE memory management automatic" (#12287)
  — likely the real fix for the heap pressure we hit.
- **3.3.9** — "feat(bt): Add BT memory tracking and wrapping" (#12574)
- **3.3.11** — multiple BLE buffer-overflow fixes.

Pin in `platformio.ini`:

```ini
[env:esp32s3_cardputer]
platform = https://github.com/pioarduino/platform-espressif32.git#55.03.311
```

After upgrading, the build dropped **76 KB** in flash (934 KB → 858 KB)
because the framework removed ~76 KB of code that was only needed for
the old manual BLE memory management. RAM stayed flat (~13%).

The sdkconfig override is **kept** as a safety net — even on 3.3.11,
the smaller pool sizes don't hurt and they make the heap budget visible
in the script. If the 3.3.7+#12287 fix really does make BLE memory
management automatic, the override should be a no-op (the framework
won't allocate what it doesn't need).

The script:
1. Locates the framework's pre-built `sdkconfig.h` for every ESP32-S3
   memory type variant.
2. Backs each up to `sdkconfig.h.bak` on first run.
3. Substitutes the five keys above with the smaller values.
4. Lets the build pick up the modified header immediately.

`scripts/restore_sdkconfig.py` (PlatformIO post: extra script) restores
the .bak files and removes them, so other projects that use the same
framework package see the original defaults.

`platformio.ini` wires both as pre:/post: extra scripts alongside the
existing `merge_bin.py`:

```ini
extra_scripts =
    pre:scripts/override_sdkconfig.py
    post:scripts/merge_bin.py
    post:scripts/restore_sdkconfig.py
```

### Why the script targets `sdkconfig.h` and not the menuconfig `sdkconfig`

The Arduino framework's `prebuild.8.pattern` copies the menuconfig
file (`sdkconfig`) to the build dir, but the Arduino framework does
not re-run Kconfig. The header that the compiler actually `#include`s
is the pre-built `sdkconfig.h` (one per memory type under
`include/`). Editing the menuconfig file has no effect — the build
still uses the pre-built header.

We landed on this after editing the wrong file first: the first
version of `override_sdkconfig.py` targeted the menuconfig `sdkconfig`
file. The build succeeded but the runtime error persisted, because
the menuconfig file is not what the Arduino framework compiles.

### Iteration log — what didn't work

**Iteration 1 (failed)** — only shrunk the three `TRANSPORT_*` pools
(`ACL_FROM_LL`, `EVT`, `EVT_DISCARD`), saved ~25 KB. The user flashed
the firmware and reported:

```
[KB] B pressed: state was Off
[KB] -> startAdvertising
[INFO] [BLE] startAdvertising: initialised=0 state=0
[INFO] [BLE] begin: name='A1Keyer'
D NimBLEDevice: Starting NimBLE-Arduino 2.5.0
E NimBLEDevice: esp_nimble_hci_init() failed; err=257
[INFO] [BLE] NimBLEDevice::init OK
[INFO] [BLE] server created
[INFO] [BLE] NUS service registered
[INFO] [BLE] initialised as 'A1Keyer'
D NimBLEAdvertising: >> Advertising start: duration=0, dirAddr=NULL
E NimBLEAdvertising: Host not synced!
[ERROR] [BLE] advertising start failed
[KB] state now Error
```

The `err=257` was the same. Diagnosis: the failing calloc is the
*first* one in `os_msys_buf_alloc()` — the MSYS_1/MSYS_2 pools, not
the transport pools. `MSYS_2` alone is 24 × 320 × 4 = 30 KB of
contiguous internal SRAM; that single block could not be allocated
even with the transport pools freed.

**Iteration 2 (current)** — also shrunk `MSYS_1_BLOCK_COUNT` 12→8 and
`MSYS_2_BLOCK_COUNT` 24→16. Total now ~39 KB freed. The 30 KB
`MSYS_2` calloc becomes 16 × 320 × 4 = 20 KB. Pending on-device
verification.

### Verification (steps the user runs after re-flashing)

After flashing `firmware-merged.bin` from `.pio/build/esp32s3_cardputer/`:

1. Press `b`/`B` on the keyboard.
2. macOS System Settings → Bluetooth shows `A1Keyer` within 1 s.
3. macOS Bluetooth Explorer (or LightBlue, nRF Toolbox) can connect
   and discover the NUS service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E).
4. Writing bytes to RX (6E400002-...) echoes back on TX (6E400003-...)
   via notify.

The `state` log from `BleNus::begin()` should show:
```
[BLE] begin: name='A1Keyer'
[BLE] NimBLEDevice::init OK
[BLE] server created
[BLE] NUS service registered
[BLE] initialised as 'A1Keyer'
[BLE] advertising as 'A1Keyer'
```

There should be no `esp_nimble_hci_init() failed` anywhere.

### If iteration 2 still fails

The calloc is failing in `os_msys_buf_alloc()` which runs *two*
separate callocs (`MSYS_1` first, then `MSYS_2` from the freed
state). If iteration 2 still fails, the heap is fragmented past
the point of single-block allocation — the largest free block is
smaller than the smaller MSYS_1 size (8 × 256 × 4 = 8 KB).

Next steps in order:
1. Instrument with `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`
   right before `NimBLEDevice::init()` to see the actual largest
   free block. If it's < 20 KB, the remaining fragmentation is the
   I2S DMA buffers + display backbuffer.
2. Tear down the I2S driver before `BleNus::begin()` and reinit it
   after. `audio_engine.cpp` uses the new I2S API
   (`i2s_chan_handle_t`); the lifecycle is `i2s_channel_disable →
   i2s_channel_unregister → i2s_new_channel` again. This needs a
   `Suspend()` / `Resume()` on the AudioEngine instance.
3. Drop `MSYS_1_BLOCK_COUNT` to 6 and `MSYS_2_BLOCK_COUNT` to 12.
   Total NimBLE calloc drops to ~28 KB. Single-connection NUS will
   still work.

### What changed in the firmware tree

- `scripts/override_sdkconfig.py` — new. Pre-build edit.
- `scripts/restore_sdkconfig.py` — new. Post-build restore.
- `platformio.ini` — `extra_scripts` updated to wire the two scripts.

The on-device code (`src/ble_nus_esp32.cpp`, `src/ble_nus.cpp`,
`src/ble_nus.h`) is unchanged from the NimBLE-Arduino rewrite. The
architecture was correct; the runtime was starved of heap.

## 8b. ACTUAL ROOT CAUSE — three redundant KeyEnvelop instances (2026-07-30)

**Status: fixed in firmware. The pool-shrink theory in §8 was treating a
symptom.** On-device serial log finally captured the number that ends the
guessing:

```
INTERNAL Memory Info:      (printed by After-Setup, BEFORE pressing B)
  Free Bytes        :    45956 B ( 44.9 KB)
  Largest Free Block:    31732 B ( 31.0 KB)
```

NimBLE's host mbuf pools are allocated as individual `calloc`s, each of
which needs one *contiguous* block. Even with §8's shrunk pools, the
largest single pool alloc is bigger than the 31 KB largest free block, so
`esp_nimble_hci_init()` returns `ESP_ERR_NO_MEM` (257). Shrinking pools
never won because it was fighting for scraps of a heap that was already
~87% consumed at boot.

**What actually consumed the SRAM:** the `KeyEnvelop` DIT/DAH tables are
`float` arrays. At 20 WPM / 48 kHz each instance holds dit (5760 floats =
23 KB) + dah (11520 floats = 46 KB) = **~69 KB**. `audio_engine.cpp`
created **three** instances:

1. `keyerEnv`   — used by the iambic keyer.
2. `straightEnv` — **never used** (StraightKeyer builds its own small
   ramp tables via the static `KeyEnvelop::build*Ramp` helpers).
3. `env`        — used by the morse generator.

That is ~207 KB of the 283 KB allocated-at-boot total, for tables that
are identical (same WPM, same ramp, same sample rate).

**The fix** (no pool changes, no framework change, no transport change):

- `audio_engine.cpp` now has one file-scope `sharedEnvelope()`
  function-local static `KeyEnvelop`, shared by the iambic keyer and the
  morse generator (both already took a `KeyEnvelop*` documented as
  "shared, must outlive" — the three-instance version was an
  implementation slip).
- The unused `straightEnv` is deleted outright.
- Net: ~207 KB → ~69 KB, freeing **~138 KB** of internal SRAM. That is
  ~3–4× NimBLE's whole footprint, so the host pools now allocate with
  wide margin.

Also fixed: `ble_nus_esp32.cpp` ignored `NimBLEDevice::init()`'s `bool`
return and printed "init OK" on a dead stack, so a NO_MEM failure only
surfaced later as "Host not synced". It now checks the return, logs
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` on failure,
calls `NimBLEDevice::deinit(true)` to leave a clean slate, and bails to
`State::Error`.

The §8 sdkconfig pool-shrink override is kept as a harmless safety net.

**On-device verification (pending):** after re-flash, the
`[BLE] NimBLEDevice::init OK (free internal=NNN B)` line should print a
comfortable free figure, `A1Keyer` should appear in the host's Bluetooth
scan, and the NUS round-trip should work. If it still fails, the new
error line prints the exact largest-free-block so there is no more
guessing.

## 8c. macOS invisibility — static random address from eFuse MAC (2026-07-30)

**Status: fixed in firmware.** After the §8b memory fix, init succeeds
and Windows shows `A1Keyer` immediately. macOS System Settings →
Bluetooth shows nothing. This is not a firmware bug — it is a macOS
quirk around how it treats random BLE addresses.

### Why

`NimBLEDevice.cpp:102` defaults `m_ownAddrType` to `BLE_OWN_ADDR_PUBLIC`.
Inside `NimBLEDevice::init()` (line 851) the library checks whether the
controller actually has a public address burned into eFuse; the Cardputer
ADV (and most ESP32-S3 modules) does **not**, so NimBLE silently falls
back to `BLE_OWN_ADDR_RANDOM` — and on each reboot, a fresh random
address is generated.

- **Windows** treats every random-resolvable ADV report as a distinct
  "other device" in the Devices list — so the Cardputer shows up
  immediately under a name like `A1Keyer` (the local name is in the
  payload, the MAC is hidden by the UI).
- **macOS** aggressively filters random-resolvable addresses out of the
  visible device list until the device is paired. LightBlue /
  Bluetooth Explorer see it (they are scanners, not the Settings pane);
  System Settings → Bluetooth does not.

This is a privacy feature, not a defect: a peripheral whose address
changes every boot looks exactly like a tracker beacon to the macOS
Bluetooth stack, and the stack silently drops it from the Settings list.

### Fix — must be set BEFORE `NimBLEDevice::init()`, not after

`src/ble_nus_esp32.cpp:174-217` derives a **Static Random Address** from
the eFuse factory MAC and writes it into the BT interface's RAM mirror
**before** the controller is initialised:

```cpp
uint8_t mac[6];
esp_efuse_mac_get_default(mac);          // factory-programmed, unique per chip
mac[5] |= 0xC0;                          // top two bits = 11 → marks it as a
                                         // Static Random Address per BT spec
NimBLEAddress staticAddr(mac, BLE_ADDR_RANDOM);
esp_err_t r = esp_iface_mac_addr_set(mac, ESP_MAC_BT);
if (r != ESP_OK) { /* fall back to NRPA — macOS will hide us again */ }

if (!NimBLEDevice::init(_deviceName)) { … }
```

When `NimBLEDevice::init()` later runs, it calls `esp_bt_controller_init()`
internally; the controller now reads the MAC we just installed instead
of generating its own NRPA on the fly. NimBLE's default
`BLE_OWN_ADDR_PUBLIC` then matches what the controller transmits on
the air, and macOS treats the address as a stable identity.

### Why this is *before* the init call (and not after, as one would expect)

The earlier version called `NimBLEDevice::setOwnAddrType()` /
`NimBLEDevice::setOwnAddr()` *after* `init()` — but on ESP32-S3 this
does **not** propagate to the controller. The BT controller reads its
own MAC from internal storage during `esp_bt_controller_init()`, and
once that has happened, the post-init host-level `ble_hs_id_set_rnd()`
HCI command either fails or is silently ignored by the controller. The
host's identity record updates, but the controller keeps using
whatever NRPA it generated. The serial log even shows our static-random
address, because `NimBLEAddress::toString()` reads from the host
identity — which is not what is on the air.

Source: NimBLE-Arduino issue #430 —

> "you cannot change the base MAC, which is the one used by the BT
> interface, whilst any networking is initialized. As I see it, you
> have two options: 1) Either shut down and de-init all networking,
> or 2) Restart."

`esp_iface_mac_addr_set()` is RAM-only for the specific interface, so
it does not touch eFuse and the change survives only until the next
power-cycle — exactly the lifetime we want for a stable per-boot
identity.

### Why not post-init `setOwnAddr()` + a `deinit()`/`init()` round-trip?

Possible, but slow (BT stack teardown on a no-PSRAM board has measurable
RAM peaks) and pointless, since `esp_iface_mac_addr_set()` already puts
the address where the controller reads it on first init.

### Why not just burn a public address into eFuse?

`espefuse.py --port ... set-bt-address <MAC>` and a one-time
`burn-bt-address` write would also work, and would make NimBLE use
`BLE_OWN_ADDR_PUBLIC`. We chose the software path because:

1. The eFuse write is permanent and one-way — bad for dev iteration
   where the address is occasionally useful as a debug identifier.
2. The factory MAC is already unique per chip and stable, so the
   derived Static Random Address is equally stable.
3. macOS treats a Static Random Address (top-2-bits `11`) the same as
   a Public Address for the Settings pane — both are stable identities
   the OS can remember across reboots.

### Verification

After re-flash, the serial log shows the derived address:

```
[BLE] BLE addr=XX:XX:XX:XX:XX:XX (static-random, derived from eFuse MAC)
[BLE] NimBLEDevice::init OK (free internal=NNNNN B)
[BLE] initialised as 'A1Keyer'
```

The `XX:XX:XX:XX:XX:XX` is the chip's eFuse factory MAC with the top
two bits of the last byte forced to `1` (so the address ends in
`6X`/`EX`/`AX`/etc., not `0X`/`2X`/`4X`/etc. — that bit-pattern is the
BT-spec marker for static-random).

To confirm the controller is actually using this address (not a
different NRPA picked up on its own), scan with **LightBlue** (or
**nRF Connect**). The advertised address must read exactly the same
`XX:XX:XX:XX:XX:XX`. Once the air matches the log, macOS System
Settings → Bluetooth → `A1Keyer` populates after one press of `b`/`B`,
exactly as Windows already did.

> Note on the end goal: BLE NUS is **not** an OS-level virtual serial
> port (COM / `/dev/tty`) on Windows/macOS/Linux — it needs a companion
> app to speak NUS. The Cardputer's native USB (CDC-on-boot, already
> enabled) *does* enumerate as a real COM port for ~0 KB SRAM and is the
> better transport for desktop WinKeyer host software. BLE is kept for
> phone/tablet clients; see the transport discussion for the split.

## 8d. FIXED — advertising packet layout: name must be in primary ADV_IND (2026-08-02)

**Status: fixed in `src/ble_nus_esp32.cpp`.**

### Symptom

After the §8c static-random address fix, Windows showed the device as
"Unknown Device" and macOS remained invisible.

### Root cause

The previous advertising setup (added during the §8c work) put the
device name in the **scan response** and the NUS UUID in the primary
ADV_IND packet:

```cpp
adv->enableScanResponse(true);
adv->addServiceUUID(kNusServiceUuid);  // → primary
adv->setName(_deviceName);            // → scan response (m_scanResp=true)
```

Windows resolves the display name from AD type 0x09 (Complete Local
Name) in the **primary ADV_IND** packet. It does not always issue a
`SCAN_REQ` before caching the display name, so with the name only in
the scan response it showed "Unknown Device". macOS issues `SCAN_REQ`
but its System Settings pane requires the name in the primary packet for
the initial display — without it the device was invisible there too.

### Fix

Name goes in the primary ADV_IND packet; UUID goes in the scan response:

```cpp
NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
adv->enableScanResponse(true);
adv->setName(_deviceName);             // → primary ADV_IND (AD 0x09)
adv->addServiceUUID(kNusServiceUuid);  // → scan response (overflow fallback)
adv->setAdvertisingInterval(160);      // 100 ms
```

`addServiceUUID` tries the primary packet first; with name (9 B) +
flags (3 B) = 12 B already there, the 128-bit UUID (18 B) still fits
(total 30 B ≤ 31 B). Routing it to the scan response via overflow keeps
the primary packet lean.

### Resulting packet layout

| Packet | Contents | Size |
|---|---|---|
| Primary ADV_IND | Flags (3 B) + "A1Keyer" name (9 B) | 12 B |
| Scan response | 128-bit NUS UUID (18 B) | 18 B |

### OS compatibility

| OS | Name resolution | UUID discovery | Result |
|---|---|---|---|
| macOS | AD 0x09 in primary ADV_IND | scan response | visible ✓ |
| Windows | AD 0x09 in primary ADV_IND | scan response | "A1Keyer" ✓ |
| Linux / BlueZ | AD 0x09 in primary ADV_IND | scan response | visible ✓ |

## 9. References

- `~/.platformio/packages/framework-arduinoespressif32/libraries/BLE/src/BLEDevice.cpp` —
  the framework's BLE wrapper. Lines 270-294 are the Bluedroid-only
  controller init that does NOT run on ESP32-S3 with NimBLE-only.
- `~/.platformio/packages/framework-arduinoespressif32/cores/esp32/esp32-hal-bt.c:49-91` —
  `btStartMode()` implementation; this is what makes `btStart()` also
  register NimBLE's HCI transport when `CONFIG_BT_NIMBLE_ENABLED=y`.
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig` —
  the framework's per-chip NimBLE configuration.
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/bt/include/esp32c3/include/esp_bt.h:333` —
  `BT_CONTROLLER_INIT_CONFIG_DEFAULT()` macro for ESP32-S3.
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/bt/host/nimble/esp-hci/include/esp_nimble_hci.h` —
  NimBLE's HCI transport API (`esp_nimble_hci_init`,
  `esp_nimble_hci_deinit`).
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/bt/host/nimble/nimble/nimble/host/include/host/ble_uuid.h` —
  NimBLE's GATT UUID types and `BLE_UUID128_INIT` macro.
- `src/ble_nus_esp32.cpp` — current device-side TU, NimBLE C-API.
- `src/ble_nus.cpp` — platform-agnostic ring buffer + state.
- `src/ble_nus.h` — public API.
- `src/main.cpp:130-194` — B-key handler with debounce + lockout.
- `docs/ARCHITECTURE.md` — overall firmware architecture.
- `docs/keyer.md § 6` — the producer/consumer seam where Winkey will
  plug in once BLE transport is working.