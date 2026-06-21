// Real-time mic spectrum analyzer for the Guition JC4880P443C-I-W (ESP32-P4).
// Captures the ES8311 mic, runs an FFT, and draws a log-spaced bar graph with
// peak-hold markers, a level meter, a LISTENING/quiet light and dominant freq.
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "lvgl.h"
#include "board.h"
#include "nvs_flash.h"
#include "nvs.h"

#define FFT_N        1024                 // 64 ms frame @ 16 kHz -> ~15 fps
#define HALF_N       (FFT_N / 2)
#define BIN_HZ       ((float)MIC_SAMPLE_RATE / FFT_N)
#define N_BARS       24
#define F_LO         80.0f                // lowest band edge (Hz)
#define F_HI         (MIC_SAMPLE_RATE / 2) // Nyquist

// --- Calibration knobs (real mic, not paper) -------------------------------
// dB window mapped onto the bar/level height, and the LISTENING threshold.
// Tune these to your room: raise the floor if idle noise lights bars.
#define DB_FLOOR     25.0f
#define DB_CEIL      85.0f
#define LISTEN_DB    34.0f
#define PEAK_DECAY   3                    // px the peak marker falls per frame
#define BAR_FALL     12                   // px a bar eases down per frame (snaps up)

// --- UI geometry (native portrait 480x800) ---------------------------------
#define SCR_W        480
#define MARGIN       10
#define PLOT_W       (SCR_W - 2 * MARGIN)
#define SPEC_TOP     190
#define SPEC_H       590

static float hann[FFT_N];
static float fft[FFT_N * 2];             // interleaved {re, im}
static int   bar_lo[N_BARS], bar_hi[N_BARS];  // bin range per bar

static lv_obj_t *bars[N_BARS], *peaks[N_BARS];
static lv_obj_t *lbl_freq, *lbl_status, *level_bar;
static int peak_px[N_BARS], bar_px[N_BARS];

// Settings UI variables
static lv_obj_t *settings_modal = NULL;
static lv_obj_t *gain_slider = NULL;
static lv_obj_t *gain_val_lbl = NULL;
static lv_obj_t *setting_level_bar = NULL;

static esp_codec_dev_handle_t mic_handle = NULL;
static float current_mic_gain = 30.0f;

static void load_gain_from_nvs(void)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        int32_t val = 30;
        err = nvs_get_i32(my_handle, "mic_gain", &val);
        if (err == ESP_OK) {
            current_mic_gain = (float)val;
        }
        nvs_close(my_handle);
    }
}

static void save_gain_to_nvs(float gain)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_i32(my_handle, "mic_gain", (int32_t)gain);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

static void slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    current_mic_gain = (float)val;
    if (mic_handle) {
        esp_codec_dev_set_in_gain(mic_handle, current_mic_gain);
    }
    lv_label_set_text_fmt(gain_val_lbl, "%+d dB", (int)val);
}

static void close_click_cb(lv_event_t * e)
{
    if (settings_modal) {
        save_gain_to_nvs(current_mic_gain);
        lv_obj_delete(settings_modal);
        settings_modal = NULL;
        gain_slider = NULL;
        gain_val_lbl = NULL;
        setting_level_bar = NULL;
    }
}

