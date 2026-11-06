#include "crystalizer_zita.h"

#include <zita-convolver.h>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr uint32_t NBANDS = LITEFX_CRYSTAL_BANDS;
constexpr float TRANSITION_BAND = 120.0f;
constexpr float ATTACK_TIME = 0.400f;  /* 400 ms (EasyEffects-identical slow adaptation) */
constexpr float RELEASE_TIME = 3.000f; /* 3000 ms (EasyEffects-identical slow release) */

inline float db_to_lin(float db) {
    return std::pow(10.0f, db / 20.0f);
}

static std::vector<float> make_bandpass_kernel(float min_f, float max_f, float trans_band, uint32_t rate) {
    float b = trans_band / (float)rate;
    uint32_t M = (uint32_t)std::ceil(4.0f / b);
    if (M % 2 != 0) M++;
    std::vector<float> lp(M + 1), hp(M + 1), bp(M + 1);

    // LP kernel (max_f)
    float fc_lp = max_f / (float)rate;
    float sum_lp = 0.0f;
    for (size_t n = 0; n <= M; ++n) {
        float val = (n == M / 2) ? (2.0f * (float)M_PI * fc_lp)
                                 : (std::sin(2.0f * (float)M_PI * fc_lp * (float)(n - M/2)) / (float)(n - M/2));
        float w = 0.42f - 0.5f * std::cos(2.0f * (float)M_PI * (float)n / (float)M)
                        + 0.08f * std::cos(4.0f * (float)M_PI * (float)n / (float)M);
        lp[n] = val * w;
        sum_lp += lp[n];
    }
    if (sum_lp != 0.0f) {
        for (size_t n = 0; n <= M; ++n) lp[n] /= sum_lp;
    }

    // HP kernel (min_f)
    float fc_hp = min_f / (float)rate;
    float sum_hp = 0.0f;
    for (size_t n = 0; n <= M; ++n) {
        float val = (n == M / 2) ? (2.0f * (float)M_PI * fc_hp)
                                 : (std::sin(2.0f * (float)M_PI * fc_hp * (float)(n - M/2)) / (float)(n - M/2));
        float w = 0.42f - 0.5f * std::cos(2.0f * (float)M_PI * (float)n / (float)M)
                        + 0.08f * std::cos(4.0f * (float)M_PI * (float)n / (float)M);
        hp[n] = val * w;
        sum_hp += hp[n];
    }
    if (sum_hp != 0.0f) {
        for (size_t n = 0; n <= M; ++n) hp[n] = -hp[n] / sum_hp;
    }
    hp[M / 2] += 1.0f;

    // Bandpass = Spectral Inversion of (LP + HP)
    for (size_t n = 0; n <= M; ++n) {
        bp[n] = -(lp[n] + hp[n]);
    }
    bp[M / 2] += 1.0f;
    return bp;
}

inline float extrapolate_next(const float *x, size_t n) {
    if (n < 2) return (n == 1) ? x[0] : 0.0f;
    return 2.0f * x[n - 1] - x[n - 2];
}

inline float compute_kurtosis(const float *data, size_t n) {
    float mean = 0.0f;
    for (size_t i = 0; i < n; ++i) mean += data[i];
    mean /= (float)n;

    float m2 = 0.0f, m4 = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float dev = data[i] - mean;
        float dev2 = dev * dev;
        m2 += dev2;
        m4 += dev2 * dev2;
    }
    m2 /= (float)n;
    m4 /= (float)n;
    return (m2 > 1e-6f) ? (m4 / (m2 * m2)) : 3.0f;
}

inline float compute_crest(const float *data, size_t n) {
    float rms = 0.0f, peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float v = std::fabs(data[i]);
        rms += v * v;
        if (v > peak) peak = v;
    }
    rms = std::sqrt(rms / (float)n);
    return (rms > 1e-6f) ? (peak / rms) : 1.0f;
}

inline float compute_spectral_flux(const float *data, float *prev, size_t n) {
    float flux = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        flux += std::fabs(data[i] - prev[i]);
        prev[i] = data[i];
    }
    flux /= (float)n;
    return (flux > 1e-5f) ? flux : 1e-5f;
}

} // namespace

struct CrystalizerZita {
    std::mutex              lock;
    uint32_t                sample_rate;
    uint32_t                blocksize;
    bool                    ready;

    std::array<Convproc*, NBANDS> convs;
    std::array<std::vector<float>, NBANDS> kernels;

