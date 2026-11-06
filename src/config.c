#define _GNU_SOURCE
#include "config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

uint64_t litefx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static locale_t get_c_locale(void) {
    static locale_t loc = (locale_t)0;
    if (!loc) {
        loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    }
    return loc;
}

static float parse_float_c(const char *s) {
    char buf[64];
    size_t i = 0;
    while (*s && isspace((unsigned char)*s)) s++;
    for (; s[i] && i < sizeof(buf) - 1; ++i) {
        buf[i] = (s[i] == ',') ? '.' : s[i];
    }
    buf[i] = '\0';
    locale_t c_loc = get_c_locale();
    return c_loc ? strtof_l(buf, NULL, c_loc) : strtof(buf, NULL);
}

static bool parse_bool_c(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return (s[0] == '1' || strncasecmp(s, "true", 4) == 0 ||
            strncasecmp(s, "yes", 3) == 0 || strncasecmp(s, "on", 2) == 0);
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static void mkdir_p(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

void litefx_get_sock_path(char *buf, size_t len) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) {
        snprintf(buf, len, "%s/litefx.sock", xdg);
    } else {
        snprintf(buf, len, "/tmp/litefx-%u.sock", (unsigned)getuid());
    }
}

void litefx_get_shm_path(char *buf, size_t len) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) {
        snprintf(buf, len, "%s/litefx.shm", xdg);
    } else {
        snprintf(buf, len, "/tmp/litefx-%u.shm", (unsigned)getuid());
    }
}

void litefx_get_config_dir(char *buf, size_t len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(buf, len, "%s/litefx", xdg);
    } else {
        const char *home = getenv("HOME");
        snprintf(buf, len, "%s/.config/litefx", home ? home : "/tmp");
    }
}

void litefx_get_default_config_path(char *buf, size_t len) {
    char dir[384];
    litefx_get_config_dir(dir, sizeof(dir));
    snprintf(buf, len, "%s/current.conf", dir);
}

void litefx_get_preset_path(const char *preset_name, char *buf, size_t len) {
    char dir[384];
    char safe[64];
    size_t j = 0;
    for (size_t i = 0; preset_name[i] && j < sizeof(safe) - 1; ++i) {
        unsigned char ch = (unsigned char)preset_name[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == ' ' || ch >= 128) {
            safe[j++] = (char)ch;
        }
    }
    safe[j] = '\0';
    if (j == 0) strcpy(safe, "preset");

    litefx_get_config_dir(dir, sizeof(dir));
    snprintf(buf, len, "%s/presets/%s.conf", dir, safe);
}

void litefx_config_resize_sink_eq_bands(LiteFxConfig *cfg, uint32_t new_count) {
    if (new_count < 1) new_count = 1;
    if (new_count > LITEFX_MAX_EQ_BANDS) new_count = LITEFX_MAX_EQ_BANDS;
    cfg->sink_eq_num_bands = new_count;

    if (new_count == 10) {
        for (uint32_t i = 0; i < 10; ++i) {
            cfg->sink_eq_left[i].enabled = true;
            cfg->sink_eq_left[i].type = EQ_BAND_PEAK;
            cfg->sink_eq_left[i].freq = LITEFX_ISO_10_FREQS[i];
            cfg->sink_eq_left[i].q = 1.414f;
            cfg->sink_eq_right[i] = cfg->sink_eq_left[i];
        }
        return;
    }

    if (new_count == 15) {
        for (uint32_t i = 0; i < 15; ++i) {
            cfg->sink_eq_left[i].enabled = true;
            cfg->sink_eq_left[i].type = EQ_BAND_PEAK;
            cfg->sink_eq_left[i].freq = LITEFX_ISO_15_FREQS[i];
            cfg->sink_eq_left[i].q = 1.7f;
            cfg->sink_eq_right[i] = cfg->sink_eq_left[i];
        }
        return;
    }

    float f_min = 28.0f, f_max = 16500.0f;
    float q_auto = (new_count <= 6) ? 1.0f : (0.18f * (float)new_count);
    if (q_auto > 4.3f) q_auto = 4.3f;

    for (uint32_t i = 0; i < new_count; ++i) {
        float t = (new_count > 1) ? ((float)i / (float)(new_count - 1)) : 0.5f;
        float f = roundf(f_min * powf(f_max / f_min, t));
        cfg->sink_eq_left[i].enabled = true;
        cfg->sink_eq_left[i].type = EQ_BAND_PEAK;
        cfg->sink_eq_left[i].freq = f;
        cfg->sink_eq_left[i].q = q_auto;
        cfg->sink_eq_right[i] = cfg->sink_eq_left[i];
    }
}

void litefx_config_resize_source_eq_bands(LiteFxConfig *cfg, uint32_t new_count) {
    if (new_count < 1) new_count = 1;
    if (new_count > LITEFX_MAX_EQ_BANDS) new_count = LITEFX_MAX_EQ_BANDS;
    cfg->source_eq_num_bands = new_count;

    if (new_count == 8) {
        for (uint32_t i = 0; i < 8; ++i) {
            cfg->source_eq[i].enabled = true;
            cfg->source_eq[i].type = (i == 0) ? EQ_BAND_HIGH_PASS : EQ_BAND_PEAK;
            cfg->source_eq[i].freq = LITEFX_VOICE_8_FREQS[i];
            cfg->source_eq[i].q = (i == 0) ? 0.707f : 1.414f;
        }
        return;
    }

    float f_min = 60.0f, f_max = 14000.0f;
    float q_auto = (new_count <= 6) ? 1.0f : (0.22f * (float)new_count);
    if (q_auto > 4.0f) q_auto = 4.0f;

    for (uint32_t i = 0; i < new_count; ++i) {
        float t = (new_count > 1) ? ((float)i / (float)(new_count - 1)) : 0.5f;
        float f = roundf(f_min * powf(f_max / f_min, t));
        cfg->source_eq[i].enabled = true;
        cfg->source_eq[i].type = (i == 0) ? EQ_BAND_HIGH_PASS : EQ_BAND_PEAK;
        cfg->source_eq[i].freq = f;
        cfg->source_eq[i].q = q_auto;
    }
}

