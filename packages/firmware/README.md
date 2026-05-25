# Doudou Firmware

ESP32-C3 firmware for the Doudou device. Target board: **ESP32-2424S012**
(ESP32-C3-MINI-1U + GC9A01 1.28" round LCD).

This is the *MVP-1a* skeleton: display bring-up only. Wi-Fi / WebSocket /
LVGL / touch land in later milestones — see
[`docs/technical-plan.md` 开发里程碑](../../docs/technical-plan.md#开发里程碑).

## Build

Prerequisites (already installed on this machine — see top-level `CLAUDE.md`):
- ESP-IDF v5.3 at `~/esp/esp-idf/`
- Toolchain at `~/.espressif/`
- USB-C cable + the board powered

Each shell session, source the IDF environment **once**:

```bash
. ~/esp/esp-idf/export.sh
```

Then from `packages/firmware/`:

```bash
idf.py set-target esp32c3        # first time only
idf.py build                     # compiles bootloader + app
idf.py -p /dev/cu.usbmodem* flash monitor   # flash + serial log
```

Last verified build: `doudou.bin` ≈ **217 KB** (ota_0 partition 14% used).

The first `set-target` pulls `espressif/esp_lcd_gc9a01` via the IDF
Component Registry — see [`main/idf_component.yml`](main/idf_component.yml).

## What it does (MVP-1b — real Codex data over WebSocket)

`app_main` brings up:

1. GC9A01 panel + backlight
2. CST816D touch controller (I2C)
3. LVGL v9 with partial 240×40 double-buffered DMA flush
4. 4-screen pet UI mirroring the browser simulator:
   `INFO ← USAGE ← PET → HISTORY`
5. Wi-Fi STA (from `CONFIG_DOUDOU_WIFI_*` set via `idf.py menuconfig`)
6. mDNS resolution of `doudou-bridge.local` → IP
7. WebSocket client connecting to `ws://<bridge>:8788/device`
8. Device protocol v1 parser/serializer (cJSON-based):
   - parses `welcome` / `status` / `session_info` / `usage` / `thread_list` / `error` / `ping`
   - sends `hello` / `pong` / `reply` / `follow_thread`

Interactions:

- **Slide left / right** on the touch screen → switch screen
- **Tap pet** on the PET screen → wiggle reaction
- **Tap a thread** on the HISTORY screen → sends `follow_thread`, Bridge
  re-anchors RolloutWatcher to that thread; refreshed list comes back
- The pet always **breathes** (subtle scale tween, infinite)

## First-time setup

```bash
. ~/esp/esp-idf/export.sh
cd packages/firmware
idf.py menuconfig                # Doudou → set Wi-Fi SSID + Password
idf.py -p /dev/cu.usbmodem* flash monitor
```

If Wi-Fi creds are blank the firmware still boots, but the pet sits idle
with "Wi-Fi 未配置" on the title.

## Resource budget snapshot (MVP-1b)

| | Used | Budget |
|---|---:|---:|
| `doudou.bin` | 894 KB | 1.5 MB (ota_0) — **42% free** |
| LVGL static | 391 KB | — |
| Wi-Fi + lwip + websocket + mdns + cJSON | ~340 KB additional | — |
| LVGL mem pool | 48 KB | 400 KB SRAM |
| Draw buffers (2 × 240×40×2) | 38 KB | — |
| Tasks (lvgl + touch + ws + lwip + wifi) | ~30 KB | — |
| **Free SRAM heap (predicted)** | | ~250 KB available at runtime |

## Pin assignments — verified

See [`main/pins.h`](main/pins.h). Pins were cross-checked against the
vendor's official Arduino sample at
[`docs/1.28inch_ESP32-2424S012/1-Demo/Demo_Arduino/3_1-TFT-LVGL-Benchmark/`](../../docs/1.28inch_ESP32-2424S012/1-Demo/Demo_Arduino/3_1-TFT-LVGL-Benchmark/),
which is the schematic-of-record for this board.

Summary:

| Signal | GPIO | Notes |
|---|---:|---|
| LCD SCLK | 6 | SPI2_HOST, 80MHz |
| LCD MOSI | 7 |  |
| LCD DC | 2 |  |
| LCD CS | 10 |  |
| LCD RST | — | tied on PCB |
| LCD BL | 3 | GPIO HIGH enables backlight (no PWM in vendor demo) |
| Touch SDA | 4 | CST816D @ 0x15 |
| Touch SCL | 5 |  |
| Touch RST | 1 |  |
| Touch INT | 0 |  |

LCD panel needs `invert_color = true` + BGR pixel order.

## Partition table

[`partitions.csv`](partitions.csv) is the build-time source; it mirrors
[`docs/firmware/partitions.csv`](../../docs/firmware/partitions.csv)
(documentation copy). When updating, **change docs/ first** and copy
here — the protocol layout is locked, see
[partitions.md](../../docs/firmware/partitions.md) for invariants.

## Next steps

1. **MVP-1a step 3** — replace the color-fill loop with LVGL flush
   (partial buffer 240×40). Display 4 static screens (idle / thinking /
   waiting / error) without touch.
2. **MVP-1b** — add Wi-Fi + `esp_websocket_client`, connect to
   `ws://doudou-bridge.local:8788/device`. Implement `hello` / `welcome`
   handshake + `status` rendering.
3. **MVP-1c** — touch driver (once chip model is confirmed).

Each step has acceptance criteria in
[`docs/technical-plan.md` MVP-1a / 1b / 1c](../../docs/technical-plan.md#mvp-1a显示链路不涉及网络).
