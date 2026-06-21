// Board bring-up: ST7701S MIPI-DSI panel + LVGL, and ES8311 mic capture.
// Pin map, timings and gotchas all come from JC4880P443C-BOARD.md.
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lvgl_port.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "board";

// ---- Display pins / timings (JC4880P443C-BOARD.md §2, §4) ------------------
#define LCD_H_RES        480
#define LCD_V_RES        800
#define LCD_RST_GPIO     5     // active-low, driver-controlled
#define LCD_BL_GPIO      23    // backlight, active-high
#define LCD_BIT_PER_PIX  16    // RGB565

// ---- Audio pins (JC4880P443C-BOARD.md §2) ---------------------------------
#define I2C_SDA_GPIO     7     // shared touch/codec control bus
#define I2C_SCL_GPIO     8
#define I2S_MCLK_GPIO    13
#define I2S_BCLK_GPIO    12
#define I2S_WS_GPIO      10
#define I2S_DIN_GPIO     48    // codec -> P4 (mic)

// ST7701S vendor init sequence for this exact panel — copied verbatim from the
// vendor package (esp_lcd_st7701_mipi.c). Do not "tidy"; these are panel-tuned.
static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t []){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},
    {0xB0, (uint8_t []){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t []){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x58}, 1, 0},
    {0xB2, (uint8_t []){0x87}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4E}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xB9, (uint8_t []){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t []){0x03}, 1, 0},
    {0xBC, (uint8_t []){0x00}, 1, 0},
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t []){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t []){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t []){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t []){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},
};

void bsp_display_start(void)
{
    // 1. Power the MIPI D-PHY via internal LDO ch3 @ 2.5 V (§4)
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    // 2. DSI bus
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    // 3. DBI control IO
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = ST7701_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io));

    // 4. DPI video config — exact 480x800 timings from the board reference (§4).
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 34,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_pulse_width = 12,
            .hsync_back_porch = 42,
            .hsync_front_porch = 42,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 166,
        },
    };

    // 5. ST7701 panel with the vendor init sequence over MIPI
    st7701_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg },
        .flags.use_mipi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIX,
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // 6. Backlight on
    gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << LCD_BL_GPIO };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_BL_GPIO, 1);

    // 7. LVGL port + display (native portrait, no rotation -> avoid_tearing OK)
    ESP_ERROR_CHECK(lvgl_port_init(&(lvgl_port_cfg_t)ESP_LVGL_PORT_INIT_CONFIG()));
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 100,  // partial buffer in PSRAM; double-buffered
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_spiram = true },
    };
    // ponytail: avoid_tearing off on purpose — its DPI path uses the deprecated
    // on_refresh_done callback, which never fires on IDF 6.1 and hangs LVGL.
    lvgl_port_display_dsi_cfg_t dsi_disp_cfg = { .flags.avoid_tearing = false };
    lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    ESP_LOGI(TAG, "display up: %dx%d", LCD_H_RES, LCD_V_RES);
}

bool bsp_lvgl_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void bsp_lvgl_unlock(void) { lvgl_port_unlock(); }

esp_codec_dev_handle_t bsp_mic_start(void)
{
    // Shared I2C control bus (also the touch bus, but we don't use touch here)
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    // I2S STD, master, RX only (codec is the I2S slave). esp_codec_dev
    // enables/disables the channel on open/close, so leave it un-enabled.
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_GPIO,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));

    // ES8311 in ADC (mic) mode via esp_codec_dev (§6). No PA pin for record.
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = NULL,
    });
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus,
    });
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .use_mclk = true,
        .master_mode = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    esp_codec_dev_handle_t mic = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec_if, .data_if = data_if,
    });
    assert(mic && "ES8311 mic init failed");

    esp_codec_dev_set_in_gain(mic, 30.0);  // dB; calibration knob for mic sensitivity
    ESP_ERROR_CHECK(esp_codec_dev_open(mic, &(esp_codec_dev_sample_info_t){
        .sample_rate = MIC_SAMPLE_RATE, .channel = 1, .bits_per_sample = 16,
    }));
    ESP_LOGI(TAG, "mic up: %d Hz mono", MIC_SAMPLE_RATE);
    return mic;
}
