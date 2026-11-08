#include "dsp.h"
#include "crystalizer_zita.h"

#include <math.h>
#include <string.h>

#if defined(__SSE2__) || defined(__x86_64__)
#include <immintrin.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

bool dsp_compute_eq_biquad(const EqBandConfig *band, uint32_t sample_rate, BiquadCoeffs *out) {
    if (!band->enabled) {
        out->b0 = 1.0f; out->b1 = 0.0f; out->b2 = 0.0f;
        out->a1 = 0.0f; out->a2 = 0.0f;
        return false;
    }

    if ((band->type == EQ_BAND_PEAK ||
         band->type == EQ_BAND_LOW_SHELF ||
         band->type == EQ_BAND_HIGH_SHELF) &&
        fabsf(band->gain_db) < 0.01f) {
        out->b0 = 1.0f; out->b1 = 0.0f; out->b2 = 0.0f;
        out->a1 = 0.0f; out->a2 = 0.0f;
        return false;
    }

    double fs = (sample_rate >= 8000u) ? (double)sample_rate : 48000.0;
    double f = clampf(band->freq, 15.0f, (float)(0.49 * fs));
    double q = clampf(band->q, 0.1f, 24.0f);
    double gain = clampf(band->gain_db, -30.0f, 30.0f);

    double w0 = 2.0 * M_PI * f / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double A = pow(10.0, gain / 40.0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch ((EqBandType)band->type) {
        case EQ_BAND_PEAK:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cw;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha / A;
            break;

        case EQ_BAND_LOW_SHELF: {
            double two_sqrtA_alpha = 2.0 * sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + two_sqrtA_alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - two_sqrtA_alpha);
            a0 = (A + 1.0) + (A - 1.0) * cw + two_sqrtA_alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - two_sqrtA_alpha;
            break;
        }

        case EQ_BAND_HIGH_SHELF: {
            double two_sqrtA_alpha = 2.0 * sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + two_sqrtA_alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - two_sqrtA_alpha);
            a0 = (A + 1.0) - (A - 1.0) * cw + two_sqrtA_alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - two_sqrtA_alpha;
            break;
        }

        case EQ_BAND_LOW_PASS:
            b0 = (1.0 - cw) * 0.5;
            b1 = 1.0 - cw;
            b2 = (1.0 - cw) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;

        case EQ_BAND_HIGH_PASS:
            b0 = (1.0 + cw) * 0.5;
            b1 = -(1.0 + cw);
            b2 = (1.0 + cw) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;

        case EQ_BAND_NOTCH:
        default:
            b0 = 1.0;
            b1 = -2.0 * cw;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;
    }

    double inv_a0 = 1.0 / a0;
    out->b0 = (float)(b0 * inv_a0);
    out->b1 = (float)(b1 * inv_a0);
    out->b2 = (float)(b2 * inv_a0);
    out->a1 = (float)(a1 * inv_a0);
    out->a2 = (float)(a2 * inv_a0);
    return true;
}

float dsp_eval_eq_magnitude_db(const EqBandConfig *bands, uint32_t num_bands,
                               uint32_t sample_rate, float freq_hz) {
    double fs = (sample_rate >= 8000u) ? (double)sample_rate : 48000.0;
    double w = 2.0 * M_PI * clampf(freq_hz, 10.0f, (float)(0.495 * fs)) / fs;
    double z1_re = cos(-w), z1_im = sin(-w);
    double z2_re = cos(-2.0 * w), z2_im = sin(-2.0 * w);

    uint32_t n = num_bands > LITEFX_MAX_EQ_BANDS ? LITEFX_MAX_EQ_BANDS : num_bands;
    double mag_sq_total = 1.0;
    for (uint32_t i = 0; i < n; ++i) {
        BiquadCoeffs c;
        if (!dsp_compute_eq_biquad(&bands[i], (uint32_t)fs, &c)) continue;

        double num_re = (double)c.b0 + (double)c.b1 * z1_re + (double)c.b2 * z2_re;
        double num_im = (double)c.b1 * z1_im + (double)c.b2 * z2_im;
        double den_re = 1.0 + (double)c.a1 * z1_re + (double)c.a2 * z2_re;
        double den_im = (double)c.a1 * z1_im + (double)c.a2 * z2_im;

        double num_sq = num_re * num_re + num_im * num_im;
        double den_sq = den_re * den_re + den_im * den_im;
        if (den_sq > 1e-20) {
            mag_sq_total *= (num_sq / den_sq);
        }
    }

    if (mag_sq_total < 1e-12) return -120.0f;
    return (float)(10.0 * log10(mag_sq_total));
}

