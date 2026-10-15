<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# CoupleHeartbeat Firmware

Firmware for a pair of matching companion gadgets that sense each other over
Bluetooth Low Energy and answer voice questions through a cloud model. It runs
on the FoloToy AI Passport board (ESP32-C3, 8 MB flash, no PSRAM) with ESP-IDF
5.5.3.

## What it does

- **Badge** - clock, name, battery level, a Wi-Fi status icon beside the battery
  and a proximity glow around the screen edges.
- **Proximity glow** - every device broadcasts a BLE heartbeat carrying its own
  identity. When the paired device comes in range nothing switches page: the
  badge screen stays up and a light effect breathes along all four edges of the
  screen. The closer the two devices are, the brighter and faster it gets, and
  the heartbeat tone quickens with it; holding them right next to each other
  raises it to its strongest setting. One line under the name shows the current
  distance and signal strength, and it hides again once the partner leaves.
- **AI dialog** - a long press on OK opens the voice page. Speaking records
  audio, transcribes it, asks the model and speaks the answer back through the
  speaker.
- **Provisioning** - Wi-Fi credentials and the model API key are pushed from the
  companion app over BLE and stored in the device NVS only. The device can also
  scan nearby networks on request, so the app lists the networks the device can
  actually see (2.4 GHz only).

## BLE interface

One primary service (`0000ffe0-0000-1000-8000-00805f9b34fb`) exposes these
characteristics:

| UUID | Access | Purpose |
| --- | --- | --- |
| 0xFFE1 | write | pairing configuration (role, pair id and key, partner address, screen code) |
| 0xFFE2 | read / write | badge display name |
| 0xFFE3 | read / notify | device status (battery, mode, volume, partner signal) |
| 0xFFE4 | write | Wi-Fi credentials, or a clear request |
| 0xFFE5 | read / notify | Wi-Fi provisioning state (state, SSID, IP, signal) |
| 0xFFE6 | read / write | model API key; reads only report whether one is configured |
| 0xFFE7 | read / write | nearby Wi-Fi scan: write a scan request, read the candidate list |
| 0xFFE8 | write | time sync: write `{"unix":seconds,"tz_offset":seconds}` to set system time |

A read never returns the API key and the key is never written to the log.

## Build

```bash
. "$IDF_PATH/export.sh"          # ESP-IDF 5.5.3
idf.py build
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

Write the merged image at offset `0x0` with a data-capable USB cable.

## Self checks

```bash
./tools/validate.sh --static     # repository checks plus host tests
./tools/validate.sh --firmware   # build plus merged image verification
```

The host tests cover the proximity decision logic, the Wi-Fi credential parser
and the model response parser, all of which are kept free of ESP-IDF and LVGL so
they can run on a development machine.

## Companion app

The phone app that pairs and configures the devices lives in
https://github.com/CoupleHeartbeat/CoupleHeartbeatApp

## License

MIT. This firmware is derived from the FoloToy AI Passport project; see
`LICENSE` for the copyright notice that must be retained.