void litefx_config_set_defaults(LiteFxConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    /* Speakers / Headphones Defaults */
    cfg->sink_bypass = false;
    cfg->sink_preamp_db = 0.0f;
    cfg->sink_postgain_db = 0.0f;
    cfg->sink_auto_preamp = false;
    cfg->sink_soft_limiter = false;

    cfg->sink_eq_bypass = false;
    cfg->sink_eq_split_channels = false;
    cfg->sink_eq_num_bands = 10;
    for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
        float f = (i < 10) ? LITEFX_ISO_10_FREQS[i] : 1000.0f;
        cfg->sink_eq_left[i].enabled = true;
        cfg->sink_eq_left[i].type = EQ_BAND_PEAK;
        cfg->sink_eq_left[i].freq = f;
        cfg->sink_eq_left[i].gain_db = 0.0f;
        cfg->sink_eq_left[i].q = 1.414f;
        cfg->sink_eq_right[i] = cfg->sink_eq_left[i];
    }

    cfg->sink_crystal_bypass = false;
    cfg->sink_crystal_adaptive = true;
    static const float default_ee_curve[LITEFX_CRYSTAL_BANDS] = {
        -2.0f, -2.0f, -4.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -5.0f, -5.0f, -2.0f
    };
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        cfg->sink_crystal_bands[b].intensity_db = default_ee_curve[b];
        cfg->sink_crystal_bands[b].bypass = false;
        cfg->sink_crystal_bands[b].mute = false;
    }

    strcpy(cfg->target_sink, "auto");
    cfg->auto_route_sink_apps = true;
    cfg->sink_blocklist_count = 0;

    /* Microphone Defaults */
    cfg->source_bypass = false;
    cfg->source_preamp_db = 0.0f;
    cfg->source_postgain_db = 0.0f;
    cfg->source_noise_gate = false;
    cfg->source_gate_threshold_db = -45.0f;
    cfg->source_soft_limiter = false;
    cfg->source_compressor = false;
    cfg->source_comp_amount = 0.5f;
    cfg->source_denoiser = false;
    cfg->source_denoise_strength = 0.5f;

    cfg->source_eq_bypass = false;
    cfg->source_eq_num_bands = 8;
    for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
        float f = (i < 8) ? LITEFX_VOICE_8_FREQS[i] : 1000.0f;
        cfg->source_eq[i].enabled = true;
        cfg->source_eq[i].type = (i == 0) ? EQ_BAND_HIGH_PASS : EQ_BAND_PEAK;
        cfg->source_eq[i].freq = f;
        cfg->source_eq[i].gain_db = (i == 0) ? 0.0f : (i == 5 ? 2.0f : 0.0f);
        cfg->source_eq[i].q = (i == 0) ? 0.707f : 1.414f;
    }

    cfg->source_crystal_bypass = false;
    cfg->source_crystal_adaptive = true;
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        cfg->source_crystal_bands[b].intensity_db = default_ee_curve[b];
        cfg->source_crystal_bands[b].bypass = false;
        cfg->source_crystal_bands[b].mute = false;
    }

    strcpy(cfg->target_source, "auto");
    cfg->auto_route_source_apps = true;

    cfg->spectrum_enabled = true;
    cfg->ui_lang = LITEFX_LANG_EN;
    cfg->dev_profile_count = 0;
}

void litefx_config_apply_preset_only(LiteFxConfig *dst, const LiteFxConfig *src) {
    char saved_sink[LITEFX_NAME_LEN];
    char saved_source[LITEFX_NAME_LEN];
    bool saved_route_sink = dst->auto_route_sink_apps;
    bool saved_route_source = dst->auto_route_source_apps;
    uint32_t saved_bl_count = dst->sink_blocklist_count;
    char saved_bl[LITEFX_MAX_BLOCKLIST][64];
    bool saved_spec = dst->spectrum_enabled;
    uint8_t saved_lang = dst->ui_lang;
    uint32_t saved_dp_count = dst->dev_profile_count;
    LiteFxDevProfileRule saved_dp[LITEFX_MAX_DEV_PROFILES];

    memcpy(saved_sink, dst->target_sink, sizeof(saved_sink));
    memcpy(saved_source, dst->target_source, sizeof(saved_source));
    memcpy(saved_bl, dst->sink_blocklist, sizeof(saved_bl));
    memcpy(saved_dp, dst->dev_profiles, sizeof(saved_dp));

    *dst = *src;

    memcpy(dst->target_sink, saved_sink, sizeof(saved_sink));
    memcpy(dst->target_source, saved_source, sizeof(saved_source));
    dst->auto_route_sink_apps = saved_route_sink;
    dst->auto_route_source_apps = saved_route_source;
    dst->sink_blocklist_count = saved_bl_count;
    memcpy(dst->sink_blocklist, saved_bl, sizeof(saved_bl));
    dst->spectrum_enabled = saved_spec;
    dst->ui_lang = saved_lang;
    dst->dev_profile_count = saved_dp_count;
    memcpy(dst->dev_profiles, saved_dp, sizeof(saved_dp));
}

