# Mic Spectrum Analyzer — Guition JC4880P443C-I-W (ESP32-P4)

Real-time microphone test and spectrum analyzer for the board's on-board ES8311
mic, drawn on the 480×800 ST7701 panel with LVGL.

- **Log-spaced bar graph** (24 bands, ~80 Hz → 8 kHz) with **peak-hold markers**.
- **Level meter** and a **`LISTENING` / `quiet`** light driven by input RMS.
- **Dominant frequency** read out from the FFT peak.

## Build & flash

ESP-IDF v6.x must be installed (see [ESP-IDF.md](ESP-IDF.md)). Then just:

```bash
make           # build (sets target esp32p4 on first run)
make run       # build + flash + serial monitor
```

The `Makefile` sources ESP-IDF's `export.sh` itself, so no setup script needed.
Override paths if yours differ:

```bash
make IDF_PATH=~/esp/esp-idf PORT=/dev/cu.usbmodem1101 run
```

Targets: `build` · `flash` · `monitor` · `run` · `clean` · `menuconfig`.

## How it works

`main/board.c` brings up the panel (ST7701 MIPI-DSI + LVGL, native portrait) and
the ES8311 mic (I²S RX + I²C control via `esp_codec_dev`), per
[JC4880P443C-BOARD.md](JC4880P443C-BOARD.md).

`main/main.c` reads 1024-sample frames at 16 kHz, removes DC, applies a Hann
window, runs an `esp-dsp` FFT, then maps the magnitudes onto log-spaced bars,
holds peaks, computes the level meter from RMS and shows the dominant bin.

## Tuning

The mic and room are physical — expect to adjust. Knobs at the top of
[main/main.c](main/main.c):

| Knob | Meaning |
|---|---|
| `DB_FLOOR` / `DB_CEIL` | dB window mapped to bar/level height. Raise the floor if idle noise lights bars. |
| `LISTEN_DB` | RMS threshold for the `LISTENING` light. |
| `PEAK_DECAY` | How fast peak markers fall. |

Mic input gain is `esp_codec_dev_set_in_gain(..., 30.0)` in `main/board.c`.

## Not included (didn't need it)

Touch, Wi-Fi, and SD are unused and left out. The `JC4880P443C-BOARD.md`
reference covers them if a future project wants them.
