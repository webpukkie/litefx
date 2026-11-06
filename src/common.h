#ifndef LITEFX_COMMON_H
#define LITEFX_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
#include <atomic>
using std::atomic_uint_fast64_t;
using std::atomic_uint;
using std::atomic_bool;
#ifndef _Atomic
#define _Atomic(T) std::atomic<T>
#endif
#else
#include <stdatomic.h>
#endif

#define LITEFX_MAGIC            0x4C465833u /* "LFX3" */
#define LITEFX_VERSION          3u

#define LITEFX_MAX_EQ_BANDS     32
#define LITEFX_CRYSTAL_BANDS    13
#define LITEFX_CRYSTAL_SPLITS   12
#define LITEFX_SPECTRUM_BINS    64

#define LITEFX_MAX_SINKS        16
#define LITEFX_MAX_SOURCES      16
#define LITEFX_MAX_STREAMS      32
#define LITEFX_MAX_BLOCKLIST    16
#define LITEFX_MAX_DEV_PROFILES 16
#define LITEFX_NAME_LEN         96
#define LITEFX_DESC_LEN         128

typedef enum {
    LITEFX_LANG_EN = 0,
    LITEFX_LANG_UA = 1
} LiteFxLang;

typedef enum {
    EQ_BAND_PEAK = 0,
    EQ_BAND_LOW_SHELF = 1,
    EQ_BAND_HIGH_SHELF = 2,
    EQ_BAND_LOW_PASS = 3,
    EQ_BAND_HIGH_PASS = 4,
    EQ_BAND_NOTCH = 5,
    EQ_BAND_TYPE_COUNT = 6
} EqBandType;

typedef struct {
    bool     enabled;
    uint8_t  type;     /* EqBandType */
    float    freq;     /* 20.0 .. 20000.0 Hz */
    float    gain_db;  /* -24.0 .. +24.0 dB */
    float    q;        /* 0.1 .. 20.0 */
} EqBandConfig;

typedef struct {
    float intensity_db; /* -36.0 .. +15.0 dB (EasyEffects compatible scale) */
    bool  bypass;
    bool  mute;
} CrystalBandConfig;

typedef struct {
    char device_name[LITEFX_NAME_LEN];
    char preset_name[64];
} LiteFxDevProfileRule;

typedef struct {
    /* === Output Pipeline: Speakers / Headphones === */
    bool              sink_bypass;
    float             sink_preamp_db;        /* -24.0 .. +24.0 dB */
    float             sink_postgain_db;      /* -24.0 .. +24.0 dB */
    bool              sink_auto_preamp;
    bool              sink_soft_limiter;

    bool              sink_eq_bypass;
    bool              sink_eq_split_channels;
    uint32_t          sink_eq_num_bands;     /* 1 .. LITEFX_MAX_EQ_BANDS */
    EqBandConfig      sink_eq_left[LITEFX_MAX_EQ_BANDS];
    EqBandConfig      sink_eq_right[LITEFX_MAX_EQ_BANDS];

    bool              sink_crystal_bypass;
    bool              sink_crystal_adaptive;
    CrystalBandConfig sink_crystal_bands[LITEFX_CRYSTAL_BANDS];

    char              target_sink[LITEFX_NAME_LEN]; /* "auto" or hardware sink name */
    bool              auto_route_sink_apps;
    uint32_t          sink_blocklist_count;
    char              sink_blocklist[LITEFX_MAX_BLOCKLIST][64];

    /* === Input Pipeline: Microphone === */
    bool              source_bypass;
    float             source_preamp_db;         /* -24.0 .. +24.0 dB input gain */
    float             source_postgain_db;       /* -24.0 .. +24.0 dB output gain */
    bool              source_noise_gate;
    float             source_gate_threshold_db; /* -60.0 .. -15.0 dB */
    bool              source_soft_limiter;
    bool              source_compressor;
    float             source_comp_amount;       /* 0.0 .. 1.0 */
    bool              source_denoiser;
    float             source_denoise_strength;  /* 0.0 .. 1.0 */

    bool              source_eq_bypass;
    uint32_t          source_eq_num_bands;      /* 1 .. LITEFX_MAX_EQ_BANDS */
    EqBandConfig      source_eq[LITEFX_MAX_EQ_BANDS];

    bool              source_crystal_bypass;
    bool              source_crystal_adaptive;
    CrystalBandConfig source_crystal_bands[LITEFX_CRYSTAL_BANDS];

    char              target_source[LITEFX_NAME_LEN]; /* "auto" or hardware source name */
    bool              auto_route_source_apps;

    /* === UI & Automation Preferences === */
    bool                 spectrum_enabled;
    uint8_t              ui_lang; /* 0 = English (default), 1 = Ukrainian */
    uint32_t             dev_profile_count;
    LiteFxDevProfileRule dev_profiles[LITEFX_MAX_DEV_PROFILES];
} LiteFxConfig;

typedef struct {
    uint32_t id;
    uint32_t serial;
    char     name[LITEFX_NAME_LEN];
    char     description[LITEFX_DESC_LEN];
    bool     is_default;
    bool     is_active_target;
} LiteFxDeviceInfo;

