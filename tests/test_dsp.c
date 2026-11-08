#include "../src/common.h"
#include "../src/config.h"
#include "../src/dsp.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float measure_sine_gain_db(DspEngine *eng, float freq_hz, uint32_t fs) {
    dsp_engine_reset_state(eng);
    const uint32_t warmup = 16384;
    const uint32_t measure = 16384;
    double cycles = round((double)freq_hz * (double)measure / (double)fs);
    if (cycles < 1.0) cycles = 1.0;
    double exact_freq = cycles * (double)fs / (double)measure;

    float in_l[1024], in_r[1024], out_l[1024], out_r[1024];
    uint32_t total = warmup + measure;
    double sum_sq = 0.0;
    uint32_t counted = 0;

    for (uint32_t pos = 0; pos < total; pos += 1024) {
        for (uint32_t i = 0; i < 1024; ++i) {
            double t = (double)(pos + i) / (double)fs;
            float s = (float)(0.25 * sin(2.0 * M_PI * exact_freq * t));
            in_l[i] = s;
            in_r[i] = s;
        }
        dsp_engine_process(eng, in_l, in_r, out_l, out_r, 1024,
                           false, NULL, NULL, NULL, NULL);
        if (pos >= warmup) {
            for (uint32_t i = 0; i < 1024; ++i) {
                sum_sq += (double)out_l[i] * (double)out_l[i];
                counted++;
            }
        }
    }

    double rms_out = sqrt(sum_sq / (double)counted);
    double rms_in = 0.25 / sqrt(2.0);
    return (float)(20.0 * log10(rms_out / rms_in));
}