float dsp_compute_auto_preamp_db(const EqBandConfig *bands_l, const EqBandConfig *bands_r,
                                 uint32_t num_bands, bool split, uint32_t sample_rate) {
    float max_db = 0.0f;
    for (int i = 0; i < 96; ++i) {
        float t = (float)i / 95.0f;
        float f = 20.0f * powf(1000.0f, t);
        float db_l = dsp_eval_eq_magnitude_db(bands_l, num_bands, sample_rate, f);
        if (db_l > max_db) max_db = db_l;
        if (split && bands_r) {
            float db_r = dsp_eval_eq_magnitude_db(bands_r, num_bands, sample_rate, f);
            if (db_r > max_db) max_db = db_r;
        }
    }
    return (max_db > 0.1f) ? -(max_db + 0.2f) : 0.0f;
}

void dsp_engine_reset_state(DspEngine *eng) {
    memset(&eng->denoise_hpf_l, 0, sizeof(eng->denoise_hpf_l));
    memset(&eng->denoise_hpf_r, 0, sizeof(eng->denoise_hpf_r));
    memset(eng->eq_state_l, 0, sizeof(eng->eq_state_l));
    memset(eng->eq_state_r, 0, sizeof(eng->eq_state_r));
    eng->rt_prev_mask_l = 0;
    eng->rt_prev_mask_r = 0;
    eng->cur_gate_gain = 1.0f;
    eng->gate_detector = 0.0f;
    eng->denoise_floor = 0.0005f;
    eng->denoise_env = 0.0f;
    eng->comp_env = 0.0f;
    eng->cur_comp_gain = 1.0f;
    if (eng->cz_engine) {
        crystalizer_zita_reset_state(eng->cz_engine);
    }
}

void dsp_engine_init(DspEngine *eng) {
    memset(eng, 0, sizeof(*eng));
    eng->rt_sample_rate = 48000u;
    eng->cur_preamp = 1.0f;
    eng->cur_postgain = 1.0f;
    eng->cur_gate_gain = 1.0f;
    eng->denoise_floor = 0.0005f;
    eng->cur_comp_gain = 1.0f;
    eng->gain_initialized = true;
    atomic_init(&eng->active_gen, 0u);
    atomic_init(&eng->reset_requested, false);

    for (uint32_t i = 0; i < LITEFX_FFT_SIZE; ++i) {
        eng->fft_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(LITEFX_FFT_SIZE - 1)));
    }
    for (int b = 0; b < LITEFX_SPECTRUM_BINS; ++b) {
        eng->spectrum_db[b] = -90.0f;
    }

    eng->cz_engine = crystalizer_zita_create(48000, 512);
}

void dsp_engine_destroy(DspEngine *eng) {
    if (eng->cz_engine) {
        crystalizer_zita_destroy(eng->cz_engine);
        eng->cz_engine = NULL;
    }
}