    std::array<float, NBANDS> band_prev_l;
    std::array<float, NBANDS> band_prev_r;
    std::array<std::vector<float>, NBANDS> band_prev_data_l;
    std::array<std::vector<float>, NBANDS> band_prev_data_r;

    std::array<float, NBANDS> env_kurtosis_l;
    std::array<float, NBANDS> env_kurtosis_r;
    std::array<float, NBANDS> env_crest_l;
    std::array<float, NBANDS> env_crest_r;
    std::array<float, NBANDS> env_flux_l;
    std::array<float, NBANDS> env_flux_r;

    float                   global_prev_l;
    float                   global_prev_r;
    std::vector<float>      global_prev_data_l;
    std::vector<float>      global_prev_data_r;
    float                   global_kurtosis_l;
    float                   global_kurtosis_r;
    float                   global_crest_l;
    float                   global_crest_r;
    float                   global_flux_l;
    float                   global_flux_r;

    float                   attack_coeff;
    float                   release_coeff;
    bool                    first_buffer;

    // Intermediate processing buffers
    std::array<std::vector<float>, NBANDS> band_buf_l;
    std::array<std::vector<float>, NBANDS> band_buf_r;
    std::array<std::vector<float>, NBANDS> band_d2_l;
    std::array<std::vector<float>, NBANDS> band_d2_r;
    std::vector<float>      global_d2_l;
    std::vector<float>      global_d2_r;
    std::vector<float>      scratch_in_l;
    std::vector<float>      scratch_in_r;
    std::vector<float>      scratch_out_l;
    std::vector<float>      scratch_out_r;
};

static void cz_init_filters(CrystalizerZita *cz) {
    float fmin = 20.0f, fmax = 20000.0f, gamma = 0.413f;
    float r = fmax / fmin;
    std::array<float, NBANDS + 1> edges;
    for (uint32_t i = 0; i <= NBANDS; ++i) {
        float t = (float)i / (float)NBANDS;
        edges[i] = fmin * std::pow(r, std::pow(t, gamma));
    }

    uint32_t bs = cz->blocksize;
    if (bs < 64) bs = 64;
    if (bs > 8192) bs = 8192;
    uint32_t p2 = 64;
    while (p2 * 2 <= bs && p2 * 2 <= 8192) p2 *= 2;
    bs = p2;
    cz->blocksize = bs;

    float block_time = (float)bs / (float)cz->sample_rate;
    cz->attack_coeff = std::exp(-block_time / ATTACK_TIME);
    cz->release_coeff = std::exp(-block_time / RELEASE_TIME);

    for (uint32_t b = 0; b < NBANDS; ++b) {
        if (cz->convs[b]) {
            cz->convs[b]->stop_process();
            delete cz->convs[b];
            cz->convs[b] = nullptr;
        }

        cz->kernels[b] = make_bandpass_kernel(edges[b], edges[b + 1], TRANSITION_BAND, cz->sample_rate);
        cz->convs[b] = new Convproc();
        cz->convs[b]->set_options(0);

        int ret = cz->convs[b]->configure(2, 2, cz->kernels[b].size(), bs, bs, Convproc::MAXPART, 0.5f);
        if (ret == 0) {
            cz->convs[b]->impdata_create(0, 0, 1, cz->kernels[b].data(), 0, (int)cz->kernels[b].size());
            cz->convs[b]->impdata_create(1, 1, 1, cz->kernels[b].data(), 0, (int)cz->kernels[b].size());
            cz->convs[b]->start_process(0, 0);
        }

        cz->band_prev_l[b] = 0.0f;
        cz->band_prev_r[b] = 0.0f;
        cz->band_prev_data_l[b].assign(bs, 0.0f);
        cz->band_prev_data_r[b].assign(bs, 0.0f);
        cz->env_kurtosis_l[b] = 3.0f;
        cz->env_kurtosis_r[b] = 3.0f;
        cz->env_crest_l[b] = 1.0f;
        cz->env_crest_r[b] = 1.0f;
        cz->env_flux_l[b] = 1.0f;
        cz->env_flux_r[b] = 1.0f;

        cz->band_buf_l[b].resize(bs);
        cz->band_buf_r[b].resize(bs);
        cz->band_d2_l[b].resize(bs);
        cz->band_d2_r[b].resize(bs);
    }

    cz->global_prev_l = 0.0f;
    cz->global_prev_r = 0.0f;
    cz->global_prev_data_l.assign(bs, 0.0f);
    cz->global_prev_data_r.assign(bs, 0.0f);
    cz->global_kurtosis_l = 3.0f;
    cz->global_kurtosis_r = 3.0f;
    cz->global_crest_l = 1.0f;
    cz->global_crest_r = 1.0f;
    cz->global_flux_l = 1.0f;
    cz->global_flux_r = 1.0f;

    cz->global_d2_l.resize(bs);
    cz->global_d2_r.resize(bs);
    cz->scratch_in_l.assign(bs, 0.0f);
    cz->scratch_in_r.assign(bs, 0.0f);
    cz->scratch_out_l.assign(bs, 0.0f);
    cz->scratch_out_r.assign(bs, 0.0f);
    cz->first_buffer = true;
    cz->ready = true;
}

