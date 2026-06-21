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
    dsps_fft2r_init_fc32(NULL, FFT_N);
    for (int i = 0; i < FFT_N; i++)
        hann[i] = 0.5f * (1 - cosf(2 * M_PI * i / (FFT_N - 1)));
    compute_band_ranges();

    bsp_display_start();
    bsp_lvgl_lock(-1);
    build_ui();
    bsp_lvgl_unlock();

    esp_codec_dev_handle_t mic = bsp_mic_start();
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, mic, 5, NULL, 1);
}