int main(void) {
    printf("=== LiteFX DSP & Config Verification Suite ===\n");
    const uint32_t fs = 48000;

    LiteFxConfig cfg;
    litefx_config_set_defaults(&cfg);
    cfg.sink_soft_limiter = false;
    cfg.sink_auto_preamp = false;
    cfg.sink_eq_bypass = true;

    /* 1. Test 13-Band Linear-Phase FIR Filter Magnitude Flatness */
    cfg.sink_crystal_bypass = false;
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        cfg.sink_crystal_bands[b].bypass = true;
        cfg.sink_crystal_bands[b].mute = false;
        cfg.sink_crystal_bands[b].intensity_db = 0.0f;
    }

    DspEngine eng;
    dsp_engine_init(&eng);
    dsp_engine_update_sink(&eng, &cfg, fs);

    printf("[1] Testing 13-band Zita linear-phase FIR magnitude flatness (120 Hz .. 15 kHz):\n");
    const float test_freqs[] = {
        120.0f, 250.0f, 500.0f, 750.0f, 1000.0f, 1500.0f, 2000.0f,
        3000.0f, 4000.0f, 5000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f,
        10000.0f, 12500.0f, 15000.0f
    };
    float max_err_db = 0.0f;
    for (size_t i = 0; i < sizeof(test_freqs) / sizeof(test_freqs[0]); ++i) {
        float db = measure_sine_gain_db(&eng, test_freqs[i], fs);
        float err = fabsf(db);
        if (err > max_err_db) max_err_db = err;
    }
    printf("    Max linear-phase reconstruction ripple across 17 frequencies: %.6f dB\n", max_err_db);
    assert(max_err_db < 0.25f);

    /* 2. Test Crystalizer transient peak sharpening */
    float burst_in[512] = {0}, zero[512] = {0}, tmp_out[512] = {0}, tmp_r[512] = {0};
    for (int i = 40; i < 160; ++i) {
        float env = sinf((float)(i - 40) * (float)M_PI / 120.0f);
        burst_in[i] = 0.25f * env * sinf(2.0f * (float)M_PI * 4000.0f * (float)i / (float)fs);
    }

    cfg.sink_eq_bypass = true;
    cfg.sink_crystal_bypass = false;
    cfg.sink_soft_limiter = false;
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        cfg.sink_crystal_bands[b].bypass = true;
        cfg.sink_crystal_bands[b].mute = false;
        cfg.sink_crystal_bands[b].intensity_db = 0.0f;
    }
    dsp_engine_update_sink(&eng, &cfg, fs);
    dsp_engine_reset_state(&eng);
    float p_neut_l = 0, p1 = 0, p2 = 0;
    dsp_engine_process(&eng, burst_in, burst_in, tmp_out, tmp_r, 512, true, NULL, NULL, &p1, NULL);
    dsp_engine_process(&eng, zero, zero, tmp_out, tmp_r, 512, true, NULL, NULL, &p2, NULL);
    p_neut_l = (p1 > p2) ? p1 : p2;

    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        cfg.sink_crystal_bands[b].bypass = false;
        cfg.sink_crystal_bands[b].intensity_db = 0.0f;
    }
    dsp_engine_update_sink(&eng, &cfg, fs);
    dsp_engine_reset_state(&eng);
    float p_out_l = 0;
    dsp_engine_process(&eng, burst_in, burst_in, tmp_out, tmp_r, 512, true, NULL, NULL, &p1, NULL);
    dsp_engine_process(&eng, zero, zero, tmp_out, tmp_r, 512, true, NULL, NULL, &p2, NULL);
    p_out_l = (p1 > p2) ? p1 : p2;

    printf("[2] Crystalizer 4 kHz transient enhancement: neutral_peak=%.4f -> enhanced_peak=%.4f (enhancement=%.2fx)\n",
           p_neut_l, p_out_l, p_out_l / p_neut_l);
    assert(p_out_l > p_neut_l * 1.25f);

    /* 3. Equalizer accuracy */
    litefx_config_set_defaults(&cfg);
    cfg.sink_crystal_bypass = true;
    cfg.sink_eq_bypass = false;
    cfg.sink_auto_preamp = false;
    cfg.sink_soft_limiter = false;
    cfg.sink_eq_left[5].freq = 1000.0f;
    cfg.sink_eq_left[5].gain_db = 6.0f;
    cfg.sink_eq_left[5].q = 1.414f;
    cfg.sink_eq_right[5] = cfg.sink_eq_left[5];
    dsp_engine_update_sink(&eng, &cfg, fs);

    float measured_1k = measure_sine_gain_db(&eng, 1000.0f, fs);
    float analytic_1k = dsp_eval_eq_magnitude_db(cfg.sink_eq_left, cfg.sink_eq_num_bands, fs, 1000.0f);
    printf("[3] Equalizer +6.0 dB @ 1 kHz: measured=%.4f dB, analytic=%.4f dB\n",
           measured_1k, analytic_1k);
    assert(fabsf(measured_1k - 6.0f) < 0.05f);
    assert(fabsf(measured_1k - analytic_1k) < 0.02f);

    /* 3b. Low Shelf Stability and Accuracy Test (verifies AutoEQ Band #1 stability) */
    litefx_config_set_defaults(&cfg);
    cfg.sink_crystal_bypass = true;
    cfg.sink_eq_bypass = false;
    cfg.sink_auto_preamp = false;
    cfg.sink_soft_limiter = false;
    cfg.sink_eq_left[0].enabled = true;
    cfg.sink_eq_left[0].type = EQ_BAND_LOW_SHELF;
    cfg.sink_eq_left[0].freq = 105.0f;
    cfg.sink_eq_left[0].gain_db = 4.1f;
    cfg.sink_eq_left[0].q = 0.70f;
    for (int i = 1; i < LITEFX_MAX_EQ_BANDS; ++i) cfg.sink_eq_left[i].enabled = false;
    cfg.sink_eq_right[0] = cfg.sink_eq_left[0];
    dsp_engine_update_sink(&eng, &cfg, fs);

    float measured_ls = measure_sine_gain_db(&eng, 50.0f, fs);
    float analytic_ls = dsp_eval_eq_magnitude_db(cfg.sink_eq_left, cfg.sink_eq_num_bands, fs, 50.0f);
    printf("[3b] Low Shelf +4.1 dB @ 105 Hz (measured @ 50 Hz): measured=%.4f dB, analytic=%.4f dB\n",
           measured_ls, analytic_ls);
    assert(!isnan(measured_ls) && !isinf(measured_ls));
    assert(fabsf(measured_ls - analytic_ls) < 0.15f);

    /* 4. Microphone Noise Gate, Denoiser & Compressor Test */
    litefx_config_set_defaults(&cfg);
    cfg.source_noise_gate = true;
    cfg.source_gate_threshold_db = -30.0f;
    cfg.source_denoiser = true;
    cfg.source_denoise_strength = 0.75f;
    cfg.source_compressor = true;
    cfg.source_comp_amount = 0.6f;
    dsp_engine_update_source(&eng, &cfg, fs);
    dsp_engine_reset_state(&eng);

    float silence[512] = {0};
    for (int i = 0; i < 512; ++i) silence[i] = 0.005f;
    float gate_out[512];
    dsp_engine_process(&eng, silence, silence, gate_out, tmp_r, 512, false, NULL, NULL, NULL, NULL);
    printf("[4] Mic Noise Gate + Denoiser + Compressor (input=0.005, threshold=-30dB): gated_output=%.6f\n", gate_out[511]);
    assert(gate_out[511] < 0.002f);

    /* 5. Real-time FFT Spectrum Analyzer Test */
    cfg.source_crystal_bypass = true;
    cfg.source_noise_gate = false;
    dsp_engine_update_source(&eng, &cfg, fs);
    dsp_engine_reset_state(&eng);
    _Atomic(float) spec_bins[LITEFX_SPECTRUM_BINS];
    for (int i = 0; i < LITEFX_SPECTRUM_BINS; ++i) atomic_init(&spec_bins[i], -80.0f);
    float sine_1k[512];
    for (int i = 0; i < 512; ++i) {
        sine_1k[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)i / (float)fs);
    }
    for (int blk = 0; blk < 8; ++blk) {
        dsp_engine_process_ex(&eng, sine_1k, sine_1k, tmp_out, tmp_r, 512, false, NULL, NULL, NULL, NULL, true, spec_bins);
    }
    float peak_spec_db = -80.0f;
    for (int i = 0; i < LITEFX_SPECTRUM_BINS; ++i) {
        float v = atomic_load(&spec_bins[i]);
        if (v > peak_spec_db) peak_spec_db = v;
    }
    printf("[5] Real-time 512-point FFT Spectrum peak @ 1 kHz: %.2f dB\n", peak_spec_db);
    assert(peak_spec_db > -30.0f);

    /* 6. AutoEQ ParametricEQ.txt Import Test */
    const char *tmp_autoeq = "/tmp/litefx_test_autoeq.txt";
    FILE *fa = fopen(tmp_autoeq, "w");
    assert(fa != NULL);
    fprintf(fa,
            "Preamp: -6.2 dB\n"
            "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\n"
            "Filter 2: ON PK Fc 1200 Hz Gain -2.4 dB Q 1.85\n"
            "Filter 3: ON HSC Fc 10000 Hz Gain -3.1 dB Q 0.70\n");
    fclose(fa);

    char msg[256] = {0};
    bool ok = litefx_config_import_autoeq(&cfg, tmp_autoeq, false, msg, sizeof(msg));
    unlink(tmp_autoeq);
    assert(ok);
    assert(fabsf(cfg.sink_preamp_db - (-6.2f)) < 0.01f);
    assert(cfg.sink_eq_left[0].type == EQ_BAND_LOW_SHELF);
    assert(fabsf(cfg.sink_eq_left[0].freq - 105.0f) < 0.1f);
    assert(fabsf(cfg.sink_eq_left[1].gain_db - (-2.4f)) < 0.01f);
    printf("[6] AutoEQ importer test: %s\n", msg);

    dsp_engine_destroy(&eng);
    printf("ALL DSP & CONFIG TESTS PASSED!\n");
    return 0;
}
