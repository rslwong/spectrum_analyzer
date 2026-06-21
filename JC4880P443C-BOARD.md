# Guition JC4880P443C-I-W — Board Reference (ESP32-P4)

A practical, reuse-anywhere hardware reference for the **Guition JC4880P443C-I-W**:
a 4.3", 480×800 IPS MIPI-DSI panel on an **ESP32-P4** with an **ESP32-C6**
Wi-Fi/BT co-processor and an **ES8311** audio codec.

Everything here was verified on real hardware against **ESP-IDF v6.x** (built with a
v6.1-dev / 6.2 toolchain). Values marked _(vendor pkg)_ come from the official
`JC4880P443C_I_W.zip` demo package; _(community)_ from public configs.

---

## 1. At a glance

| | |
|---|---|
| Main MCU | ESP32-P4 (RISC-V dual-core, 360–400 MHz), **silicon rev v1.3** |
| Wi-Fi / BT | ESP32-C6 co-processor over **SDIO** (ESP-Hosted), pre-flashed slave fw |
| Display | 4.3" 480×800 IPS, **ST7701S** controller, **MIPI-DSI** (2 data lanes) |
| Touch | **GT911** capacitive, I²C |
| Audio | **ES8311** codec, I²S + I²C, with onboard power amplifier |
| Storage | 16 MB flash (Boya), **32 MB PSRAM** (AP/Hex, 200 MHz), microSD (SDMMC) |
| Console | UART on GPIO37 (RX) / GPIO38 (TX), 115200 |

> ⚠️ **The two things that will waste your day:** (1) the P4 is **rev v1.x**
> silicon — IDF 6.x defaults to the rev-v3 range and **won't boot**; (2) the panel
> is **ST7701, not ILI9881C**. See §3 and §9.

---

## 2. Pin map (complete)

### Display — MIPI-DSI (no GPIO data pins; DSI is a dedicated PHY)
| Signal | GPIO | Notes |
|---|---|---|
| LCD reset | **5** | active-low _(vendor pkg; `pins_config.h` says -1 but the driver uses GPIO5)_ |
| Backlight | **23** | active-high (plain on/off; PWM works too) |
| DSI D-PHY power | internal LDO **ch 3 @ 2500 mV** | `esp_ldo_acquire_channel()` |

### Touch — GT911 (I²C)
| Signal | GPIO |
|---|---|
| I²C SDA | **7** |
| I²C SCL | **8** |
| Reset | **-1** (not wired / power-on) |
| INT | **-1** |
| I²C freq | 400 kHz |

### Audio — ES8311 (I²S data + shared I²C control)
| Signal | GPIO | Notes |
|---|---|---|
| I²S MCLK | **13** | |
| I²S BCLK | **12** | |
| I²S WS / LRCK | **10** | |
| I²S DOUT (P4→codec) | **9** | playback |
| I²S DIN (codec→P4) | **48** | mic, optional |
| PA enable | **11** | active-high power amplifier |
| I²C control | **7 / 8** | **shared** with touch; addr `0x18` (7-bit) |

### Wi-Fi/BT — ESP32-C6 over SDIO
| Signal | GPIO |
|---|---|
| CLK | **18** |
| CMD | **19** |
| D0 | **14** |
| D1 | **15** |
| D2 | **16** |
| D3 | **17** |
| Slave reset | **54** |

Bus: SDIO **slot 1, 4-bit, 40 MHz**. These match the esp_hosted ESP32-C6
defaults, so no pin overrides are needed.

### microSD — SDMMC _(vendor pkg)_
| Signal | GPIO |
|---|---|
| CLK | **43** |
| CMD | **44** |
| D0 | **39** |
| D1 | **40** |
| D2 | **41** |
| D3 | **42** |

---

## 3. ESP32-P4 silicon revision (critical)

This board ships **rev v1.3** silicon. ESP-IDF 6.x defaults its minimum target to
the **v3.x** range (the two ranges are mutually exclusive and hardware-different),
so a default build flashes but **panics/reboots immediately**. esptool reports:

```
'bootloader/bootloader.bin' requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)
```

Fix — select the <3.0 range and set the minimum to v1.0:

```ini
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

---

## 4. Display (ST7701S over MIPI-DSI)

**Controller is ST7701S, not ILI9881C.** Wrong driver = the panel ignores init
and the driver's DCS read of the panel ID hangs forever (task-WDT reset).

Driver: `espressif/esp_lcd_st7701` with `flags.use_mipi_interface = 1`.
Feed it the board's **vendor init sequence** via `init_cmds` (the
`JC4880P443C_I_W.zip` package, `src/lcd/esp_lcd_st7701_mipi.c`).

**DSI / DPI parameters _(vendor pkg)_:**

| Parameter | Value |
|---|---|
| Data lanes | 2 |
| Lane bit rate | **500 Mbps** |
| DPI pixel clock | **34 MHz** |
| Resolution | 480 × 800 |
| HSYNC / HBP / HFP | 12 / 42 / 42 |
| VSYNC / VBP / VFP | 2 / 8 / 166 |
| Color | RGB565, RGB order |
| Reset | GPIO5, active-low |

**Init order:** power LDO ch3 → `esp_lcd_new_dsi_bus` → `esp_lcd_new_panel_io_dbi`
→ `esp_lcd_dpi_panel_config_t` (with the timings above) → `esp_lcd_new_panel_st7701`
→ `reset` → `init` → (`esp_lcd_dpi_panel_enable_dma2d`) → `disp_on_off(true)`.

**Rotation:** ST7701/MIPI-DPI **cannot rotate in hardware**. To run a landscape UI
on this portrait panel, rotate with the P4's **PPA** (Pixel Processing Accelerator):

```ini
CONFIG_LVGL_PORT_ENABLE_PPA=y
```
and set `esp_lvgl_port`'s `lvgl_port_display_cfg_t.flags.sw_rotate = 1`, then
`lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`. Without `sw_rotate` the
port falls back to `esp_lcd_panel_swap_xy()` — unsupported on DPI — and you get a
garbled, flickering screen.

**Tear-free alternative:** native-portrait UI (no rotation) + `num_fbs = 2` in the
DPI config + `lvgl_port_display_dsi_cfg_t.flags.avoid_tearing = 1`. `avoid_tearing`
and `sw_rotate` are mutually exclusive (one binds LVGL to the panel framebuffers,
the other needs its own buffers to rotate from).

---

## 5. Touch (GT911)

Standard `espressif/esp_lcd_touch_gt911` on the shared I²C bus (SDA7/SCL8). Reset
and INT are not wired (-1), so the GT911 comes up at its default address on
power-on. If touch lands mirrored/swapped from where you press, flip the
`swap_xy` / `mirror_x` / `mirror_y` flags in the touch config (depends on your UI
rotation).

---

## 6. Audio (ES8311)

The ES8311 is an I²S codec — it needs I²C register setup **and** an I²S stream;
there is no DAC/PWM shortcut. Use `espressif/esp_codec_dev` (handles the ES8311
register init, volume and the PA pin):

1. Create an **I²S STD TX** channel (master) on MCLK13/BCLK12/WS10/DOUT9. Do **not**
   enable it — `esp_codec_dev` enables/disables the channel on open/close.
2. `audio_codec_new_i2s_data` (tx handle) + `audio_codec_new_i2c_ctrl`
   (`bus_handle` = the shared I²C bus, `addr = ES8311_CODEC_DEFAULT_ADDR`).
3. `es8311_codec_new` with `codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC`,
   `pa_pin = 11`, `pa_reverted = false`, `master_mode = false`, `use_mclk = true`.
4. `esp_codec_dev_new(ESP_CODEC_DEV_TYPE_OUT)` → `open(sample_info)` →
   `set_out_vol()` → `write()`.

Opening the device powers the amp (PA pin); closing it powers it down — so open
only while you actually need sound.

---

## 7. Wi-Fi / Bluetooth (ESP32-C6 via ESP-Hosted)

The P4 has no radio. Wi-Fi/BT run on the onboard ESP32-C6 over SDIO via
`esp_wifi_remote` + `esp_hosted` (the C6 ships with the ESP-Hosted slave fw). The
default esp_hosted ESP32-C6 SDIO pin map already matches this board.

**Gotcha:** esp_hosted's SDIO transport buffer pool fails to allocate from
internal DMA RAM during its early init constructor and **asserts**
(`sdio_mempool_create ... buf_mp_g`) before `app_main`. The P4's GDMA can reach
PSRAM through cache, so move those buffers to PSRAM:

```ini
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y
```

Use `esp_wifi.h` (from the `esp_wifi` component) for the API; the calls are
forwarded to the C6. Expect a harmless `Version mismatch: Host > Co-proc` warning
unless you reflash the C6.

---

## 8. Flash / PSRAM / partitions

- **Flash:** 16 MB, Boya chip (a `using generic driver` warning is benign; enable
  `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP` to silence).
- **PSRAM:** 32 MB, AP/Hex, 200 MHz, with `.text`/`.rodata` execute-in-place.
- **Partition table offset:** PSRAM-XIP grows the 2nd-stage bootloader past the
  0x6000 that fits before the default 0x8000 table offset, so move the table:

```ini
CONFIG_PARTITION_TABLE_OFFSET=0x10000
```
  and start partitions at ≥ 0x11000 (app partitions must be 0x10000-aligned).

---

## 9. Minimal `sdkconfig.defaults`

The board-specific settings that make a fresh project work here:

```ini
# --- ESP32-P4 v1.x silicon (this board) -------------------------------------
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y