bool litefx_config_save(const LiteFxConfig *cfg, const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir);
    }

    FILE *fp = fopen(path, "w");
    if (!fp) return false;

    locale_t c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t old_loc = c_loc ? uselocale(c_loc) : (locale_t)0;

    fprintf(fp, "# LiteFX Configuration v2\n");

    /* Speakers / Output */
    fprintf(fp, "\n[Output]\n");
    fprintf(fp, "sink_bypass=%d\n", cfg->sink_bypass ? 1 : 0);
    fprintf(fp, "sink_preamp_db=%.3f\n", cfg->sink_preamp_db);
    fprintf(fp, "sink_postgain_db=%.3f\n", cfg->sink_postgain_db);
    fprintf(fp, "sink_auto_preamp=%d\n", cfg->sink_auto_preamp ? 1 : 0);
    fprintf(fp, "sink_soft_limiter=%d\n", cfg->sink_soft_limiter ? 1 : 0);
    fprintf(fp, "target_sink=%s\n", cfg->target_sink[0] ? cfg->target_sink : "auto");
    fprintf(fp, "auto_route_sink_apps=%d\n", cfg->auto_route_sink_apps ? 1 : 0);
    fprintf(fp, "sink_blocklist=");
    for (uint32_t i = 0; i < cfg->sink_blocklist_count; ++i) {
        fprintf(fp, "%s%s", (i > 0) ? "," : "", cfg->sink_blocklist[i]);
    }
    fprintf(fp, "\n");

    fprintf(fp, "sink_eq_bypass=%d\n", cfg->sink_eq_bypass ? 1 : 0);
    fprintf(fp, "sink_eq_split_channels=%d\n", cfg->sink_eq_split_channels ? 1 : 0);
    fprintf(fp, "sink_eq_num_bands=%u\n", cfg->sink_eq_num_bands);
    for (uint32_t i = 0; i < cfg->sink_eq_num_bands && i < LITEFX_MAX_EQ_BANDS; ++i) {
        const EqBandConfig *l = &cfg->sink_eq_left[i];
        fprintf(fp, "sink_eq_l_%u=%d,%u,%.2f,%.3f,%.4f\n",
                i, l->enabled ? 1 : 0, (unsigned)l->type, l->freq, l->gain_db, l->q);
        if (cfg->sink_eq_split_channels) {
            const EqBandConfig *r = &cfg->sink_eq_right[i];
            fprintf(fp, "sink_eq_r_%u=%d,%u,%.2f,%.3f,%.4f\n",
                    i, r->enabled ? 1 : 0, (unsigned)r->type, r->freq, r->gain_db, r->q);
        }
    }

    fprintf(fp, "sink_crystal_bypass=%d\n", cfg->sink_crystal_bypass ? 1 : 0);
    fprintf(fp, "sink_crystal_adaptive=%d\n", cfg->sink_crystal_adaptive ? 1 : 0);
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        const CrystalBandConfig *cb = &cfg->sink_crystal_bands[b];
        fprintf(fp, "sink_crystal_%d=%.3f,%d,%d\n",
                b, cb->intensity_db, cb->bypass ? 1 : 0, cb->mute ? 1 : 0);
    }

    /* Microphone / Input */
    fprintf(fp, "\n[Input]\n");
    fprintf(fp, "source_bypass=%d\n", cfg->source_bypass ? 1 : 0);
    fprintf(fp, "source_preamp_db=%.3f\n", cfg->source_preamp_db);
    fprintf(fp, "source_postgain_db=%.3f\n", cfg->source_postgain_db);
    fprintf(fp, "source_noise_gate=%d\n", cfg->source_noise_gate ? 1 : 0);
    fprintf(fp, "source_gate_threshold_db=%.3f\n", cfg->source_gate_threshold_db);
    fprintf(fp, "source_soft_limiter=%d\n", cfg->source_soft_limiter ? 1 : 0);
    fprintf(fp, "source_compressor=%d\n", cfg->source_compressor ? 1 : 0);
    fprintf(fp, "source_comp_amount=%.3f\n", cfg->source_comp_amount);
    fprintf(fp, "source_denoiser=%d\n", cfg->source_denoiser ? 1 : 0);
    fprintf(fp, "source_denoise_strength=%.3f\n", cfg->source_denoise_strength);
    fprintf(fp, "target_source=%s\n", cfg->target_source[0] ? cfg->target_source : "auto");
    fprintf(fp, "auto_route_source_apps=%d\n", cfg->auto_route_source_apps ? 1 : 0);

    fprintf(fp, "source_eq_bypass=%d\n", cfg->source_eq_bypass ? 1 : 0);
    fprintf(fp, "source_eq_num_bands=%u\n", cfg->source_eq_num_bands);
    for (uint32_t i = 0; i < cfg->source_eq_num_bands && i < LITEFX_MAX_EQ_BANDS; ++i) {
        const EqBandConfig *b = &cfg->source_eq[i];
        fprintf(fp, "source_eq_%u=%d,%u,%.2f,%.3f,%.4f\n",
                i, b->enabled ? 1 : 0, (unsigned)b->type, b->freq, b->gain_db, b->q);
    }

    fprintf(fp, "source_crystal_bypass=%d\n", cfg->source_crystal_bypass ? 1 : 0);
    fprintf(fp, "source_crystal_adaptive=%d\n", cfg->source_crystal_adaptive ? 1 : 0);
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        const CrystalBandConfig *cb = &cfg->source_crystal_bands[b];
        fprintf(fp, "source_crystal_%d=%.3f,%d,%d\n",
                b, cb->intensity_db, cb->bypass ? 1 : 0, cb->mute ? 1 : 0);
    }

    fprintf(fp, "\n[Preferences]\n");
    fprintf(fp, "spectrum_enabled=%d\n", cfg->spectrum_enabled ? 1 : 0);
    fprintf(fp, "ui_lang=%u\n", (unsigned)cfg->ui_lang);
    fprintf(fp, "dev_profile_count=%u\n", cfg->dev_profile_count);
    for (uint32_t i = 0; i < cfg->dev_profile_count && i < LITEFX_MAX_DEV_PROFILES; ++i) {
        fprintf(fp, "dev_profile_%u=%s|%s\n",
                i, cfg->dev_profiles[i].device_name, cfg->dev_profiles[i].preset_name);
    }

    if (c_loc) {
        uselocale(old_loc);
        freelocale(c_loc);
    }

    fclose(fp);
    return true;
}

