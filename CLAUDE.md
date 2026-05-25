# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository status

MVP-0 implemented. The Bridge + browser simulator are working; firmware
is at MVP-1a step 2 skeleton (display bring-up). Touch chip model is
still unconfirmed — a known risk in `docs/technical-plan.md`.

The authoritative design documents live under `docs/`:

- `docs/technical-plan.md` — master plan: architecture, milestones, scope (Chinese).
- `docs/codex-app-server-research.md` — Codex app-server JSON-RPC events Bridge must handle.
- `docs/device-protocol.md` — full Bridge ↔ device wire protocol v1, including all current message types (welcome / status / question / reply / usage / session_info / thread_list / follow_thread / error / ping / pong / ack / device_status), pairing flow, mDNS announce, error codes, audit format.
- `docs/risk-policy.md` — low / medium / high mapping and second-confirm rules.
- `docs/firmware/partitions.csv` + `docs/firmware/partitions.md` — 4MB flash layout (locked; do not change `ota_0`/`ota_1`/`assets` offsets without OTA migration plan).

Read the relevant doc before proposing changes to architecture, protocol, or scope. When the plan and a request disagree, flag it before changing the doc.

## Workspace layout

pnpm workspace (`packages/`):

- `device-protocol/` — zod-validated TypeScript types for the wire protocol (shared by Bridge and Simulator).
- `codex-app-server-protocol/` — TS types generated from `codex app-server generate-ts` (regen via `pnpm regen:codex-bindings`). 500+ Codex types.
- `bridge/` — Node.js Bridge service. Spawns sidecar codex processes for `account/read` (one-shot) + `thread/list` and `account/rateLimits/read` (long-lived polling). Watches `~/.codex/sessions/.../rollout-*.jsonl` files in follow mode.
- `simulator/` — Browser simulator (vanilla TS + esbuild → static files served by Bridge at port 8788). Four-screen round display mock with swipe.
- `firmware/` — ESP-IDF C project for ESP32-C3 + GC9A01 round LCD. Build with `idf.py build` (requires ESP-IDF v5.3 installed). MVP-1a step 2: color-fill test pattern, no LVGL yet.

## Transport: WS (default) vs BLE (opt-in)

The firmware ↔ Bridge link can be either:

- **Wi-Fi + WebSocket** (default). Bridge listens on `:8788`; firmware connects via mDNS-discovered `ws://`. Code paths: `packages/firmware/main/bridge_client.c` ↔ `packages/bridge/src/deviceConnection.ts`.
- **BLE GATT** (opt-in). Firmware advertises `doudou-XXXXXX` (last-6 of MAC); Bridge runs a noble-based central scanner that finds + connects + speaks the same JSON envelope over chunked GATT writes/notifies. Code paths: `packages/firmware/main/ble_transport.c` ↔ `packages/bridge/src/ble/*`.

To switch to BLE end-to-end:

```bash
# 1. Firmware: re-configure with the BLE defaults
cd packages/firmware
rm sdkconfig                 # discard the WS-configured copy
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.ble" reconfigure
idf.py build flash monitor

# 2. Bridge: install the native BLE binding once, then start with the flag
pnpm --filter @doudou/bridge add @abandonware/noble
DOUDOU_BLE=1 pnpm dev
```

BLE wire format: 2-byte fragment header (`seq`, `flags`) + UTF-8 JSON. Chunk payload ≤ MTU-3-2. Single GATT service `0xDD00DDDD-...` with TX (notify, device→bridge), RX (write, bridge→device), CTL (read+notify status).

**Pairing + encryption** (Phase 3): all data characteristics gate on `BLE_GATT_CHR_F_*_ENC`, so NimBLE auto-starts pairing on first connect. We run **LE Secure Connections + Just Works + bonding-on**: no PIN/passkey UI on the device, the LTK is persisted to NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`, `MAX_BONDS=1`) so reconnects only encrypt without re-pairing. `BLE_GAP_EVENT_REPEAT_PAIRING` handler nukes the stored bond + retries when the central forgets. The legacy `pairing_token` JSON field still travels in the `hello` envelope and Bridge enforces it — that's the application-layer secret, layered on top of link-layer encryption.

**Slim BLE build** (Phase 4): when `CONFIG_DOUDOU_TRANSPORT_BLE` is selected, `main.c` skips `doudou_net_start` and calls `doudou_bridge_connect(NULL, ...)` directly. `net.h` is `#ifdef`-guarded out. `net.c` stays compiled (unused symbols get DCE'd at link), so flipping transport is a Kconfig change, no source edits.

## Common commands

