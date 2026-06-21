# ESP-IDF setup for the Guition JC4880P443C-I-W (ESP32-P4)

Reusable across projects for this board. The board's ESP32-P4 is **rev v1.3**
silicon and the panel/PSRAM need specific options, so the ESP-IDF version and a
few `sdkconfig` settings matter — pin them and you won't fight version drift.

## Version

- **ESP-IDF v6.x** (verified on `v6.1-dev`, the same line the vendor used).
  Installed here at `~/esp/esp-idf`.
- Do **not** use IDF ≤ 5.4 toolchains that lack ESP32-P4 rev-v1 support; a
  default 6.x build also won't boot until the rev options below are set.

Check what you have:

```bash
cd ~/esp/esp-idf && git describe --tags     # -> v6.1-dev-...
```

Install / pin a known-good version (only if you don't have it):

```bash
git -C ~/esp/esp-idf fetch --tags
# either track the 6.x line:
git -C ~/esp/esp-idf checkout release/v6.1
# then (re)install the toolchain for the P4:
~/esp/esp-idf/install.sh esp32p4
```

## The settings that make this board work

These live in each project's `sdkconfig.defaults` (see this repo's copy). They
are the difference between "boots" and "instant reboot / garbled / hang":

| Setting | Why |
|---|---|
| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` | P4 is rev v1.3; IDF 6.x defaults to the v3.x range and won't boot. |
| `CONFIG_SPIRAM=y`, `MODE_HEX`, `SPEED_200M`, `XIP_FROM_PSRAM` | 32 MB Hex PSRAM @ 200 MHz, code/data XIP. |
| `CONFIG_PARTITION_TABLE_OFFSET=0x10000` | XIP bootloader overflows the default 0x8000 table offset. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` | 16 MB flash. |

Display is **ST7701S over MIPI-DSI** (not ILI9881C) and audio is the **ES8311**
codec. Full pin map, timings and gotchas: `JC4880P443C-BOARD.md`.

## LVGL on this panel with IDF 6.x — the `avoid_tearing` trap

If you bring up the display with `esp_lvgl_port` (2.8.x) and turn on tear-free
mode, the app boots, prints `display up`, then **task-WDT resets** a few seconds
later with `taskLVGL` stuck in `wait_for_flushing` (`lv_refr.c`). You'll also see
this warning at build time:

```
warning: 'on_refresh_done' is deprecated: use on_frame_buf_complete instead
```

Cause: `lvgl_port_add_disp_dsi()` with `flags.avoid_tearing = true` registers the
DPI **`on_refresh_done`** callback, which IDF 6.x deprecated and **never fires**,
so LVGL waits for a flush-done that never comes.

Fix — use the normal (non-tear-free) path, which registers `on_color_trans_done`
and works on 6.x:

```c
lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags.avoid_tearing = false };
```

and give LVGL its own partial buffer instead of binding to the panel
framebuffers (which is what tear-free mode needs):

```c
lvgl_port_display_cfg_t disp_cfg = {
    .buffer_size  = LCD_H_RES * 100,   // partial buffer
    .double_buffer = true,
    .flags.buff_spiram = true,         // 480x800 is too big for internal RAM
    /* ... io_handle, panel_handle, hres/vres, color_format ... */
};
```

With `avoid_tearing` off you can set the DPI panel's `num_fbs = 1` (saves a
768 KB framebuffer). Re-enable tear-free only once `esp_lvgl_port` registers
`on_frame_buf_complete` for DSI.

### Benign noise you can ignore on this board

These print during a healthy bring-up — not errors:

- `st7701_mipi: LCD ID: FF FF FF` — panel ID read-back; the panel still inits.
- `lcd_panel: esp_lcd_panel_swap_xy(...): swap_xy is not supported` — the port
  pokes swap_xy even at rotation 0; DPI ignores it. (Don't rotate via swap_xy —
  use the PPA; see `JC4880P443C-BOARD.md` §4.)
- `i2s_common: i2s_channel_disable(...): the channel has not been enabled yet` —
  `esp_codec_dev` closing before its first open.
- `spi_flash: Detected boya flash chip but using generic driver` — silence with
  `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y` if it bothers you.

## Building

`make` sources `export.sh` for you (see `Makefile`). Manually it's:

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build flash monitor
```