static void parse_eq_band_tuple(const char *val, EqBandConfig *b) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", val);
    char *save = NULL;
    char *t0 = strtok_r(tmp, ",", &save);
    char *t1 = strtok_r(NULL, ",", &save);
    char *t2 = strtok_r(NULL, ",", &save);
    char *t3 = strtok_r(NULL, ",", &save);
    char *t4 = strtok_r(NULL, ",", &save);
    if (t0) b->enabled = (atoi(t0) != 0);
    if (t1) b->type = (uint8_t)(atoi(t1) % EQ_BAND_TYPE_COUNT);
    if (t2) b->freq = parse_float_c(t2);
    if (t3) b->gain_db = parse_float_c(t3);
    if (t4) b->q = parse_float_c(t4);
}

static void parse_blocklist_csv(LiteFxConfig *cfg, const char *csv) {
    cfg->sink_blocklist_count = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", csv);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save);
         tok && cfg->sink_blocklist_count < LITEFX_MAX_BLOCKLIST;
         tok = strtok_r(NULL, ",", &save)) {
        char *item = trim_ws(tok);
        if (item[0]) {
            snprintf(cfg->sink_blocklist[cfg->sink_blocklist_count++], 64, "%s", item);
        }
    }
}

bool litefx_config_load(LiteFxConfig *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    litefx_config_set_defaults(cfg);

    char line[512];
    bool saw_sink_eq_r = false;
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_ws(line);
        if (!s[0] || s[0] == '#' || s[0] == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_ws(s);
        char *val = trim_ws(eq + 1);

        /* Legacy or v2 keys for Output */
        if (strcmp(key, "sink_bypass") == 0 || strcmp(key, "global_bypass") == 0)
            cfg->sink_bypass = parse_bool_c(val);
        else if (strcmp(key, "sink_preamp_db") == 0 || strcmp(key, "preamp_db") == 0)
            cfg->sink_preamp_db = parse_float_c(val);
        else if (strcmp(key, "sink_postgain_db") == 0 || strcmp(key, "postgain_db") == 0)
            cfg->sink_postgain_db = parse_float_c(val);
        else if (strcmp(key, "sink_auto_preamp") == 0 || strcmp(key, "auto_preamp") == 0)
            cfg->sink_auto_preamp = parse_bool_c(val);
        else if (strcmp(key, "sink_soft_limiter") == 0 || strcmp(key, "soft_limiter") == 0)
            cfg->sink_soft_limiter = parse_bool_c(val);
        else if (strcmp(key, "target_sink") == 0)
            snprintf(cfg->target_sink, sizeof(cfg->target_sink), "%s", val);
        else if (strcmp(key, "auto_route_sink_apps") == 0 || strcmp(key, "auto_route_apps") == 0)
            cfg->auto_route_sink_apps = parse_bool_c(val);
        else if (strcmp(key, "sink_blocklist") == 0 || strcmp(key, "blocklist") == 0)
            parse_blocklist_csv(cfg, val);
        else if (strcmp(key, "sink_eq_bypass") == 0 || strcmp(key, "eq_bypass") == 0)
            cfg->sink_eq_bypass = parse_bool_c(val);
        else if (strcmp(key, "sink_eq_split_channels") == 0 || strcmp(key, "eq_split_channels") == 0)
            cfg->sink_eq_split_channels = parse_bool_c(val);
        else if (strcmp(key, "sink_eq_num_bands") == 0 || strcmp(key, "eq_num_bands") == 0) {
            int nb = atoi(val);
            if (nb >= 1 && nb <= LITEFX_MAX_EQ_BANDS) cfg->sink_eq_num_bands = (uint32_t)nb;
        } else if (strncmp(key, "sink_eq_l_", 10) == 0 || strncmp(key, "eq_l_", 5) == 0) {
            int idx = atoi(key + (key[0] == 's' ? 10 : 5));
            if (idx >= 0 && idx < LITEFX_MAX_EQ_BANDS) {
                parse_eq_band_tuple(val, &cfg->sink_eq_left[idx]);
            }
        } else if (strncmp(key, "sink_eq_r_", 10) == 0 || strncmp(key, "eq_r_", 5) == 0) {
            int idx = atoi(key + (key[0] == 's' ? 10 : 5));
            if (idx >= 0 && idx < LITEFX_MAX_EQ_BANDS) {
                parse_eq_band_tuple(val, &cfg->sink_eq_right[idx]);
                saw_sink_eq_r = true;
            }
        } else if (strcmp(key, "sink_crystal_bypass") == 0 || strcmp(key, "crystal_bypass") == 0)
            cfg->sink_crystal_bypass = parse_bool_c(val);
        else if (strcmp(key, "sink_crystal_adaptive") == 0 || strcmp(key, "crystal_adaptive") == 0)
            cfg->sink_crystal_adaptive = parse_bool_c(val);
        else if (strncmp(key, "sink_crystal_", 13) == 0 || strncmp(key, "crystal_", 8) == 0) {
            int idx = atoi(key + (key[0] == 's' ? 13 : 8));
            if (idx >= 0 && idx < LITEFX_CRYSTAL_BANDS) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s", val);
                char *save = NULL;
                char *t0 = strtok_r(tmp, ",", &save);
                char *t1 = strtok_r(NULL, ",", &save);
                char *t2 = strtok_r(NULL, ",", &save);
                if (t0) cfg->sink_crystal_bands[idx].intensity_db = parse_float_c(t0);
                if (t1) cfg->sink_crystal_bands[idx].bypass = (atoi(t1) != 0);
                if (t2) cfg->sink_crystal_bands[idx].mute = (atoi(t2) != 0);
            }
        }
        /* Input (Microphone) */
        else if (strcmp(key, "source_bypass") == 0) cfg->source_bypass = parse_bool_c(val);
        else if (strcmp(key, "source_preamp_db") == 0) cfg->source_preamp_db = parse_float_c(val);
        else if (strcmp(key, "source_postgain_db") == 0) cfg->source_postgain_db = parse_float_c(val);
        else if (strcmp(key, "source_noise_gate") == 0) cfg->source_noise_gate = parse_bool_c(val);
        else if (strcmp(key, "source_gate_threshold_db") == 0) cfg->source_gate_threshold_db = parse_float_c(val);
        else if (strcmp(key, "source_soft_limiter") == 0) cfg->source_soft_limiter = parse_bool_c(val);
        else if (strcmp(key, "source_compressor") == 0) cfg->source_compressor = parse_bool_c(val);
        else if (strcmp(key, "source_comp_amount") == 0) cfg->source_comp_amount = parse_float_c(val);
        else if (strcmp(key, "source_denoiser") == 0) cfg->source_denoiser = parse_bool_c(val);
        else if (strcmp(key, "source_denoise_strength") == 0) cfg->source_denoise_strength = parse_float_c(val);
        else if (strcmp(key, "target_source") == 0) snprintf(cfg->target_source, sizeof(cfg->target_source), "%s", val);
        else if (strcmp(key, "auto_route_source_apps") == 0) cfg->auto_route_source_apps = parse_bool_c(val);
        else if (strcmp(key, "source_eq_bypass") == 0) cfg->source_eq_bypass = parse_bool_c(val);
        else if (strcmp(key, "source_eq_num_bands") == 0) {
            int nb = atoi(val);
            if (nb >= 1 && nb <= LITEFX_MAX_EQ_BANDS) cfg->source_eq_num_bands = (uint32_t)nb;
        } else if (strncmp(key, "source_eq_", 10) == 0) {
            int idx = atoi(key + 10);
            if (idx >= 0 && idx < LITEFX_MAX_EQ_BANDS) {
                parse_eq_band_tuple(val, &cfg->source_eq[idx]);
            }
        } else if (strcmp(key, "source_crystal_bypass") == 0) cfg->source_crystal_bypass = parse_bool_c(val);
        else if (strcmp(key, "source_crystal_adaptive") == 0) cfg->source_crystal_adaptive = parse_bool_c(val);
        else if (strncmp(key, "source_crystal_", 15) == 0) {
            int idx = atoi(key + 15);
            if (idx >= 0 && idx < LITEFX_CRYSTAL_BANDS) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s", val);
                char *save = NULL;
                char *t0 = strtok_r(tmp, ",", &save);
                char *t1 = strtok_r(NULL, ",", &save);
                char *t2 = strtok_r(NULL, ",", &save);
                if (t0) cfg->source_crystal_bands[idx].intensity_db = parse_float_c(t0);
                if (t1) cfg->source_crystal_bands[idx].bypass = (atoi(t1) != 0);
                if (t2) cfg->source_crystal_bands[idx].mute = (atoi(t2) != 0);
            }
        } else if (strcmp(key, "spectrum_enabled") == 0) {
            cfg->spectrum_enabled = parse_bool_c(val);
        } else if (strcmp(key, "ui_lang") == 0) {
            int l = atoi(val);
            cfg->ui_lang = (l == 1) ? LITEFX_LANG_UA : LITEFX_LANG_EN;
        } else if (strcmp(key, "dev_profile_count") == 0) {
            int c = atoi(val);
            if (c >= 0 && c <= LITEFX_MAX_DEV_PROFILES) cfg->dev_profile_count = (uint32_t)c;
        } else if (strncmp(key, "dev_profile_", 12) == 0) {
            int idx = atoi(key + 12);
            if (idx >= 0 && idx < LITEFX_MAX_DEV_PROFILES) {
                char *bar = strchr(val, '|');
                if (bar) {
                    *bar = '\0';
                    snprintf(cfg->dev_profiles[idx].device_name,
                             sizeof(cfg->dev_profiles[idx].device_name), "%s", trim_ws(val));
                    snprintf(cfg->dev_profiles[idx].preset_name,
                             sizeof(cfg->dev_profiles[idx].preset_name), "%s", trim_ws(bar + 1));
                    if ((uint32_t)(idx + 1) > cfg->dev_profile_count) {
                        cfg->dev_profile_count = (uint32_t)(idx + 1);
                    }
                }
            }
        }
    }

    if (!cfg->sink_eq_split_channels || !saw_sink_eq_r) {
        for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
            cfg->sink_eq_right[i] = cfg->sink_eq_left[i];
        }
    }

    fclose(fp);
    return true;
}