```bash
pnpm install                              # workspace install
pnpm -r build                             # build all TS packages
pnpm test                                 # TS tests + firmware host tests
pnpm test:ts                              # TS only (faster iteration)
pnpm firmware:test                        # protocol_parse host tests only
pnpm --filter @doudou/bridge test         # bridge unit tests (vitest)
pnpm --filter @doudou/bridge typecheck    # typecheck without emit

# Run bridge (defaults to real Codex + follow mode):
node packages/bridge/dist/cli.js
# Then open http://localhost:8788/ for the simulator.

# Firmware on-device (requires `. ~/esp/esp-idf/export.sh` once per shell):
cd packages/firmware && idf.py build
cd packages/firmware && idf.py -p /dev/cu.usbmodem* flash monitor

# Firmware in Wokwi (browser simulation — no hardware needed for UI work):
#   install the "Wokwi for VSCode" extension, then open
#   packages/firmware/diagram.json and click ▶. See docs/wokwi-guide.md.
#   Wokwi runs the real doudou.bin on a simulated C3 + LCD + buttons.
#   Limits: no Wi-Fi/BLE/CST816D — UI iteration only, real link still needs hardware.

# Firmware host unit tests (no IDF / hardware needed; needs curl on first run):
make -C packages/firmware/test/host test

# Regenerate pet-art LVGL C arrays from PNG sources (needs Pillow):
pnpm pet-art:build
```

## Toolchains installed

- **Node.js v24** + **pnpm v10** — for Bridge / Simulator / device-protocol TS workspace
- **ESP-IDF v5.3** at `~/esp/esp-idf/`, toolchain at `~/.espressif/` (RISC-V GCC 13.2.0 for ESP32-C3) — for firmware
- **Codex CLI** — Bridge spawns it for `account/read`, `thread/list`, `account/rateLimits/read` sidecar polls

## Firmware status (MVP-1c — sprite pet + question UI)

`packages/firmware/` builds end-to-end on ESP-IDF 5.3 (`doudou.bin` ≈ 1024 KB):
- Display (GC9A01), touch (CST816D) — vendor pins verified
- LVGL v9 — 4-screen UI (INFO / USAGE / PET / HISTORY) with swipe
- **Sprite-layered pet** (`pet_ui.c`): single `pet_body_glossy` + per-state eye/mouth/cheek/accessory swap from `pet_art.h`. Breath via `transform_scale` 256→272, gear spin via `lv_image_set_rotation`, accessory float via `translate_y`, blink scheduler with `esp_random()` for 3-7 s cadence, error-shake on state entry.
- Wi-Fi STA + mDNS + WebSocket client (`espressif/esp_websocket_client`)
- Device protocol v1 parser/serializer (cJSON) — covers all current message types incl. **question/reply**. Pure logic lives in `main/protocol_parse.{c,h}` (IDF-free) so host unit tests can link against it directly — see `test/host/`. 43 host tests today.
- **Question overlay** (`pet_ui.c`): risk-colored modal on the PET screen with 1-4 tappable choice buttons. Tap → `doudou_bridge_reply(qid, cid)` → bridge fans replies + closes the inflight question across the registry. Wired through `main.c`'s `on_question` / `on_question_reply` glue.
- **Codex PermissionRequest hook bridge** (`bridge/src/approval.ts` + `scripts/permission_request_hook.py`): Codex Desktop hook fires on every tool-permission request; the Python script POSTs the payload to `/approval/request` on the Bridge, which classifies risk, dispatches a `question` to known devices, races the first reply, and echoes the resulting `allow` / `deny` JSON back on hook stdout. Missing devices or timeout → empty `{}` so Codex falls back to its native prompt. See `docs/esp32-codex-approval-bridge.md` for design notes, and the docstring at the top of `permission_request_hook.py` for the `~/.codex/hooks.json` snippet.
- Pet-art sprite assets live in `main/pet_art/` as 19 generated `lv_image_dsc_t` arrays (~146KB raw RGB565A8). Source PNGs are at `packages/simulator/public/pet-art/`; `scripts/build_pet_art.py` resamples + packs them. Only the glossy body ships to firmware (flat is web-simulator only). `pet_art.h` declares all extern symbols.
- Slide gestures swap screens; tapping a thread row sends `follow_thread` to Bridge

User configures Wi-Fi via `idf.py menuconfig → Doudou → Wi-Fi SSID/Password`.
With Wi-Fi blank the firmware still runs, just shows an offline pet.

Environment overrides (Bridge CLI):

- `DOUDOU_PORT=8788` — HTTP/WS port
- `DOUDOU_CODEX=real|stub` — real Codex sidecar or scripted demo events
- `DOUDOU_SOURCE=follow|own|auto` — follow mode tails Codex Desktop rollouts (read-only); own mode spawns its own thread
- `DOUDOU_APPROVAL=on-request|untrusted|...` — only meaningful in `own` mode
- `DOUDOU_LOG_LEVEL=info|debug|warn|error`
- `DOUDOU_AUDIT_DIR=./audit`

## Operational architecture (follow mode, default)