void dsp_engine_update_sink(DspEngine *eng, const LiteFxConfig *cfg, uint32_t sample_rate) {
    uint32_t fs = (sample_rate >= 8000u) ? sample_rate : 48000u;
    uint32_t gen = atomic_load_explicit(&eng->active_gen, memory_order_relaxed);
    uint32_t next_gen = gen + 1u;
    DspCoeffsSlot *slot = &eng->slots[next_gen & 3u];
    memset(slot, 0, sizeof(*slot));

    slot->sample_rate = fs;
    slot->bypass = cfg->sink_bypass;
    slot->soft_limiter = cfg->sink_soft_limiter;
    slot->noise_gate = false;
    slot->voice_denoiser = false;
    slot->voice_compressor = false;

    float preamp_db = cfg->sink_preamp_db;
    if (cfg->sink_auto_preamp && !cfg->sink_eq_bypass) {
        preamp_db += dsp_compute_auto_preamp_db(cfg->sink_eq_left, cfg->sink_eq_right,
                                                cfg->sink_eq_num_bands, cfg->sink_eq_split_channels, fs);
    }
    slot->target_preamp = db_to_lin(clampf(preamp_db, -36.0f, 24.0f));
    slot->target_postgain = db_to_lin(clampf(cfg->sink_postgain_db, -36.0f, 24.0f));

    slot->eq_split = cfg->sink_eq_split_channels;
    uint32_t nb = cfg->sink_eq_num_bands;
    if (nb > LITEFX_MAX_EQ_BANDS) nb = LITEFX_MAX_EQ_BANDS;

    if (!cfg->sink_eq_bypass && nb > 0) {
        for (uint32_t i = 0; i < nb; ++i) {
            if (dsp_compute_eq_biquad(&cfg->sink_eq_left[i], fs, &slot->eq_l[i])) {
                slot->eq_mask_l |= (1u << i);
            }
            const EqBandConfig *rb = cfg->sink_eq_split_channels ? &cfg->sink_eq_right[i] : &cfg->sink_eq_left[i];
            if (dsp_compute_eq_biquad(rb, fs, &slot->eq_r[i])) {
                slot->eq_mask_r |= (1u << i);
            }
        }
        slot->eq_active = (slot->eq_mask_l != 0u || slot->eq_mask_r != 0u);
    }

    if (!cfg->sink_crystal_bypass) {
        bool any_effect = false;
        for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
            const CrystalBandConfig *cb = &cfg->sink_crystal_bands[b];
            slot->crystal_bypass_band[b] = cb->bypass;
            slot->crystal_mute_band[b] = cb->mute;
            slot->crystal_db[b] = cb->intensity_db;
            if (cb->mute || (!cb->bypass && cb->intensity_db > -35.9f)) {
                any_effect = true;
            }
        }
        slot->crystal_active = any_effect;
        slot->crystal_adaptive = cfg->sink_crystal_adaptive;
    }

    atomic_store_explicit(&eng->active_gen, next_gen, memory_order_release);
}

void dsp_engine_update_source(DspEngine *eng, const LiteFxConfig *cfg, uint32_t sample_rate) {
    uint32_t fs = (sample_rate >= 8000u) ? sample_rate : 48000u;
    uint32_t gen = atomic_load_explicit(&eng->active_gen, memory_order_relaxed);
    uint32_t next_gen = gen + 1u;
    DspCoeffsSlot *slot = &eng->slots[next_gen & 3u];
    memset(slot, 0, sizeof(*slot));

    slot->sample_rate = fs;
    slot->bypass = cfg->source_bypass;
    slot->soft_limiter = cfg->source_soft_limiter;

    slot->noise_gate = cfg->source_noise_gate;
    slot->gate_thresh = db_to_lin(clampf(cfg->source_gate_threshold_db, -70.0f, -10.0f));

    slot->voice_denoiser = cfg->source_denoiser;
    slot->denoise_strength = clampf(cfg->source_denoise_strength, 0.0f, 1.0f);
    if (slot->voice_denoiser) {
        EqBandConfig hpf_band = {
            .enabled = true,
            .type = EQ_BAND_HIGH_PASS,
            .freq = 75.0f + 35.0f * slot->denoise_strength,
            .gain_db = 0.0f,
            .q = 0.707f
        };
        dsp_compute_eq_biquad(&hpf_band, fs, &slot->denoise_hpf);
    }

    slot->voice_compressor = cfg->source_compressor;
    slot->comp_amount = clampf(cfg->source_comp_amount, 0.0f, 1.0f);

    slot->target_preamp = db_to_lin(clampf(cfg->source_preamp_db, -36.0f, 24.0f));
    slot->target_postgain = db_to_lin(clampf(cfg->source_postgain_db, -36.0f, 24.0f));

    slot->eq_split = false;
    uint32_t nb = cfg->source_eq_num_bands;
    if (nb > LITEFX_MAX_EQ_BANDS) nb = LITEFX_MAX_EQ_BANDS;

    if (!cfg->source_eq_bypass && nb > 0) {
        for (uint32_t i = 0; i < nb; ++i) {
            if (dsp_compute_eq_biquad(&cfg->source_eq[i], fs, &slot->eq_l[i])) {
                slot->eq_mask_l |= (1u << i);
                slot->eq_r[i] = slot->eq_l[i];
                slot->eq_mask_r |= (1u << i);
            }
        }
        slot->eq_active = (slot->eq_mask_l != 0u);
    }

    if (!cfg->source_crystal_bypass) {
        bool any_effect = false;
        for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
            const CrystalBandConfig *cb = &cfg->source_crystal_bands[b];
            slot->crystal_bypass_band[b] = cb->bypass;
            slot->crystal_mute_band[b] = cb->mute;
            slot->crystal_db[b] = cb->intensity_db;
            if (cb->mute || (!cb->bypass && cb->intensity_db > -35.9f)) {
                any_effect = true;
            }
        }
        slot->crystal_active = any_effect;
        slot->crystal_adaptive = cfg->source_crystal_adaptive;
    }

    atomic_store_explicit(&eng->active_gen, next_gen, memory_order_release);
}