bool litefx_config_import_easyeffects(LiteFxConfig *cfg, char *msg_out, size_t msg_len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char ee_db[384];
    if (xdg && xdg[0]) {
        snprintf(ee_db, sizeof(ee_db), "%s/easyeffects/db", xdg);
    } else {
        snprintf(ee_db, sizeof(ee_db), "%s/.config/easyeffects/db", home ? home : "");
    }

    char path_main[448], path_cryst[448], path_eq[448];
    snprintf(path_main, sizeof(path_main), "%s/easyeffectsrc", ee_db);
    snprintf(path_cryst, sizeof(path_cryst), "%s/crystalizerrc", ee_db);
    snprintf(path_eq, sizeof(path_eq), "%s/equalizerrc", ee_db);

    FILE *fp_main = fopen(path_main, "r");
    FILE *fp_cryst = fopen(path_cryst, "r");
    FILE *fp_eq = fopen(path_eq, "r");

    if (!fp_main && !fp_cryst && !fp_eq) {
        if (msg_out) snprintf(msg_out, msg_len, "EasyEffects config not found in %s", ee_db);
        return false;
    }

    int soe_cryst_id = 0, soe_eq_id = 0;
    int sie_cryst_id = 0, sie_eq_id = 0;
    bool has_soe_cryst = false, has_soe_eq = false;
    bool has_sie_cryst = false, has_sie_eq = false;

    if (fp_main) {
        char line[512];
        int sec = 0; /* 1 = StreamOutputs, 2 = StreamInputs */
        while (fgets(line, sizeof(line), fp_main)) {
            char *s = trim_ws(line);
            if (!s[0]) continue;
            if (s[0] == '[') {
                if (strcmp(s, "[StreamOutputs]") == 0) sec = 1;
                else if (strcmp(s, "[StreamInputs]") == 0) sec = 2;
                else sec = 0;
                continue;
            }
            if (sec == 0) continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim_ws(s);
            char *v = trim_ws(eq + 1);

            if (sec == 1) {
                if (strcmp(k, "blocklist") == 0) parse_blocklist_csv(cfg, v);
                else if (strcmp(k, "outputDevice") == 0 && v[0]) snprintf(cfg->target_sink, sizeof(cfg->target_sink), "%s", v);
                else if (strcmp(k, "plugins") == 0) {
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "%s", v);
                    char *save = NULL;
                    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                        char *p = trim_ws(tok);
                        if (strncmp(p, "crystalizer#", 12) == 0 && !has_soe_cryst) {
                            soe_cryst_id = atoi(p + 12);
                            has_soe_cryst = true;
                        } else if (strncmp(p, "equalizer#", 10) == 0 && !has_soe_eq) {
                            soe_eq_id = atoi(p + 10);
                            has_soe_eq = true;
                        }
                    }
                }
            } else if (sec == 2) {
                if (strcmp(k, "inputDevice") == 0 && v[0]) snprintf(cfg->target_source, sizeof(cfg->target_source), "%s", v);
                else if (strcmp(k, "plugins") == 0) {
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "%s", v);
                    char *save = NULL;
                    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                        char *p = trim_ws(tok);
                        if (strncmp(p, "crystalizer#", 12) == 0 && !has_sie_cryst) {
                            sie_cryst_id = atoi(p + 12);
                            has_sie_cryst = true;
                        } else if (strncmp(p, "equalizer#", 10) == 0 && !has_sie_eq) {
                            sie_eq_id = atoi(p + 10);
                            has_sie_eq = true;
                        }
                    }
                }
            }
        }
        fclose(fp_main);
    }

    /* Parse Crystalizer (both soe and sie) */
    if (fp_cryst) {
        char want_soe[64], want_sie[64];
        snprintf(want_soe, sizeof(want_soe), "[soe][Crystalizer#%d]", soe_cryst_id);
        snprintf(want_sie, sizeof(want_sie), "[sie][Crystalizer#%d]", sie_cryst_id);
        char line[512];
        int in_sec = 0; /* 1 = soe, 2 = sie */
        while (fgets(line, sizeof(line), fp_cryst)) {
            char *s = trim_ws(line);
            if (!s[0]) continue;
            if (s[0] == '[') {
                if (strcasecmp(s, want_soe) == 0) in_sec = 1;
                else if (strcasecmp(s, want_sie) == 0) in_sec = 2;
                else in_sec = 0;
                continue;
            }
            if (in_sec == 0) continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim_ws(s);
            char *v = trim_ws(eq + 1);

            CrystalBandConfig *bands = (in_sec == 1) ? cfg->sink_crystal_bands : cfg->source_crystal_bands;
            if (strcmp(k, "bypass") == 0) {
                if (in_sec == 1) cfg->sink_crystal_bypass = parse_bool_c(v);
                else cfg->source_crystal_bypass = parse_bool_c(v);
            } else if (strcmp(k, "adaptiveIntensity") == 0) {
                if (in_sec == 1) cfg->sink_crystal_adaptive = parse_bool_c(v);
                else cfg->source_crystal_adaptive = parse_bool_c(v);
            } else if (strncmp(k, "intensityBand", 13) == 0) {
                int b = atoi(k + 13);
                if (b >= 0 && b < LITEFX_CRYSTAL_BANDS) bands[b].intensity_db = parse_float_c(v);
            } else if (strncmp(k, "bypassBand", 10) == 0) {
                int b = atoi(k + 10);
                if (b >= 0 && b < LITEFX_CRYSTAL_BANDS) bands[b].bypass = parse_bool_c(v);
            } else if (strncmp(k, "muteBand", 8) == 0) {
                int b = atoi(k + 8);
                if (b >= 0 && b < LITEFX_CRYSTAL_BANDS) bands[b].mute = parse_bool_c(v);
            }
        }
        fclose(fp_cryst);
    }

    /* Parse Equalizer */
    if (fp_eq) {
        char soe_main[64], soe_l[64], soe_r[64];
        snprintf(soe_main, sizeof(soe_main), "[soe][Equalizer#%d]", soe_eq_id);
        snprintf(soe_l, sizeof(soe_l), "[soe][Equalizer#%d#left]", soe_eq_id);
        snprintf(soe_r, sizeof(soe_r), "[soe][Equalizer#%d#right]", soe_eq_id);

        char sie_main[64], sie_l[64];
        snprintf(sie_main, sizeof(sie_main), "[sie][Equalizer#%d]", sie_eq_id);
        snprintf(sie_l, sizeof(sie_l), "[sie][Equalizer#%d#left]", sie_eq_id);

        int which = 0; /* 1=soe_main, 2=soe_l, 3=soe_r, 4=sie_main, 5=sie_l */
        char line[512];
        while (fgets(line, sizeof(line), fp_eq)) {
            char *s = trim_ws(line);
            if (!s[0]) continue;
            if (s[0] == '[') {
                if (strcasecmp(s, soe_main) == 0) which = 1;
                else if (strcasecmp(s, soe_l) == 0) which = 2;
                else if (strcasecmp(s, soe_r) == 0) which = 3;
                else if (strcasecmp(s, sie_main) == 0) which = 4;
                else if (strcasecmp(s, sie_l) == 0) which = 5;
                else which = 0;
                continue;
            }
            if (which == 0) continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim_ws(s);
            char *v = trim_ws(eq + 1);

            if (which == 1) {
                if (strcmp(k, "bypass") == 0) cfg->sink_eq_bypass = parse_bool_c(v);
                else if (strcmp(k, "splitChannels") == 0) cfg->sink_eq_split_channels = parse_bool_c(v);
                else if (strcmp(k, "inputGain") == 0) cfg->sink_preamp_db = parse_float_c(v);
                else if (strcmp(k, "outputGain") == 0) cfg->sink_postgain_db = parse_float_c(v);
                else if (strcmp(k, "numBands") == 0) {
                    int nb = atoi(v);
                    if (nb >= 1 && nb <= LITEFX_MAX_EQ_BANDS) cfg->sink_eq_num_bands = (uint32_t)nb;
                }
            } else if (which == 4) {
                if (strcmp(k, "bypass") == 0) cfg->source_eq_bypass = parse_bool_c(v);
                else if (strcmp(k, "inputGain") == 0) cfg->source_preamp_db = parse_float_c(v);
                else if (strcmp(k, "outputGain") == 0) cfg->source_postgain_db = parse_float_c(v);
                else if (strcmp(k, "numBands") == 0) {
                    int nb = atoi(v);
                    if (nb >= 1 && nb <= LITEFX_MAX_EQ_BANDS) cfg->source_eq_num_bands = (uint32_t)nb;
                }
            } else if ((which == 2 || which == 3 || which == 5) && strncmp(k, "band", 4) == 0) {
                char *endp = NULL;
                long b = strtol(k + 4, &endp, 10);
                if (b >= 0 && b < LITEFX_MAX_EQ_BANDS && endp) {
                    EqBandConfig *arr = (which == 2) ? cfg->sink_eq_left :
                                        (which == 3) ? cfg->sink_eq_right : cfg->source_eq;
                    if (strcmp(endp, "Frequency") == 0) arr[b].freq = parse_float_c(v);
                    else if (strcmp(endp, "Gain") == 0) arr[b].gain_db = parse_float_c(v);
                    else if (strcmp(endp, "Q") == 0) arr[b].q = parse_float_c(v);
                    else if (strcmp(endp, "Mute") == 0) arr[b].enabled = !parse_bool_c(v);
                    else if (strcmp(endp, "Type") == 0) {
                        int t = atoi(v);
                        if (t == 0) arr[b].enabled = false;
                        else if (t == 1) arr[b].type = EQ_BAND_PEAK;
                        else if (t == 2) arr[b].type = EQ_BAND_HIGH_PASS;
                        else if (t == 3) arr[b].type = EQ_BAND_HIGH_SHELF;
                        else if (t == 4) arr[b].type = EQ_BAND_LOW_PASS;
                        else if (t == 5) arr[b].type = EQ_BAND_LOW_SHELF;
                        else if (t >= 6) arr[b].type = EQ_BAND_NOTCH;
                    }
                }
            }
        }
        fclose(fp_eq);
    }

    if (msg_out) {
        snprintf(msg_out, msg_len, "Imported EasyEffects (Speakers & Mic)");
    }
    return true;
}