CrystalizerZita *crystalizer_zita_create(uint32_t sample_rate, uint32_t blocksize) {
    auto *cz = new CrystalizerZita();
    cz->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    cz->blocksize = blocksize > 0 ? blocksize : 512;
    cz->ready = false;
    for (uint32_t b = 0; b < NBANDS; ++b) cz->convs[b] = nullptr;

    cz_init_filters(cz);
    return cz;
}

void crystalizer_zita_destroy(CrystalizerZita *cz) {
    if (!cz) return;
    std::scoped_lock lock(cz->lock);
    for (uint32_t b = 0; b < NBANDS; ++b) {
        if (cz->convs[b]) {
            cz->convs[b]->stop_process();
            delete cz->convs[b];
            cz->convs[b] = nullptr;
        }
    }
    delete cz;
}

void crystalizer_zita_reconfigure(CrystalizerZita *cz, uint32_t sample_rate, uint32_t blocksize) {
    if (!cz) return;
    std::scoped_lock lock(cz->lock);
    if (cz->sample_rate == sample_rate && cz->blocksize == blocksize && cz->ready) return;
    cz->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    cz->blocksize = blocksize > 0 ? blocksize : 512;
    cz->ready = false;
    cz_init_filters(cz);
}

void crystalizer_zita_reset_state(CrystalizerZita *cz) {
    if (!cz) return;
    std::scoped_lock lock(cz->lock);
    cz->first_buffer = true;
    for (uint32_t b = 0; b < NBANDS; ++b) {
        cz->band_prev_l[b] = 0.0f;
        cz->band_prev_r[b] = 0.0f;
        cz->band_prev_data_l[b].assign(cz->blocksize, 0.0f);
        cz->band_prev_data_r[b].assign(cz->blocksize, 0.0f);
        cz->env_kurtosis_l[b] = 3.0f;
        cz->env_kurtosis_r[b] = 3.0f;
        cz->env_crest_l[b] = 1.0f;
        cz->env_crest_r[b] = 1.0f;
        cz->env_flux_l[b] = 1.0f;
        cz->env_flux_r[b] = 1.0f;
        if (cz->convs[b]) cz->convs[b]->reset();
    }
    cz->global_prev_l = 0.0f;
    cz->global_prev_r = 0.0f;
    cz->global_prev_data_l.assign(cz->blocksize, 0.0f);
    cz->global_prev_data_r.assign(cz->blocksize, 0.0f);
    cz->global_kurtosis_l = 3.0f;
    cz->global_kurtosis_r = 3.0f;
    cz->global_crest_l = 1.0f;
    cz->global_crest_r = 1.0f;
    cz->global_flux_l = 1.0f;
    cz->global_flux_r = 1.0f;
}