```
Codex Desktop ──writes──> ~/.codex/sessions/.../rollout-*.jsonl
                                      │
                                      │ fs.watch tail
                                      ▼
              RolloutWatcherFeed ────► Bridge ────► WS /device ────► Simulator (or real doudou)
                                      ▲
                                      │
              ThreadListPoller ───────┘
                  (sidecar codex app-server)
                  ├─ thread/list (10s) → thread_list + session_info.thread_title
                  └─ account/rateLimits/read (30s) → usage.limits

              AccountProbe (one-shot at startup)
                  └─ account/read → session_info.account_email + plan_type
```

Bridge never modifies anything in `~/.codex/` — purely read-only via filesystem tail and JSON-RPC. The sidecar codex is independent of the user's Codex Desktop; both run in parallel.

## Product in one line

A 1.28-inch round ESP32-C3 touchscreen ("Doudou / 豆豆") that displays Codex status and lets the user approve/decline Codex prompts by tapping the screen. It is a **companion device**, not a Codex client.

## Three-tier architecture

```
Codex app-server  <--JSON-RPC-->  Doudou Bridge (on user's computer)  <--WebSocket over Wi-Fi-->  ESP32-C3 device
```

Responsibilities are deliberately asymmetric — this split is load-bearing and the reason the project is feasible on a C3:

- **Codex**: real task execution, context, tool calls.
- **Doudou Bridge** (planned: Node.js or Rust, local-only HTTP/WS server, default config UI at `127.0.0.1:8787`, device socket at `ws://<bridge>:8788/device`): connects to `codex app-server`, normalizes events into the device protocol, summarizes/truncates text to fit the small screen, classifies risk, enforces request expiry and reply idempotency, holds all credentials, writes audit logs.
- **ESP32-C3 firmware** (planned: ESP-IDF + FreeRTOS + LVGL + GC9A01 driver): renders status, reads touch, sends short replies. Knows nothing about Codex semantics.

Credentials (OpenAI API key, Codex auth) and long-form content (diffs, source, terminal logs) **must never** reach the device. If a feature request implies sending those to the C3, that is a design bug — push it back into the Bridge.

## Hard constraints any implementation must respect

These come from the hardware and are non-negotiable for MVP:

- **Memory**: ESP32-C3 has 400KB SRAM, 4MB flash. LVGL must use a partial buffer (`240 x 20 x 2` or `240 x 40 x 2` to start), not full-screen double buffering. No large frame animations, font count must stay small.
- **TLS deferred**: MVP uses plaintext `ws://` on LAN. TLS is V1 only — adding it earlier risks heap exhaustion when LVGL + Wi-Fi + JSON are all live. Security relies on LAN scope + pairing token + request expiry + Bridge not binding to public interfaces.
- **Round screen**: 240x240 with corners invisible. UI must use a circular safe area (≥18–24px inset). Don't place buttons or critical text in corners. Touch targets ≥44px.
- **Message size budget** (device protocol payload, enforced by Bridge):
  - whole message < 1KB
  - `title` ≤ ~10 Chinese chars (20–30 bytes)
  - `body` ≤ ~48 Chinese chars (150 bytes)
  - `choices` ≤ 4, each `label` ≤ 8 Chinese chars
- **BOOT/RESET buttons are for flashing**, not for user interaction. The only user input surface is touch.

## Device protocol

Four message types defined in `docs/technical-plan.md` (sections "状态消息 / 问题消息 / 回复消息 / 错误消息"):

- Bridge → device: `status`, `question`, `error`
- device → Bridge: `reply`

Invariants when changing this protocol:

- Every `question` carries an `id` and an `expires_at`. Replies must echo `id`; Bridge dedupes by it.
- High-risk actions (run command, modify file, network access) must surface the action type and go through a second-confirmation page on the device.
- The device never gets the raw Codex event — Bridge always normalizes first.

## Planned firmware bring-up order

Don't skip ahead. Each step gates the next, per `docs/technical-plan.md`:

1. Serial log + power + flash flow
2. GC9A01 fill/color/rotation
3. LVGL flush on top of GC9A01
4. Touch driver + coordinate read
5. Backlight + sleep
6. Wi-Fi STA with reconnect
7. WebSocket client + heartbeat
8. Device protocol: `status` then `question`
9. Touch reply + confirmation page + request expiry
10. Heap / stack / soak test

Touch chip model and pinout are **not yet confirmed** — they need to come from the vendor's Arduino example or schematic before firmware work past step 1 is meaningful. This is called out as a risk in the plan; don't assume pins.

## What is explicitly out of scope for MVP

From the plan's "明确不做" section — push back if asked to implement any of these on the device:

- Browsing source / diff / full terminal output on the screen
- Long-text input on the device
- Picking workspace / model / Codex config on the device
- Direct device-to-Codex or device-to-OpenAI connection (must go through Bridge)
- Complex business logic for Codex events running on the C3

## Documentation conventions

- `docs/technical-plan.md` is in Chinese. Keep new design docs in Chinese to match, unless the user asks otherwise. Code identifiers and protocol field names are English (see JSON examples in the plan).
- When the plan and a request disagree on scope or protocol shape, flag it to the user before changing the plan — the document is treated as a committed design, not a draft.