bool litefx_config_import_autoeq(LiteFxConfig *cfg, const char *path, bool is_mic,
                                 char *msg_out, size_t msg_len) {
    if (!path || !path[0]) return false;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (msg_out) snprintf(msg_out, msg_len, "Cannot open file: %s", path);
        return false;
    }

    EqBandConfig parsed_bands[LITEFX_MAX_EQ_BANDS];
    uint32_t band_count = 0;
    bool has_preamp = false;
    float preamp_val = 0.0f;
    bool saw_graphic_eq = false;

    float geq_freqs[256];
    float geq_gains[256];
    int geq_count = 0;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_ws(line);
        if (!s[0] || s[0] == '#') continue;

        if (strncasecmp(s, "Preamp:", 7) == 0) {
            preamp_val = parse_float_c(s + 7);
            has_preamp = true;
        } else if (strncasecmp(s, "GraphicEQ:", 10) == 0) {
            saw_graphic_eq = true;
            char *p = s + 10;
            char *save = NULL;
            for (char *tok = strtok_r(p, ";", &save); tok && geq_count < 256; tok = strtok_r(NULL, ";", &save)) {
                char *pair = trim_ws(tok);
                if (!pair[0]) continue;
                char *sp = strchr(pair, ' ');
                if (!sp) sp = strchr(pair, '\t');
                if (sp) {
                    *sp = '\0';
                    float f = parse_float_c(pair);
                    float g = parse_float_c(sp + 1);
                    if (f >= 10.0f && f <= 24000.0f) {
                        geq_freqs[geq_count] = f;
                        geq_gains[geq_count] = g;
                        geq_count++;
                    }
                }
            }
        } else if (strncasecmp(s, "Filter", 6) == 0 && band_count < LITEFX_MAX_EQ_BANDS) {
            char *colon = strchr(s, ':');
            if (!colon) continue;
            char *body = trim_ws(colon + 1);
            bool enabled = true;
            if (strncasecmp(body, "ON", 2) == 0 && isspace((unsigned char)body[2])) {
                enabled = true;
                body = trim_ws(body + 2);
            } else if (strncasecmp(body, "OFF", 3) == 0 && isspace((unsigned char)body[3])) {
                enabled = false;
                body = trim_ws(body + 3);
            }

            uint8_t btype = EQ_BAND_PEAK;
            if (strncasecmp(body, "LSC", 3) == 0 || strncasecmp(body, "LS", 2) == 0) {
                btype = EQ_BAND_LOW_SHELF;
            } else if (strncasecmp(body, "HSC", 3) == 0 || strncasecmp(body, "HS", 2) == 0) {
                btype = EQ_BAND_HIGH_SHELF;
            } else if (strncasecmp(body, "LPQ", 3) == 0 || strncasecmp(body, "LP", 2) == 0) {
                btype = EQ_BAND_LOW_PASS;
            } else if (strncasecmp(body, "HPQ", 3) == 0 || strncasecmp(body, "HP", 2) == 0) {
                btype = EQ_BAND_HIGH_PASS;
            } else if (strncasecmp(body, "NO", 2) == 0) {
                btype = EQ_BAND_NOTCH;
            }

            float fc = 1000.0f, gain = 0.0f, q = 1.414f;
            char *fc_pos = strcasestr(body, "Fc");
            if (fc_pos) fc = parse_float_c(fc_pos + 2);
            char *gain_pos = strcasestr(body, "Gain");
            if (gain_pos) gain = parse_float_c(gain_pos + 4);
            char *q_pos = strcasestr(body, " Q ");
            if (!q_pos) q_pos = strcasestr(body, "\tQ ");
            if (q_pos) q = parse_float_c(q_pos + 2);

            if (fc < 20.0f) fc = 20.0f;
            if (fc > 20000.0f) fc = 20000.0f;
            if (gain < -24.0f) gain = -24.0f;
            if (gain > 24.0f) gain = 24.0f;
            if (q < 0.1f) q = 0.1f;
            if (q > 20.0f) q = 20.0f;

            parsed_bands[band_count].enabled = enabled;
            parsed_bands[band_count].type = btype;
            parsed_bands[band_count].freq = fc;
            parsed_bands[band_count].gain_db = gain;
            parsed_bands[band_count].q = q;
            band_count++;
        }
    }
    fclose(fp);

    if (band_count == 0 && saw_graphic_eq && geq_count >= 2) {
        uint32_t target_n = is_mic ? 8 : 15;
        const float *centers = is_mic ? LITEFX_VOICE_8_FREQS : LITEFX_ISO_15_FREQS;
        float max_gain = 0.0f;
        for (uint32_t i = 0; i < target_n; ++i) {
            float cf = centers[i];
            float g = geq_gains[0];
            if (cf >= geq_freqs[geq_count - 1]) {
                g = geq_gains[geq_count - 1];
            } else {
                for (int k = 0; k < geq_count - 1; ++k) {
                    if (cf >= geq_freqs[k] && cf <= geq_freqs[k + 1]) {
                        float denom = geq_freqs[k + 1] - geq_freqs[k];
                        float t = (denom > 0.01f) ? ((cf - geq_freqs[k]) / denom) : 0.0f;
                        g = geq_gains[k] + t * (geq_gains[k + 1] - geq_gains[k]);
                        break;
                    }
                }
            }
            if (g < -24.0f) g = -24.0f;
            if (g > 24.0f) g = 24.0f;
            if (g > max_gain) max_gain = g;
            parsed_bands[i].enabled = true;
            parsed_bands[i].type = EQ_BAND_PEAK;
            parsed_bands[i].freq = cf;
            parsed_bands[i].gain_db = g;
            parsed_bands[i].q = is_mic ? 1.414f : 1.7f;
        }
        band_count = target_n;
        if (!has_preamp && max_gain > 0.1f) {
            preamp_val = -max_gain;
            has_preamp = true;
        }
    }

    if (band_count == 0) {
        if (msg_out) snprintf(msg_out, msg_len, "No AutoEQ filters found in file");
        return false;
    }

    if (is_mic) {
        cfg->source_eq_bypass = false;
        cfg->source_eq_num_bands = band_count;
        if (has_preamp) cfg->source_preamp_db = preamp_val;
        for (uint32_t i = 0; i < band_count; ++i) {
            cfg->source_eq[i] = parsed_bands[i];
        }
    } else {
        cfg->sink_eq_bypass = false;
        cfg->sink_eq_split_channels = false;
        cfg->sink_eq_num_bands = band_count;
        if (has_preamp) cfg->sink_preamp_db = preamp_val;
        for (uint32_t i = 0; i < band_count; ++i) {
            cfg->sink_eq_left[i] = parsed_bands[i];
            cfg->sink_eq_right[i] = parsed_bands[i];
        }
    }

    if (msg_out) {
        snprintf(msg_out, msg_len, "Imported %u AutoEQ bands (Preamp: %+.1f dB)",
                 band_count, has_preamp ? preamp_val : 0.0f);
    }
    return true;
}