static inline void biquad_chunk_mono(const BiquadCoeffs * restrict c,
                                     BiquadStateMono * restrict s,
                                     float * restrict buf,
                                     uint32_t n) {
    const double b0 = c->b0, b1 = c->b1, b2 = c->b2;
    const double a1 = c->a1, a2 = c->a2;
    double z1 = s->z1, z2 = s->z2;
    if (isnan(z1) || isinf(z1)) z1 = 0.0;
    if (isnan(z2) || isinf(z2)) z2 = 0.0;

    for (uint32_t i = 0; i < n; ++i) {
        double x = (double)buf[i];
        if (isnan(x) || isinf(x)) x = 0.0;
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        buf[i] = (float)y;
    }
    s->z1 = (isnan(z1) || isinf(z1)) ? 0.0 : z1;
    s->z2 = (isnan(z2) || isinf(z2)) ? 0.0 : z2;
}

static inline float soft_limit_sample(float x) {
    if (isnan(x) || isinf(x)) return 0.0f;
    const float T = 0.96f;
    const float CEIL = 0.999f;
    const float HEAD = CEIL - T;
    float ax = fabsf(x);
    if (ax <= T) return x;
    float over = ax - T;
    float compressed = T + (HEAD * over) / (HEAD + over);
    return (x >= 0.0f) ? compressed : -compressed;
}

/* Fast in-place Radix-2 Cooley-Tukey 512-point complex FFT (zero allocations) */
static void compute_fft_512(float * restrict re, float * restrict im) {
    const int N = LITEFX_FFT_SIZE;
    int j = 0;
    for (int i = 0; i < N; ++i) {
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
        int m = N >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1;
        float ang = -2.0f * (float)M_PI / (float)len;
        float wlen_re = cosf(ang);
        float wlen_im = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float w_re = 1.0f;
            float w_im = 0.0f;
            for (int k = 0; k < half; ++k) {
                int u = i + k;
                int v = u + half;
                float vr = re[v] * w_re - im[v] * w_im;
                float vi = re[v] * w_im + im[v] * w_re;
                re[v] = re[u] - vr;
                im[v] = im[u] - vi;
                re[u] += vr;
                im[u] += vi;
                float next_re = w_re * wlen_re - w_im * wlen_im;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_re;
            }
        }
    }
}

