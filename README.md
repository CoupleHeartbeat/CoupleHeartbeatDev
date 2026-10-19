<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# CoupleHeartbeat · Firmware

![platform](https://img.shields.io/badge/ESP32--C3-ESP--IDF%205.5.3-blue)
![flash](https://img.shields.io/badge/flash-8%20MB-informational)
![license](https://img.shields.io/badge/license-MIT-green)

Firmware for a pair of matching companion badges. The two devices sense each
other over Bluetooth Low Energy and answer spoken questions through a cloud
model. It runs on the **FoloToy AI Passport** board (ESP32-C3, 8 MB flash, no
PSRAM).

> **The binding constraint is RAM.** About 170 KB is available in total and
> roughly 30 KB stays free once Wi-Fi, BLE, LVGL and the audio path are running.
> TLS handshakes, Wi-Fi buffers and even the drawing buffer are sized around
> that budget.

## Features

| Area | Behaviour |
| --- | --- |
| **Badge** | Time, nickname, a localized date with weekday, battery, Wi-Fi icon and a proximity glow around the screen edges. |
| **Proximity** | Each device broadcasts a BLE heartbeat. When the partner is in range the badge stays on screen and the edge glow brightens and quickens with closeness; the heartbeat tone follows the same curve. One line shows distance and RSSI, and hides when the partner leaves. |
| **Voice dialog** | Long-press OK to talk. Audio is streamed to the ASR model on release, the transcript goes to the chat model, and the reply is shown on screen. Playback is text-only by design; TTS has been removed. |
| **Settings** | Short-press OK opens the menu (about, sleep now, factory reset). UP/DOWN adjusts volume; the current level is shown on screen. |
| **Power** | After 60 s idle with no partner the backlight drops to 10 % and only name, time and date stay visible. After 15 minutes idle a 10 s warning precedes deep sleep; "sleep now" triggers it manually. |
| **Provisioning** | Wi-Fi credentials and the model API key are pushed from the companion app over BLE and stored in NVS only. The device can scan nearby 2.4 GHz networks on request, and syncs time (with timezone) from the phone. |
| **Fonts** | A generated GB2312 font set (6 763 hanzi plus full-width punctuation, 7 118 glyphs at 16 px and 20 px) lives in flash and is read on demand, so dynamic Chinese text renders without spending RAM. |

## BLE interface

One primary service (`0000ffe0-0000-1000-8000-00805f9b34fb`) exposes:

| UUID | Access | Purpose |
| --- | --- | --- |
| 0xFFE1 | write | Pairing configuration (role, pair id and key, partner address, screen code) |
| 0xFFE2 | read / write | Badge display name |
| 0xFFE3 | read / notify | Device status (battery, mode, volume, partner signal) |
| 0xFFE4 | write | Wi-Fi credentials, or a clear request |
| 0xFFE5 | read / notify | Wi-Fi provisioning state (state, SSID, IP, signal) |
| 0xFFE6 | read / write | Model API key; reads only report whether one is configured |
| 0xFFE7 | read / write | Nearby Wi-Fi scan: write a request, read the candidate list |
| 0xFFE8 | write | Time sync: `{"unix":seconds,"tz_offset":seconds}` |

A read never returns the API key, and the key is never written to the log.

## Build

```bash
. "$IDF_PATH/export.sh"        # ESP-IDF 5.5.3
./tools/validate.sh --firmware # build, merge and verify the image
```

The verified merged image is written to
`build/firmware/<sha256>/FoloToy-AI-Passport-full.bin` and flashed at offset
`0x0` with a data-capable USB cable:

```bash
python3 -m esptool --chip esp32c3 -p <PORT> -b 460800 \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/firmware/<sha256>/FoloToy-AI-Passport-full.bin
```

### Flash layout and data preservation

| Name | Type | Offset | Size | Purpose |
| --- | --- | --- | --- | --- |
| factory | app | 0x10000 | ~4.94 MiB | application |
| nvs | data | 0x500000 | 24 KiB | pairing / Wi-Fi / API key / volume / saved time |
| phy_init | data | 0x506000 | 4 KiB | RF calibration |
| user | data (FAT) | 0x507000 | ~2.97 MiB | user files, mounted at `/userdata` |

All data partitions sit **after** the application and the merged image only
covers `0x0` to the end of the application, so flashing a new build keeps NVS
settings and user files. The `user` volume is wear-levelled FAT and is formatted
on first use.

Migrating from the old layout (NVS at `0x9000`) erases the old NVS once:
re-pair the devices and re-enter Wi-Fi credentials after that first flash.

## Project layout

```
main/              application: badge UI, AI dialog, BLE/GATT, power, storage
components/bsp/    board support: display, audio, buttons, battery, I2C
assets/fonts/      generated LVGL fonts (GB2312 set)
tools/             validate/archive/verify scripts and the font generator
tests/             host tests (proximity logic, parsers, BSP contracts)
```

## Self checks

```bash
./tools/validate.sh --static     # repository checks plus host tests
./tools/validate.sh --firmware   # build plus merged image verification
python3 tools/gen_couple_fonts.py --check   # CJK coverage gate
```

The host tests cover the proximity decision logic, the Wi-Fi credential parser,
the model response parser and the power policy; those modules deliberately do
not depend on ESP-IDF or LVGL so they run on a development machine.

## Companion app

Pairing and configuration live in the Flutter app:
https://github.com/CoupleHeartbeat/CoupleHeartbeatApp

## License

MIT. This firmware derives from the FoloToy AI Passport project; keep the
copyright notice in [`LICENSE`](LICENSE).
