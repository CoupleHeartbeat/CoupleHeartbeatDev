<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# CoupleHeartbeat · Firmware

![platform](https://img.shields.io/badge/ESP32--C3-ESP--IDF%205.5.3-blue)
![flash](https://img.shields.io/badge/flash-8%20MB-informational)
![license](https://img.shields.io/badge/license-MIT-green)

Firmware for a pair of matching companion badges. The two devices sense each
other over Bluetooth Low Energy; spoken questions are recorded on the badge and
relayed by the companion phone, which runs speech recognition and the chat model
and sends the reply text back over BLE. It runs on the **FoloToy AI Passport**
board (ESP32-C3, 8 MB flash, no PSRAM).

> **The device never joins a network.** Wi-Fi, TLS and the on-device model
> credentials are gone. The firmware only keeps BLE, LVGL, the audio capture
> path and the small power state machine, which leaves most of the ~170 KB RAM
> free for the UI and the BLE stacks.

## Features

| Area | Behaviour |
| --- | --- |
| **Badge** | Time, nickname, a localized date with weekday, battery, an app-link icon and a proximity glow around the screen edges. |
| **Proximity** | Each device broadcasts a BLE heartbeat. When the partner is in range the badge stays on screen and the edge glow brightens and quickens with closeness; the heartbeat tone follows the same curve. One line shows distance and RSSI, and hides when the partner leaves. |
| **Voice dialog** | Long-press OK to talk. PCM is streamed to the phone over BLE while recording, then the phone returns the model reply as text, which is shown on screen. No TTS and no audio playback by design. |
| **Settings** | Short-press OK opens the menu (about, sleep now, factory reset). UP/DOWN adjusts volume; the current level is shown on screen. |
| **Power** | After 60 s idle with no partner the backlight drops to 10 % and only name, time and date stay visible. After 15 minutes idle a 10 s warning precedes deep sleep; "sleep now" triggers it manually. An active phone session suppresses deep sleep. |
| **Configuration** | Pairing data, the badge name and the clock are written by the companion app over BLE. Nothing about Wi-Fi or model providers is stored on the device. |
| **Fonts** | A generated GB2312 font set (6 763 hanzi plus full-width punctuation, 7 118 glyphs at 16 px and 20 px) lives in flash and is read on demand, so dynamic Chinese text renders without spending RAM. |

## BLE interface

One primary service (`0000ffe0-0000-1000-8000-00805f9b34fb`) exposes:

| UUID | Access | Purpose |
| --- | --- | --- |
| 0xFFE1 | write | Pairing configuration (role, pair id and key, partner address, screen code) |
| 0xFFE2 | read / write | Badge display name |
| 0xFFE3 | read / notify | Device status (battery, mode, volume, partner signal) |
| 0xFFE8 | write | Time sync: `{"unix":seconds,"tz_offset":seconds}` |
| 0xFFE9 | notify | Voice uplink: 6-byte header + PCM, device → phone |
| 0xFFEA | write | Reply downlink: `[status][UTF-8 text]`, phone → device |

### Voice uplink (0xFFE9)

Audio is 8 kHz / 16 bit / mono. Up to 4 seconds (64 000 bytes) are streamed
while recording; no 64 KB buffer exists on the device.

| Offset | Field |
| --- | --- |
| 0 | type: `1` BEGIN, `2` DATA, `3` END, `4` CANCEL |
| 1 | reserved, 0 |
| 2..3 | offset, little endian |
| 4..5 | total PCM bytes, little endian |
| 6.. | PCM payload (empty on BEGIN / END) |

The `END` packet's offset is the exact number of valid bytes, so silence is
never padded. The phone reassembles the stream, converts it to WAV, calls the
ASR and chat models, and writes `[0][text]` (success) or `[1][reason]` (failure)
back on 0xFFEA within the 30 s reply window.

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
| nvs | data | 0x500000 | 24 KiB | pairing / volume / saved time |
| phy_init | data | 0x506000 | 4 KiB | RF calibration |
| user | data (FAT) | 0x507000 | ~2.97 MiB | user files, mounted at `/userdata` |

All data partitions sit **after** the application and the merged image only
covers `0x0` to the end of the application, so flashing a new build keeps NVS
settings and user files. The `user` volume is wear-levelled FAT and is formatted
on first use.

Upgrading from a Wi-Fi build keeps the stored pairing, but the badges can no
longer reach a network: re-pair them from the app if the pairing data changed.

## Project layout

```
main/              application: badge UI, AI dialog, BLE/GATT, voice uplink, power
components/bsp/    board support: display, audio, buttons, battery, I2C
assets/fonts/      generated LVGL fonts (GB2312 set)
tools/             validate/archive/verify scripts and the font generator
tests/             host tests (proximity logic, power policy, BSP contracts)
```

## Self checks

```bash
./tools/validate.sh --static     # repository checks plus host tests
./tools/validate.sh --firmware   # build plus merged image verification
python3 tools/gen_couple_fonts.py --check   # CJK coverage gate
```

The host tests cover the proximity decision logic and the power policy; those
modules deliberately do not depend on ESP-IDF or LVGL so they run on a
development machine.

## Companion app

Pairing, speech recognition and the chat model live in the Flutter app:
https://github.com/CoupleHeartbeat/CoupleHeartbeatApp

Two phones are used: each pairs its own badge, then the two phones exchange a
`pair_id` / `pair_key` / partner address through a QR code.

## License

MIT. This firmware derives from the FoloToy AI Passport project; keep the
copyright notice in [`LICENSE`](LICENSE).
