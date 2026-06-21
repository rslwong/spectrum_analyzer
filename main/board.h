// Board bring-up for the Guition JC4880P443C-I-W (ESP32-P4): ST7701 MIPI-DSI
// display + LVGL, and the ES8311 microphone. See JC4880P443C-BOARD.md.
#pragma once

#include "esp_codec_dev.h"
#include "lvgl.h"

#define MIC_SAMPLE_RATE 16000  // Nyquist 8 kHz — enough for a mic spectrum

// Bring up the panel and LVGL. Call once before creating any LVGL objects.
// LVGL runs in its own task; guard widget access with bsp_lvgl_lock/unlock.
lv_display_t *bsp_display_start(void);

bool bsp_lvgl_lock(int timeout_ms);  // <0 = wait forever
void bsp_lvgl_unlock(void);

// Bring up the ES8311 in record mode. Returns an open codec handle; read 16-bit
// mono samples with esp_codec_dev_read().
esp_codec_dev_handle_t bsp_mic_start(void);