# --- PSRAM (32 MB, XIP) ------------------------------------------------------
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y

# --- Bootloader / partition table (XIP needs more room) ----------------------
CONFIG_PARTITION_TABLE_OFFSET=0x10000

# --- Wi-Fi co-processor (ESP32-C6 over SDIO via ESP-Hosted) ------------------
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y

# --- Display rotation via the P4 PPA (only if you run a rotated UI) ----------
CONFIG_LVGL_PORT_ENABLE_PPA=y
```

(Flash size 16 MB and a custom `partitions.csv` are also assumed.)

---

## 10. Managed components (ESP Component Registry)

`main/idf_component.yml` dependencies known to work on this board:

| Component | Purpose | Verified version |
|---|---|---|
| `espressif/esp_lcd_st7701` | ST7701S MIPI-DSI panel | 2.0.2 |
| `espressif/esp_lcd_touch_gt911` | GT911 touch | 1.2.x |
| `espressif/esp_lvgl_port` | LVGL ↔ esp_lcd glue, PPA rotation | 2.8.x |
| `lvgl/lvgl` | UI toolkit | 9.x |
| `espressif/esp_codec_dev` | ES8311 codec | latest |
| `espressif/esp_wifi_remote` | Wi-Fi API forwarding to the C6 | 1.6.x |
| `espressif/esp_hosted` | host↔C6 SDIO transport | 2.12.x |
| `espressif/cjson` | (if you parse JSON; left core IDF in 6.0) | 1.7.x |

---

## 11. Gotchas checklist

- [ ] **Won't boot / instant reboot** → set the rev-<3.0 options (§3).
- [ ] **Display hangs at boot (WDT)** → wrong panel driver; it's **ST7701**, and its
      ID read only works under the ST7701 driver (§4).
- [ ] **Garbled / flickering screen** → rotation via `swap_xy` is unsupported on DPI;
      enable PPA + `sw_rotate`, or go native-portrait + `avoid_tearing` (§4).
- [ ] **`sdio_mempool_create` assert before app_main** → `MEMPOOL_PREFER_SPIRAM` (§7).
- [ ] **Bootloader too large for 0x8000** → move partition table to 0x10000 (§8).
- [ ] **No sound** → ES8311 needs I²C init (use `esp_codec_dev`) and the PA pin
      (GPIO11) high; it shares the touch I²C bus (§6).
- [ ] **Touch offset/mirrored** → flip swap/mirror flags to match your rotation (§5).

---

## 12. Reference: vendor package layout

`JC4880P443C_I_W.zip` (from Guition) contains the authoritative source:
- `1-Demo/arduino_examples/lvgl_demo_v8/src/lcd/esp_lcd_st7701_mipi.c` — ST7701
  init sequence + the real 480×800 DPI timings (in `esp_lcd_st7701.h`).
- `1-Demo/arduino_examples/lvgl_demo_v8/pins_config.h` — touch/LCD pin map.
- `1-Demo/arduino_examples/mp3_player/mp3_player.ino` — I²S/ES8311/SD pin map.
- `1-Demo/idf_examples/ESP-IDF/...` — full IDF examples (Brookesia, xiaozhi) with
  codec configs.