static void cz_process_block(CrystalizerZita *cz,
                             const float *in_l, const float *in_r,
                             float *out_l, float *out_r,
                             uint32_t bs,
                             const float *intensities_db,
                             const bool *bypasses,
                             const bool *mutes,
                             bool adaptive) {
    // Filter each band with Zita linear-phase FIR
    for (uint32_t b = 0; b < NBANDS; ++b) {
        std::memcpy(cz->convs[b]->inpdata(0), in_l, bs * sizeof(float));
        std::memcpy(cz->convs[b]->inpdata(1), in_r, bs * sizeof(float));
        cz->convs[b]->process(true);
        std::memcpy(cz->band_buf_l[b].data(), cz->convs[b]->outdata(0), bs * sizeof(float));
        std::memcpy(cz->band_buf_r[b].data(), cz->convs[b]->outdata(1), bs * sizeof(float));
    }

    // Adaptive intensity metrics (EasyEffects algorithm)
    if (adaptive) {
        if (cz->first_buffer) {
            cz->global_prev_l = in_l[0];
            cz->global_prev_r = in_r[0];
            std::fill(cz->global_prev_data_l.begin(), cz->global_prev_data_l.end(), 0.0f);
            std::fill(cz->global_prev_data_r.begin(), cz->global_prev_data_r.end(), 0.0f);
        }
        float next_gl_l = extrapolate_next(in_l, bs);
        float next_gl_r = extrapolate_next(in_r, bs);

        float *d2_gl_l = cz->global_d2_l.data();
        float *d2_gl_r = cz->global_d2_r.data();
        d2_gl_l[0] = in_l[1] - 2.0f * in_l[0] + cz->global_prev_l;
        d2_gl_r[0] = in_r[1] - 2.0f * in_r[0] + cz->global_prev_r;
        for (uint32_t m = 1; m < bs - 1; ++m) {
            d2_gl_l[m] = in_l[m + 1] - 2.0f * in_l[m] + in_l[m - 1];
            d2_gl_r[m] = in_r[m + 1] - 2.0f * in_r[m] + in_r[m - 1];
        }
        d2_gl_l[bs - 1] = next_gl_l - 2.0f * in_l[bs - 1] + in_l[bs - 2];
        d2_gl_r[bs - 1] = next_gl_r - 2.0f * in_r[bs - 1] + in_r[bs - 2];

        cz->global_prev_l = in_l[bs - 1];
        cz->global_prev_r = in_r[bs - 1];

        // Global metrics
        float crest_l = compute_crest(d2_gl_l, bs);
        float crest_r = compute_crest(d2_gl_r, bs);
        cz->global_crest_l = (crest_l > cz->global_crest_l ? cz->attack_coeff : cz->release_coeff) * cz->global_crest_l
                           + (1.0f - (crest_l > cz->global_crest_l ? cz->attack_coeff : cz->release_coeff)) * crest_l;
        cz->global_crest_r = (crest_r > cz->global_crest_r ? cz->attack_coeff : cz->release_coeff) * cz->global_crest_r
                           + (1.0f - (crest_r > cz->global_crest_r ? cz->attack_coeff : cz->release_coeff)) * crest_r;

        float kurt_l = compute_kurtosis(d2_gl_l, bs);
        float kurt_r = compute_kurtosis(d2_gl_r, bs);
        cz->global_kurtosis_l = (kurt_l > cz->global_kurtosis_l ? cz->attack_coeff : cz->release_coeff) * cz->global_kurtosis_l
                              + (1.0f - (kurt_l > cz->global_kurtosis_l ? cz->attack_coeff : cz->release_coeff)) * kurt_l;
        cz->global_kurtosis_r = (kurt_r > cz->global_kurtosis_r ? cz->attack_coeff : cz->release_coeff) * cz->global_kurtosis_r
                              + (1.0f - (kurt_r > cz->global_kurtosis_r ? cz->attack_coeff : cz->release_coeff)) * kurt_r;

        float flux_l = compute_spectral_flux(d2_gl_l, cz->global_prev_data_l.data(), bs);
        float flux_r = compute_spectral_flux(d2_gl_r, cz->global_prev_data_r.data(), bs);
        cz->global_flux_l = (flux_l > cz->global_flux_l ? cz->attack_coeff : cz->release_coeff) * cz->global_flux_l
                          + (1.0f - (flux_l > cz->global_flux_l ? cz->attack_coeff : cz->release_coeff)) * flux_l;
        cz->global_flux_r = (flux_r > cz->global_flux_r ? cz->attack_coeff : cz->release_coeff) * cz->global_flux_r
                          + (1.0f - (flux_r > cz->global_flux_r ? cz->attack_coeff : cz->release_coeff)) * flux_r;
    }

    // Process each band
    for (uint32_t b = 0; b < NBANDS; ++b) {
        float *band_l = cz->band_buf_l[b].data();
        float *band_r = cz->band_buf_r[b].data();

        if (cz->first_buffer) {
            cz->band_prev_l[b] = band_l[0];
            cz->band_prev_r[b] = band_r[0];
            std::fill(cz->band_prev_data_l[b].begin(), cz->band_prev_data_l[b].end(), 0.0f);
            std::fill(cz->band_prev_data_r[b].begin(), cz->band_prev_data_r[b].end(), 0.0f);
        }

        if (!bypasses[b]) {
            float next_l = extrapolate_next(band_l, bs);
            float next_r = extrapolate_next(band_r, bs);

            float *d2_l = cz->band_d2_l[b].data();
            float *d2_r = cz->band_d2_r[b].data();

            d2_l[0] = band_l[1] - 2.0f * band_l[0] + cz->band_prev_l[b];
            d2_r[0] = band_r[1] - 2.0f * band_r[0] + cz->band_prev_r[b];

            for (uint32_t m = 1; m < bs - 1; ++m) {
                d2_l[m] = band_l[m + 1] - 2.0f * band_l[m] + band_l[m - 1];
                d2_r[m] = band_r[m + 1] - 2.0f * band_r[m] + band_r[m - 1];
            }
            d2_l[bs - 1] = next_l - 2.0f * band_l[bs - 1] + band_l[bs - 2];
            d2_r[bs - 1] = next_r - 2.0f * band_r[bs - 1] + band_r[bs - 2];

            float base_intensity = db_to_lin(intensities_db[b]);
            float intensity_l = base_intensity;
            float intensity_r = base_intensity;

            if (adaptive) {
                float crest_l = compute_crest(d2_l, bs);
                float crest_r = compute_crest(d2_r, bs);
                cz->env_crest_l[b] = (crest_l > cz->env_crest_l[b] ? cz->attack_coeff : cz->release_coeff) * cz->env_crest_l[b]
                                   + (1.0f - (crest_l > cz->env_crest_l[b] ? cz->attack_coeff : cz->release_coeff)) * crest_l;
                cz->env_crest_r[b] = (crest_r > cz->env_crest_r[b] ? cz->attack_coeff : cz->release_coeff) * cz->env_crest_r[b]
                                   + (1.0f - (crest_r > cz->env_crest_r[b] ? cz->attack_coeff : cz->release_coeff)) * crest_r;
                float crest_r_l = (cz->env_crest_l[b] > 1e-5f) ? (cz->global_crest_l / cz->env_crest_l[b]) : 1.0f;
                float crest_r_r = (cz->env_crest_r[b] > 1e-5f) ? (cz->global_crest_r / cz->env_crest_r[b]) : 1.0f;

                float kurt_l = compute_kurtosis(d2_l, bs);
                float kurt_r = compute_kurtosis(d2_r, bs);
                cz->env_kurtosis_l[b] = (kurt_l > cz->env_kurtosis_l[b] ? cz->attack_coeff : cz->release_coeff) * cz->env_kurtosis_l[b]
                                      + (1.0f - (kurt_l > cz->env_kurtosis_l[b] ? cz->attack_coeff : cz->release_coeff)) * kurt_l;
                cz->env_kurtosis_r[b] = (kurt_r > cz->env_kurtosis_r[b] ? cz->attack_coeff : cz->release_coeff) * cz->env_kurtosis_r[b]
                                      + (1.0f - (kurt_r > cz->env_kurtosis_r[b] ? cz->attack_coeff : cz->release_coeff)) * kurt_r;
                float kurt_r_l = (cz->env_kurtosis_l[b] > 1e-5f) ? (cz->global_kurtosis_l / cz->env_kurtosis_l[b]) : 1.0f;
                float kurt_r_r = (cz->env_kurtosis_r[b] > 1e-5f) ? (cz->global_kurtosis_r / cz->env_kurtosis_r[b]) : 1.0f;

                float flux_l = compute_spectral_flux(d2_l, cz->band_prev_data_l[b].data(), bs);
                float flux_r = compute_spectral_flux(d2_r, cz->band_prev_data_r[b].data(), bs);
                cz->env_flux_l[b] = (flux_l > cz->env_flux_l[b] ? cz->attack_coeff : cz->release_coeff) * cz->env_flux_l[b]
                                  + (1.0f - (flux_l > cz->env_flux_l[b] ? cz->attack_coeff : cz->release_coeff)) * flux_l;
                cz->env_flux_r[b] = (flux_r > cz->env_flux_r[b] ? cz->attack_coeff : cz->release_coeff) * cz->env_flux_r[b]
                                  + (1.0f - (flux_r > cz->env_flux_r[b] ? cz->attack_coeff : cz->release_coeff)) * flux_r;
                float flux_r_l = (cz->env_flux_l[b] > 1e-5f) ? (cz->global_flux_l / cz->env_flux_l[b]) : 1.0f;
                float flux_r_r = (cz->env_flux_r[b] > 1e-5f) ? (cz->global_flux_r / cz->env_flux_r[b]) : 1.0f;

                // Clamp individual ratios to prevent detector divergence
                crest_r_l = std::clamp(crest_r_l, 0.2f, 5.0f);
                crest_r_r = std::clamp(crest_r_r, 0.2f, 5.0f);
                kurt_r_l = std::clamp(kurt_r_l, 0.2f, 5.0f);
                kurt_r_r = std::clamp(kurt_r_r, 0.2f, 5.0f);
                flux_r_l = std::clamp(flux_r_l, 0.2f, 5.0f);
                flux_r_r = std::clamp(flux_r_r, 0.2f, 5.0f);

                float adapt_mult_l = std::cbrtf(crest_r_l * kurt_r_l * flux_r_l);
                float adapt_mult_r = std::cbrtf(crest_r_r * kurt_r_r * flux_r_r);

                // Safe clamping on overall adaptive multiplier [0.5f, 1.5f]
                // Eliminates sudden 0.5s transient volume flare-ups / pumping
                adapt_mult_l = std::clamp(adapt_mult_l, 0.5f, 1.5f);
                adapt_mult_r = std::clamp(adapt_mult_r, 0.5f, 1.5f);

                intensity_l *= adapt_mult_l;
                intensity_r *= adapt_mult_r;
            }

            for (uint32_t m = 0; m < bs; ++m) {
                band_l[m] -= intensity_l * d2_l[m];
                band_r[m] -= intensity_r * d2_r[m];
            }
        }

        cz->band_prev_l[b] = band_l[bs - 1];
        cz->band_prev_r[b] = band_r[bs - 1];
    }

    // Reconstruct output signal by summing unmuted bands
    std::fill_n(out_l, bs, 0.0f);
    std::fill_n(out_r, bs, 0.0f);

    for (uint32_t b = 0; b < NBANDS; ++b) {
        if (!mutes[b]) {
            const float *bl = cz->band_buf_l[b].data();
            const float *br = cz->band_buf_r[b].data();
            for (uint32_t m = 0; m < bs; ++m) {
                out_l[m] += bl[m];
                out_r[m] += br[m];
            }
        }
    }

    if (cz->first_buffer) cz->first_buffer = false;
}