static void update_spectrum_from_ring(DspEngine *eng, uint32_t fs, _Atomic(float) *out_spectrum_db) {
    float re[LITEFX_FFT_SIZE];
    float im[LITEFX_FFT_SIZE];
    uint32_t wp = eng->fft_write_pos;
    for (uint32_t i = 0; i < LITEFX_FFT_SIZE; ++i) {
        uint32_t idx = (wp + i) & (LITEFX_FFT_SIZE - 1u);
        re[i] = eng->fft_ring[idx] * eng->fft_window[i];
        im[i] = 0.0f;
    }

    compute_fft_512(re, im);

    const float bin_hz = (float)fs / (float)LITEFX_FFT_SIZE;
    const float norm = 2.0f / (float)LITEFX_FFT_SIZE;
    const int half_n = LITEFX_FFT_SIZE / 2;

    for (int b = 0; b < LITEFX_SPECTRUM_BINS; ++b) {
        float t0 = (float)b / (float)LITEFX_SPECTRUM_BINS;
        float t1 = (float)(b + 1) / (float)LITEFX_SPECTRUM_BINS;
        float f0 = 20.0f * powf(1000.0f, t0);
        float f1 = 20.0f * powf(1000.0f, t1);

        int k0 = (int)(f0 / bin_hz);
        int k1 = (int)ceilf(f1 / bin_hz);
        if (k0 < 1) k0 = 1;
        if (k0 >= half_n) k0 = half_n - 1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > half_n) k1 = half_n;

        float max_mag = 1e-6f;
        for (int k = k0; k < k1; ++k) {
            float m = sqrtf(re[k] * re[k] + im[k] * im[k]) * norm;
            if (m > max_mag) max_mag = m;
        }
        float target_db = clampf(20.0f * log10f(max_mag) + 12.0f, -72.0f, 6.0f);
        float prev = eng->spectrum_db[b];
        float cur = (target_db > prev)
            ? (prev + 0.45f * (target_db - prev))
            : (prev + 0.16f * (target_db - prev));
        eng->spectrum_db[b] = cur;
        atomic_store_explicit(&out_spectrum_db[b], cur, memory_order_relaxed);
    }
}