typedef struct {
    uint32_t id;
    char     app_name[64];
    char     binary[64];
    char     media_name[96];
    bool     routed_to_litefx;
    bool     blocklisted;
} LiteFxStreamInfo;

/* Shared memory mapped by GUI/CLI and updated by daemon */
typedef struct {
    uint32_t             magic;
    uint32_t             version;
    int32_t              daemon_pid;
    atomic_uint_fast64_t gui_heartbeat_ns;
    atomic_uint          config_seq;

    /* Output pipeline telemetry */
    _Atomic(float)       sink_peak_in_l;
    _Atomic(float)       sink_peak_in_r;
    _Atomic(float)       sink_peak_out_l;
    _Atomic(float)       sink_peak_out_r;
    _Atomic(float)       sink_spectrum_db[LITEFX_SPECTRUM_BINS];
    atomic_uint          sink_dsp_proc_ns;
    atomic_bool          sink_dsp_streaming;

    /* Input pipeline telemetry */
    _Atomic(float)       source_peak_in_l;
    _Atomic(float)       source_peak_in_r;
    _Atomic(float)       source_peak_out_l;
    _Atomic(float)       source_peak_out_r;
    _Atomic(float)       source_spectrum_db[LITEFX_SPECTRUM_BINS];
    atomic_uint          source_dsp_proc_ns;
    atomic_bool          source_dsp_streaming;

    atomic_uint          sample_rate;
    atomic_uint          quantum_frames;

    /* Current configuration */
    LiteFxConfig         config;

    /* Available hardware devices */
    uint32_t             sink_count;
    LiteFxDeviceInfo     sinks[LITEFX_MAX_SINKS];
    char                 resolved_sink_desc[LITEFX_DESC_LEN];

    uint32_t             source_count;
    LiteFxDeviceInfo     sources[LITEFX_MAX_SOURCES];
    char                 resolved_source_desc[LITEFX_DESC_LEN];

    /* Active streams */
    uint32_t             stream_count;
    LiteFxStreamInfo     streams[LITEFX_MAX_STREAMS];
} LiteFxShm;

typedef enum {
    CMD_SET_CONFIG = 1,         /* Apply full LiteFxConfig */
    CMD_SAVE_CURRENT = 2,       /* Save active config to ~/.config/litefx/current.conf */
    CMD_RELOAD_CURRENT = 3,     /* Reload ~/.config/litefx/current.conf */
    CMD_IMPORT_EASYEFFECTS = 4, /* Import ~/.config/easyeffects/db/ */
    CMD_TOGGLE_SINK_BYPASS = 5, /* Toggle sink_bypass */
    CMD_TOGGLE_SOURCE_BYPASS = 6,/* Toggle source_bypass */
    CMD_STOP_DAEMON = 7         /* Restore default sinks/sources and exit cleanly */
} LiteFxCmdType;

typedef struct {
    uint32_t     magic;
    uint32_t     cmd;        /* LiteFxCmdType */
    int32_t      arg_int;
    LiteFxConfig config;     /* Used when cmd == CMD_SET_CONFIG */
} LiteFxCmdPacket;

typedef struct {
    int32_t ok;
    char    message[128];
} LiteFxReplyPacket;

/* Crystalizer 13-band metadata (exact EasyEffects band layout) */
static const float LITEFX_CRYSTAL_CROSSOVERS[LITEFX_CRYSTAL_SPLITS] = {
    219.3f, 485.1f, 867.5f, 1395.8f, 2102.9f, 3026.5f,
    4209.6f, 5701.1f, 7556.6f, 9839.0f, 12619.1f, 15976.4f
};

static const float LITEFX_CRYSTAL_CENTERS[LITEFX_CRYSTAL_BANDS] = {
    120.0f, 352.0f, 676.0f, 1132.0f, 1749.0f, 2565.0f, 3618.0f,
    4955.0f, 6629.0f, 8698.0f, 11229.0f, 14298.0f, 17988.0f
};

static const char * const LITEFX_CRYSTAL_LABELS[LITEFX_CRYSTAL_BANDS] = {
    "120", "352", "676", "1.1k", "1.7k", "2.6k", "3.6k",
    "5.0k", "6.6k", "8.7k", "11.2k", "14.3k", "18.0k"
};

/* Standard ISO 10-band centers */
static const float LITEFX_ISO_10_FREQS[10] = {
    32.0f, 64.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

/* Standard ISO 15-band centers */
static const float LITEFX_ISO_15_FREQS[15] = {
    25.0f, 40.0f, 63.0f, 100.0f, 160.0f,
    250.0f, 400.0f, 630.0f, 1000.0f, 1600.0f,
    2500.0f, 4000.0f, 6300.0f, 10000.0f, 16000.0f
};

/* Standard Voice Microphone 8-band centers */
static const float LITEFX_VOICE_8_FREQS[8] = {
    80.0f, 160.0f, 320.0f, 640.0f, 1250.0f, 2500.0f, 5000.0f, 10000.0f
};

#endif /* LITEFX_COMMON_H */