static void settings_click_cb(lv_event_t * e)
{
    if (settings_modal != NULL) return; // already open
    
    lv_obj_t *scr = lv_scr_act();
    
    // Modal background
    settings_modal = lv_obj_create(scr);
    lv_obj_set_size(settings_modal, SCR_W, 800); // cover screen
    lv_obj_align(settings_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(settings_modal, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(settings_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_modal, 0, 0);
    lv_obj_set_style_radius(settings_modal, 0, 0);
    lv_obj_add_flag(settings_modal, LV_OBJ_FLAG_CLICKABLE); // prevent clicking through
    
    // Modal Title
    lv_obj_t *title = lv_label_create(settings_modal);
    lv_label_set_text(title, "MIC SENSITIVITY");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00E0FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);
    
    // Slider label
    lv_obj_t *lbl_slider_title = lv_label_create(settings_modal);
    lv_label_set_text(lbl_slider_title, "Adjust Input Gain");
    lv_obj_set_style_text_color(lbl_slider_title, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_slider_title, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_slider_title, LV_ALIGN_TOP_MID, 0, 180);
    
    // Slider
    gain_slider = lv_slider_create(settings_modal);
    lv_obj_set_size(gain_slider, PLOT_W - 40, 24);
    lv_obj_align(gain_slider, LV_ALIGN_TOP_MID, 0, 220);
    lv_slider_set_range(gain_slider, 0, 42); // 0 to 42 dB range
    lv_slider_set_value(gain_slider, (int)current_mic_gain, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(gain_slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(gain_slider, lv_color_hex(0x00E0FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(gain_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_event_cb(gain_slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Gain value text
    gain_val_lbl = lv_label_create(settings_modal);
    lv_label_set_text_fmt(gain_val_lbl, "%+d dB", (int)current_mic_gain);
    lv_obj_set_style_text_font(gain_val_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(gain_val_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(gain_val_lbl, LV_ALIGN_TOP_MID, 0, 270);
    
    // Live Level Label
    lv_obj_t *lbl_level_title = lv_label_create(settings_modal);
    lv_label_set_text(lbl_level_title, "Live Input Level");
    lv_obj_set_style_text_color(lbl_level_title, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_level_title, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_level_title, LV_ALIGN_TOP_MID, 0, 360);
    
    // Live Level Meter
    setting_level_bar = lv_bar_create(settings_modal);
    lv_obj_set_size(setting_level_bar, PLOT_W - 40, 20);
    lv_obj_align(setting_level_bar, LV_ALIGN_TOP_MID, 0, 400);
    lv_bar_set_range(setting_level_bar, 0, 100);
    lv_obj_set_style_bg_color(setting_level_bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_color(setting_level_bar, lv_color_hex(0x00C070), LV_PART_INDICATOR);
    
    // Close Button
    lv_obj_t *btn_close = lv_button_create(settings_modal);
    lv_obj_set_size(btn_close, 180, 50);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xFF4060), 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xD02040), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_close, 25, 0);
    
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Save & Close");
    lv_obj_set_style_text_color(lbl_close, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl_close, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_add_event_cb(btn_close, close_click_cb, LV_EVENT_CLICKED, NULL);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MIC SPECTRUM");
    lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lbl_freq = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_freq, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_freq, lv_color_hex(0x00E0FF), 0);
    lv_label_set_text(lbl_freq, "-- Hz");
    lv_obj_align(lbl_freq, LV_ALIGN_TOP_MID, 0, 44);

    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_status, "quiet");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x444444), 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 92);

    level_bar = lv_bar_create(scr);
    lv_obj_set_size(level_bar, PLOT_W, 16);
    lv_obj_align(level_bar, LV_ALIGN_TOP_MID, 0, 132);
    lv_bar_set_range(level_bar, 0, 100);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x00C070), LV_PART_INDICATOR);

    // Spectrum container; bars are bottom-anchored children that grow upward.
    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, PLOT_W, SPEC_H);
    lv_obj_set_pos(area, MARGIN, SPEC_TOP);

    int pitch = PLOT_W / N_BARS;
    int bar_w = pitch - 3;
    for (int i = 0; i < N_BARS; i++) {
        bars[i] = lv_obj_create(area);
        lv_obj_remove_style_all(bars[i]);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bars[i], lv_color_hex(0x00C070), 0);
        lv_obj_set_size(bars[i], bar_w, 1);
        lv_obj_set_align(bars[i], LV_ALIGN_BOTTOM_LEFT);
        lv_obj_set_pos(bars[i], i * pitch, 0);

        peaks[i] = lv_obj_create(area);
        lv_obj_remove_style_all(peaks[i]);
        lv_obj_set_style_bg_opa(peaks[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(peaks[i], lv_color_hex(0xFF4060), 0);
        lv_obj_set_size(peaks[i], bar_w, 3);
        lv_obj_set_align(peaks[i], LV_ALIGN_BOTTOM_LEFT);
        lv_obj_set_pos(peaks[i], i * pitch, 0);
    }

    // Create Settings Button on the main screen (top right)
    lv_obj_t *btn_settings = lv_button_create(scr);
    lv_obj_set_size(btn_settings, 70, 70);
    lv_obj_align(btn_settings, LV_ALIGN_TOP_RIGHT, -MARGIN, MARGIN);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_settings, 14, 0);
    lv_obj_set_style_border_width(btn_settings, 1, 0);
    lv_obj_set_style_border_color(btn_settings, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_color(btn_settings, lv_color_hex(0x00E0FF), LV_STATE_PRESSED);
    
    lv_obj_t *lbl_settings = lv_label_create(btn_settings);
    lv_label_set_text(lbl_settings, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lbl_settings, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_settings, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_add_event_cb(btn_settings, settings_click_cb, LV_EVENT_CLICKED, NULL);
}

static void compute_band_ranges(void)
{
    // Log-spaced band edges from F_LO..F_HI -> FFT bin indices.
    for (int b = 0; b < N_BARS; b++) {
        float f0 = F_LO * powf(F_HI / F_LO, (float)b / N_BARS);
        float f1 = F_LO * powf(F_HI / F_LO, (float)(b + 1) / N_BARS);
        bar_lo[b] = (int)(f0 / BIN_HZ);
        bar_hi[b] = (int)(f1 / BIN_HZ);
        if (bar_lo[b] < 1) bar_lo[b] = 1;
        if (bar_hi[b] <= bar_lo[b]) bar_hi[b] = bar_lo[b] + 1;
        if (bar_hi[b] > HALF_N) bar_hi[b] = HALF_N;
    }
}

// dB magnitude -> fraction of full scale, clamped to [0,1].
static float db_frac(float mag)
{
    float db = 20.0f * log10f(mag + 1e-6f);
    float f = (db - DB_FLOOR) / (DB_CEIL - DB_FLOOR);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static void analyze_and_draw(const int16_t *raw)
{
    // DC removal (mic has an offset) + Hann window; RMS from the raw samples.
    float mean = 0;
    for (int i = 0; i < FFT_N; i++) mean += raw[i];
    mean /= FFT_N;

    float sumsq = 0;
    for (int i = 0; i < FFT_N; i++) {
        float s = raw[i] - mean;
        sumsq += s * s;
        fft[2 * i]     = s * hann[i];
        fft[2 * i + 1] = 0;
    }
    float rms = sqrtf(sumsq / FFT_N);

    dsps_fft2r_fc32(fft, FFT_N);
    dsps_bit_rev_fc32(fft, FFT_N);

    // Single-sided magnitudes; track the dominant bin.
    static float mag[HALF_N];
    int dom = 1;
    for (int k = 1; k < HALF_N; k++) {
        float re = fft[2 * k], im = fft[2 * k + 1];
        mag[k] = sqrtf(re * re + im * im) * 2.0f / FFT_N;
        if (mag[k] > mag[dom]) dom = k;
    }

    bool listening = 20.0f * log10f(rms + 1e-6f) > LISTEN_DB;

    if (!bsp_lvgl_lock(0)) return;  // skip a frame rather than block audio
    for (int b = 0; b < N_BARS; b++) {
        float peak_mag = 0;
        for (int k = bar_lo[b]; k < bar_hi[b]; k++)
            if (mag[k] > peak_mag) peak_mag = mag[k];
        float frac = db_frac(peak_mag);
        int h = (int)(frac * SPEC_H);
        if (h < 1) h = 1;

        // Smooth: snap up on attack, ease down on decay.
        if (h >= bar_px[b]) bar_px[b] = h;
        else { bar_px[b] -= BAR_FALL; if (bar_px[b] < h) bar_px[b] = h; }
        lv_obj_set_height(bars[b], bar_px[b]);

        // Color by height: green (quiet) -> red (loud).
        lv_obj_set_style_bg_color(bars[b],
            lv_color_hsv_to_rgb((uint16_t)((1.0f - frac) * 120), 90, 100), 0);

        if (bar_px[b] >= peak_px[b]) peak_px[b] = bar_px[b];
        else { peak_px[b] -= PEAK_DECAY; if (peak_px[b] < bar_px[b]) peak_px[b] = bar_px[b]; }
        lv_obj_set_y(peaks[b], -peak_px[b]);
    }

    lv_bar_set_value(level_bar, (int)(db_frac(rms) * 100), LV_ANIM_OFF);
    if (setting_level_bar) {
        lv_bar_set_value(setting_level_bar, (int)(db_frac(rms) * 100), LV_ANIM_OFF);
    }

    if (listening) {
        lv_label_set_text_fmt(lbl_freq, "%d Hz", (int)(dom * BIN_HZ));
        lv_label_set_text(lbl_status, "LISTENING");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00FF80), 0);
    } else {
        lv_label_set_text(lbl_status, "quiet");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x444444), 0);
    }
    bsp_lvgl_unlock();
}

static void audio_task(void *arg)
{
    esp_codec_dev_handle_t mic = (esp_codec_dev_handle_t)arg;
    int16_t *raw = malloc(FFT_N * sizeof(int16_t));
    assert(raw);
    while (1) {
        if (esp_codec_dev_read(mic, raw, FFT_N * sizeof(int16_t)) == ESP_OK)
            analyze_and_draw(raw);
        else
            vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    // Initialize NVS for persisting mic sensitivity
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_gain_from_nvs();

    dsps_fft2r_init_fc32(NULL, FFT_N);
    for (int i = 0; i < FFT_N; i++)
        hann[i] = 0.5f * (1 - cosf(2 * M_PI * i / (FFT_N - 1)));
    compute_band_ranges();

    bsp_display_start();
    bsp_lvgl_lock(-1);
    build_ui();
    bsp_lvgl_unlock();

    mic_handle = bsp_mic_start();
    esp_codec_dev_set_in_gain(mic_handle, current_mic_gain);
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, mic_handle, 5, NULL, 1);
}