void dsp_engine_process_ex(DspEngine *eng,
                           const float *in_l, const float *in_r,
                           float *out_l, float *out_r,
                           uint32_t n_samples,
                           bool measure_peaks,
                           float *peak_in_l, float *peak_in_r,
                           float *peak_out_l, float *peak_out_r,
                           bool compute_spectrum,
                           _Atomic(float) *out_spectrum_db) {
    if (n_samples == 0 || !out_l || !out_r) return;

#if defined(__SSE2__) || defined(__x86_64__)
    unsigned int old_mxcsr = _mm_getcsr();
    _mm_setcsr(old_mxcsr | 0x8040u);
#endif

    if (atomic_exchange_explicit(&eng->reset_requested, false, memory_order_acq_rel)) {
        dsp_engine_reset_state(eng);
    }

    uint32_t gen = atomic_load_explicit(&eng->active_gen, memory_order_acquire);
    const DspCoeffsSlot *c = &eng->slots[gen & 3u];

    if (c->sample_rate != eng->rt_sample_rate) {
        dsp_engine_reset_state(eng);
        eng->rt_sample_rate = c->sample_rate;
    }

    float pin_l = 0.0f, pin_r = 0.0f, pout_l = 0.0f, pout_r = 0.0f;

    if (c->bypass) {
        for (uint32_t i = 0; i < n_samples; ++i) {
            float xl = in_l ? in_l[i] : 0.0f;
            float xr = in_r ? in_r[i] : 0.0f;
            if (measure_peaks) {
                float al = fabsf(xl), ar = fabsf(xr);
                if (al > pin_l) pin_l = al;
                if (ar > pin_r) pin_r = ar;
            }
            out_l[i] = xl;
            out_r[i] = xr;
        }
        pout_l = pin_l;
        pout_r = pin_r;
        goto done;
    }

    uint32_t new_l = (~eng->rt_prev_mask_l) & c->eq_mask_l;
    while (new_l) {
        int b = __builtin_ctz(new_l);
        new_l &= (new_l - 1u);
        eng->eq_state_l[b].z1 = 0.0;
        eng->eq_state_l[b].z2 = 0.0;
    }
    uint32_t new_r = (~eng->rt_prev_mask_r) & c->eq_mask_r;
    while (new_r) {
        int b = __builtin_ctz(new_r);
        new_r &= (new_r - 1u);
        eng->eq_state_r[b].z1 = 0.0;
        eng->eq_state_r[b].z2 = 0.0;
    }
    eng->rt_prev_mask_l = c->eq_mask_l;
    eng->rt_prev_mask_r = c->eq_mask_r;

    if (!eng->gain_initialized) {
        eng->cur_preamp = c->target_preamp;
        eng->cur_postgain = c->target_postgain;
        eng->gain_initialized = true;
    }

    float cur_pre = eng->cur_preamp;
    float tgt_pre = c->target_preamp;
    float cur_post = eng->cur_postgain;
    float tgt_post = c->target_postgain;
    const float gain_smooth = 0.0025f;

    const bool eq_on = c->eq_active;
    const uint32_t mask_l = c->eq_mask_l;
    const uint32_t mask_r = c->eq_mask_r;
    const bool cryst_on = c->crystal_active;
    const bool cryst_adapt = c->crystal_adaptive;
    const bool limit_on = c->soft_limiter;
    const bool gate_on = c->noise_gate;
    const float gate_th = c->gate_thresh;
    const bool denoise_on = c->voice_denoiser;
    const float denoise_str = c->denoise_strength;
    const bool comp_on = c->voice_compressor;
    const float comp_amt = c->comp_amount;

    float cur_gate = eng->cur_gate_gain;
    float det = eng->gate_detector;
    float dn_floor = eng->denoise_floor;
    float dn_env = eng->denoise_env;

    // 1. Input Preamp, Adaptive Voice Denoiser & Noise Gate
    for (uint32_t i = 0; i < n_samples; ++i) {
        float xl = in_l ? in_l[i] : 0.0f;
        float xr = in_r ? in_r[i] : 0.0f;
        if (measure_peaks) {
            float al = fabsf(xl), ar = fabsf(xr);
            if (al > pin_l) pin_l = al;
            if (ar > pin_r) pin_r = ar;
        }
        cur_pre += gain_smooth * (tgt_pre - cur_pre);
        xl *= cur_pre;
        xr *= cur_pre;

        if (denoise_on && denoise_str > 0.001f) {
            float sig = (fabsf(xl) + fabsf(xr)) * 0.5f;
            dn_env = (sig > dn_env) ? (dn_env + 0.08f * (sig - dn_env)) : (dn_env * 0.996f);
            if (dn_env < dn_floor) {
                dn_floor = dn_env;
            } else {
                dn_floor = dn_floor * 0.99992f + 0.00008f * fminf(dn_env, 0.025f);
            }
            if (dn_floor < 0.0001f) dn_floor = 0.0001f;
            float snr_thresh = dn_floor * (1.8f + 3.2f * denoise_str);
            if (dn_env < snr_thresh) {
                float ratio = dn_env / (snr_thresh + 1e-7f);
                float atten = ratio * ratio;
                float min_floor = 1.0f - 0.88f * denoise_str;
                float g_dn = min_floor + (1.0f - min_floor) * atten;
                xl *= g_dn;
                xr *= g_dn;
            }
        }

        if (gate_on) {
            float sig = (fabsf(xl) + fabsf(xr)) * 0.5f;
            det = (sig > det) ? (det + 0.15f * (sig - det)) : (det * 0.9985f);
            float tgt_gate = (det >= gate_th) ? 1.0f : (det / (gate_th + 1e-6f));
            if (tgt_gate < 0.02f) tgt_gate = 0.0f;
            cur_gate += 0.015f * (tgt_gate - cur_gate);
            xl *= cur_gate;
            xr *= cur_gate;
        }

        out_l[i] = xl;
        out_r[i] = xr;
    }

    if (denoise_on && denoise_str > 0.001f) {
        biquad_chunk_mono(&c->denoise_hpf, &eng->denoise_hpf_l, out_l, n_samples);
        biquad_chunk_mono(&c->denoise_hpf, &eng->denoise_hpf_r, out_r, n_samples);
    }

    // 2. Equalizer (Stereo / Mono biquads with double-precision accumulators)
    if (eq_on) {
        uint32_t ml = mask_l;
        while (ml) {
            int b = __builtin_ctz(ml);
            ml &= (ml - 1u);
            biquad_chunk_mono(&c->eq_l[b], &eng->eq_state_l[b], out_l, n_samples);
        }
        uint32_t mr = mask_r;
        while (mr) {
            int b = __builtin_ctz(mr);
            mr &= (mr - 1u);
            biquad_chunk_mono(&c->eq_r[b], &eng->eq_state_r[b], out_r, n_samples);
        }
    }

    // 3. Crystalizer (13-Band Linear-Phase FIR via Zita Convolver, EasyEffects-identical)
    if (cryst_on && eng->cz_engine) {
        crystalizer_zita_process(eng->cz_engine,
                                 out_l, out_r,
                                 out_l, out_r,
                                 n_samples,
                                 c->crystal_db,
                                 c->crystal_bypass_band,
                                 c->crystal_mute_band,
                                 cryst_adapt);
    }

    // 4. Broadcast Voice Compressor, Postgain & Soft Limiter
    float comp_env = eng->comp_env;
    float cur_comp = eng->cur_comp_gain;
    const float comp_thresh = 0.125f; /* -18 dBFS */
    const float comp_makeup = 1.0f + 0.65f * comp_amt;

    for (uint32_t i = 0; i < n_samples; ++i) {
        cur_post += gain_smooth * (tgt_post - cur_post);
        float xl = out_l[i] * cur_post;
        float xr = out_r[i] * cur_post;

        if (comp_on && comp_amt > 0.001f) {
            float peak = fmaxf(fabsf(xl), fabsf(xr));
            comp_env = (peak > comp_env)
                ? (comp_env + 0.015f * (peak - comp_env))
                : (comp_env * 0.9992f);
            float tgt_comp = 1.0f;
            if (comp_env > comp_thresh) {
                float slope = 0.60f * comp_amt;
                tgt_comp = powf(comp_env / comp_thresh, -slope);
            }
            cur_comp += 0.01f * (tgt_comp * comp_makeup - cur_comp);
            xl *= cur_comp;
            xr *= cur_comp;
        }

        if (limit_on) {
            xl = soft_limit_sample(xl);
            xr = soft_limit_sample(xr);
        }
        if (measure_peaks) {
            float al = fabsf(xl), ar = fabsf(xr);
            if (al > pout_l) pout_l = al;
            if (ar > pout_r) pout_r = ar;
        }
        out_l[i] = xl;
        out_r[i] = xr;
    }

    eng->cur_preamp = cur_pre;
    eng->cur_postgain = cur_post;
    eng->cur_gate_gain = cur_gate;
    eng->gate_detector = det;
    eng->denoise_floor = dn_floor;
    eng->denoise_env = dn_env;
    eng->comp_env = comp_env;
    eng->cur_comp_gain = cur_comp;

done:
    if (compute_spectrum && out_spectrum_db) {
        uint32_t wp = eng->fft_write_pos;
        for (uint32_t i = 0; i < n_samples; ++i) {
            eng->fft_ring[wp] = 0.5f * (out_l[i] + out_r[i]);
            wp = (wp + 1u) & (LITEFX_FFT_SIZE - 1u);
        }
        eng->fft_write_pos = wp;
        eng->fft_hop_accum += n_samples;
        if (eng->fft_hop_accum >= 1024u) {
            eng->fft_hop_accum = 0u;
            update_spectrum_from_ring(eng, eng->rt_sample_rate, out_spectrum_db);
        }
    }

    if (measure_peaks) {
        if (peak_in_l) *peak_in_l = pin_l;
        if (peak_in_r) *peak_in_r = pin_r;
        if (peak_out_l) *peak_out_l = pout_l;
        if (peak_out_r) *peak_out_r = pout_r;
    }

#if defined(__SSE2__) || defined(__x86_64__)
    _mm_setcsr(old_mxcsr);
#endif
}

void dsp_engine_process(DspEngine *eng,
                        const float *in_l, const float *in_r,
                        float *out_l, float *out_r,
                        uint32_t n_samples,
                        bool measure_peaks,
                        float *peak_in_l, float *peak_in_r,
                        float *peak_out_l, float *peak_out_r) {
    dsp_engine_process_ex(eng, in_l, in_r, out_l, out_r, n_samples,
                          measure_peaks, peak_in_l, peak_in_r, peak_out_l, peak_out_r,
                          false, NULL);
}
