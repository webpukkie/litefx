#ifndef LITEFX_DSP_H
#define LITEFX_DSP_H

#include "common.h"
#include <stddef.h>

#define LITEFX_FFT_SIZE 512

typedef struct {
    float b0, b1, b2;
    float a1, a2;
} BiquadCoeffs;

typedef struct {
    double z1, z2;
} BiquadStateMono;

typedef struct CrystalizerZita CrystalizerZita;

/* Lock-free immutable coefficient snapshot published to RT audio thread */
typedef struct {
    uint32_t sample_rate;
    bool     bypass;
    bool     soft_limiter;
    float    target_preamp;
    float    target_postgain;

    /* Microphone Voice Processing (Noise Gate, Denoiser, Compressor) */
    bool         noise_gate;
    float        gate_thresh;
    bool         voice_denoiser;
    float        denoise_strength;
    BiquadCoeffs denoise_hpf;
    bool         voice_compressor;
    float        comp_amount;

    /* Equalizer active biquads */
    bool         eq_active;
    bool         eq_split;
    uint32_t     eq_mask_l;
    BiquadCoeffs eq_l[LITEFX_MAX_EQ_BANDS];
    uint32_t     eq_mask_r;
    BiquadCoeffs eq_r[LITEFX_MAX_EQ_BANDS];

    /* 13-band Linear-Phase FIR Crystalizer */
    bool         crystal_active;
    bool         crystal_adaptive;
    float        crystal_db[LITEFX_CRYSTAL_BANDS];
    bool         crystal_bypass_band[LITEFX_CRYSTAL_BANDS];
    bool         crystal_mute_band[LITEFX_CRYSTAL_BANDS];
} DspCoeffsSlot;

typedef struct {
    DspCoeffsSlot slots[4];
    atomic_uint   active_gen;
    atomic_bool   reset_requested;

    CrystalizerZita *cz_engine;

    uint32_t         rt_sample_rate;
    uint32_t         rt_prev_mask_l;
    uint32_t         rt_prev_mask_r;
    float            cur_preamp;
    float            cur_postgain;
    float            cur_gate_gain;
    float            gate_detector;
    float            denoise_floor;
    float            denoise_env;
    float            comp_env;
    float            cur_comp_gain;
    bool             gain_initialized;

    BiquadStateMono  denoise_hpf_l;
    BiquadStateMono  denoise_hpf_r;
    BiquadStateMono  eq_state_l[LITEFX_MAX_EQ_BANDS];
    BiquadStateMono  eq_state_r[LITEFX_MAX_EQ_BANDS];

    /* Real-time 512-point FFT Spectrum Analyzer state (used only when GUI is active) */
    float            fft_ring[LITEFX_FFT_SIZE];
    float            fft_window[LITEFX_FFT_SIZE];
    uint32_t         fft_write_pos;
    uint32_t         fft_hop_accum;
    float            spectrum_db[LITEFX_SPECTRUM_BINS];
} DspEngine;

void dsp_engine_init(DspEngine *eng);
void dsp_engine_destroy(DspEngine *eng);
void dsp_engine_reset_state(DspEngine *eng);

/* Populate slot for Speakers/Headphones output pipeline */
void dsp_engine_update_sink(DspEngine *eng, const LiteFxConfig *cfg, uint32_t sample_rate);

/* Populate slot for Microphone input pipeline */
void dsp_engine_update_source(DspEngine *eng, const LiteFxConfig *cfg, uint32_t sample_rate);

/* Process stereo block (RT safe, zero allocations, zero locks) */
void dsp_engine_process(DspEngine *eng,
                        const float *in_l, const float *in_r,
                        float *out_l, float *out_r,
                        uint32_t n_samples,
                        bool measure_peaks,
                        float *peak_in_l, float *peak_in_r,
                        float *peak_out_l, float *peak_out_r);

void dsp_engine_process_ex(DspEngine *eng,
                           const float *in_l, const float *in_r,
                           float *out_l, float *out_r,
                           uint32_t n_samples,
                           bool measure_peaks,
                           float *peak_in_l, float *peak_in_r,
                           float *peak_out_l, float *peak_out_r,
                           bool compute_spectrum,
                           _Atomic(float) *out_spectrum_db);

bool dsp_compute_eq_biquad(const EqBandConfig *band, uint32_t sample_rate, BiquadCoeffs *out);
float dsp_eval_eq_magnitude_db(const EqBandConfig *bands, uint32_t num_bands,
                               uint32_t sample_rate, float freq_hz);
float dsp_compute_auto_preamp_db(const EqBandConfig *bands_l, const EqBandConfig *bands_r,
                                 uint32_t num_bands, bool split, uint32_t sample_rate);

#endif /* LITEFX_DSP_H */