int litefx_list_presets(char names[][64], int max_presets) {
    char dir[384], pdir[448];
    litefx_get_config_dir(dir, sizeof(dir));
    snprintf(pdir, sizeof(pdir), "%s/presets", dir);

    DIR *d = opendir(pdir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_presets) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        if (len > 5 && strcmp(n + len - 5, ".conf") == 0) {
            size_t plen = len - 5;
            if (plen >= 64) plen = 63;
            memcpy(names[count], n, plen);
            names[count][plen] = '\0';
            count++;
        }
    }
    closedir(d);
    return count;
}

bool litefx_send_cmd(const LiteFxCmdPacket *pkt, LiteFxReplyPacket *reply_out) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    litefx_get_sock_path(addr.sun_path, sizeof(addr.sun_path));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    const uint8_t *ptr = (const uint8_t *)pkt;
    size_t rem = sizeof(*pkt);
    while (rem > 0) {
        ssize_t w = send(fd, ptr, rem, MSG_NOSIGNAL);
        if (w <= 0) {
            close(fd);
            return false;
        }
        ptr += (size_t)w;
        rem -= (size_t)w;
    }

    LiteFxReplyPacket rep;
    memset(&rep, 0, sizeof(rep));
    ssize_t r = recv(fd, &rep, sizeof(rep), 0);
    close(fd);

    if (r <= 0) return false;
    if (reply_out) *reply_out = rep;
    return rep.ok != 0;
}