void crystalizer_zita_process(CrystalizerZita *cz,
                              const float *in_l, const float *in_r,
                              float *out_l, float *out_r,
                              uint32_t n_samples,
                              const float *intensities_db,
                              const bool *bypasses,
                              const bool *mutes,
                              bool adaptive) {
    if (!cz || n_samples == 0) {
        if (in_l != out_l) std::memcpy(out_l, in_l, n_samples * sizeof(float));
        if (in_r != out_r) std::memcpy(out_r, in_r, n_samples * sizeof(float));
        return;
    }

    std::scoped_lock lock(cz->lock);

    if (!cz->ready || (cz->blocksize != n_samples && (n_samples & (n_samples - 1)) == 0 && n_samples >= 64 && n_samples <= 2048)) {
        cz->blocksize = n_samples;
        cz_init_filters(cz);
    }

    uint32_t bs = cz->blocksize;
    if (n_samples == bs) {
        cz_process_block(cz, in_l, in_r, out_l, out_r, bs, intensities_db, bypasses, mutes, adaptive);
    } else {
        float *tmp_in_l = cz->scratch_in_l.data();
        float *tmp_in_r = cz->scratch_in_r.data();
        float *tmp_out_l = cz->scratch_out_l.data();
        float *tmp_out_r = cz->scratch_out_r.data();
        for (uint32_t offset = 0; offset < n_samples; offset += bs) {
            uint32_t chunk = n_samples - offset;
            if (chunk >= bs) {
                cz_process_block(cz, in_l + offset, in_r + offset, out_l + offset, out_r + offset,
                                 bs, intensities_db, bypasses, mutes, adaptive);
            } else {
                std::memcpy(tmp_in_l, in_l + offset, chunk * sizeof(float));
                std::memcpy(tmp_in_r, in_r + offset, chunk * sizeof(float));
                std::fill(cz->scratch_in_l.begin() + chunk, cz->scratch_in_l.end(), 0.0f);
                std::fill(cz->scratch_in_r.begin() + chunk, cz->scratch_in_r.end(), 0.0f);
                cz_process_block(cz, tmp_in_l, tmp_in_r, tmp_out_l, tmp_out_r,
                                 bs, intensities_db, bypasses, mutes, adaptive);
                std::memcpy(out_l + offset, tmp_out_l, chunk * sizeof(float));
                std::memcpy(out_r + offset, tmp_out_r, chunk * sizeof(float));
            }
        }
    }
}
