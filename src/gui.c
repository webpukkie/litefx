#define _GNU_SOURCE
#include "common.h"
#include "config.h"
#include "dsp.h"

#include <cairo.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <libgen.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct GuiApp GuiApp;

typedef struct {
    GuiApp      *app;
    GtkWidget   *window;
    bool         is_microphone; /* false = Speakers, true = Mic */

    /* Topbar widgets */
    GtkWidget   *lbl_brand;
    GtkWidget   *sw_bypass;
    GtkWidget   *chk_limiter;
    GtkWidget   *btn_sw_speakers;
    GtkWidget   *btn_sw_mic;
    GtkWidget   *lbl_dev;
    GtkWidget   *combo_dev;
    char         dev_names[LITEFX_MAX_SINKS + 1][LITEFX_NAME_LEN];
    uint32_t     dev_count;
    GtkWidget   *lbl_top_prof;
    GtkWidget   *combo_top_profile;
    GtkWidget   *btn_top_save;
    GtkWidget   *meter_area;
    GtkWidget   *btn_lang;
    GtkWidget   *btn_reset_top;
    GtkWidget   *btn_stop;

    /* Notebook & Tab Labels */
    GtkWidget   *notebook;
    GtkWidget   *tab_lbl_eq;
    GtkWidget   *tab_lbl_cryst;
    GtkWidget   *tab_lbl_presets;

    /* Equalizer */
    GtkWidget   *lbl_eq_title;
    GtkWidget   *sw_eq;
    GtkWidget   *lbl_preamp;
    GtkWidget   *scale_preamp;
    GtkWidget   *spin_preamp;
    GtkWidget   *chk_auto_preamp;
    GtkWidget   *chk_split_lr;
    GtkWidget   *combo_lr_chan;
    GtkWidget   *chk_spectrum;
    GtkWidget   *btn_autoeq;
    GtkWidget   *bode_area;
    int          selected_eq_band;
    int          dragging_eq_band;
    bool         edit_right_channel;

    /* Inspector */
    GtkWidget   *lbl_sel_band;
    GtkWidget   *chk_band_enabled;
    GtkWidget   *combo_band_type;
    GtkWidget   *lbl_insp_freq;
    GtkWidget   *spin_band_freq;
    GtkWidget   *lbl_insp_gain;
    GtkWidget   *spin_band_gain;
    GtkWidget   *lbl_insp_q;
    GtkWidget   *spin_band_q;

    GtkWidget   *eq_faders_box;
    GtkWidget   *eq_fader_scales[LITEFX_MAX_EQ_BANDS];
    GtkWidget   *eq_fader_labels[LITEFX_MAX_EQ_BANDS];

    /* Crystalizer */
    GtkWidget   *lbl_cryst_title;
    GtkWidget   *sw_cryst;
    GtkWidget   *chk_cryst_adaptive;
    GtkWidget   *cryst_scales[LITEFX_CRYSTAL_BANDS];
    GtkWidget   *cryst_val_labels[LITEFX_CRYSTAL_BANDS];
    GtkWidget   *cryst_btn_bypass[LITEFX_CRYSTAL_BANDS];
    GtkWidget   *cryst_btn_mute[LITEFX_CRYSTAL_BANDS];

    /* Mic specific */
    GtkWidget   *chk_noise_gate;
    GtkWidget   *lbl_gate_thresh;
    GtkWidget   *scale_gate_thresh;
    GtkWidget   *chk_denoiser;
    GtkWidget   *scale_denoise;
    GtkWidget   *chk_compressor;
    GtkWidget   *scale_comp;

    /* Presets / Routing */
    GtkWidget   *lbl_pr_title;
    GtkWidget   *combo_presets;
    GtkWidget   *btn_pr_load;
    GtkWidget   *btn_pr_del;
    GtkWidget   *entry_preset_name;
    GtkWidget   *btn_pr_save;
    GtkWidget   *btn_pr_imp;
    GtkWidget   *btn_pr_exp;
    GtkWidget   *btn_pr_autoeq;
    GtkWidget   *btn_pr_ee;
    GtkWidget   *btn_pr_reset;
    GtkWidget   *lbl_prof_list;
    GtkWidget   *profiles_listbox;

    /* Per-Device Auto-Profile */
    GtkWidget   *lbl_dev_prof_title;
    GtkWidget   *btn_bind_dev_prof;
    GtkWidget   *dev_profiles_listbox;

    GtkWidget   *lbl_autostart;
    GtkWidget   *sw_autostart;
    GtkWidget   *lbl_blocklist;
    GtkWidget   *entry_blocklist;
    GtkWidget   *btn_blocklist;
    GtkWidget   *lbl_streams_title;
    GtkWidget   *streams_listbox;
    GtkWidget   *lbl_ram_note;

    GtkWidget   *lbl_status;
} PipelineWin;

struct GuiApp {
    int          shm_fd;
    LiteFxShm   *shm;
    LiteFxConfig cfg;
    uint32_t     last_seen_seq;
    bool         updating_ui;
    guint        save_timer_id;
    guint        telemetry_timer_id;
    char         active_profile[64];

    PipelineWin  win_sink;
    PipelineWin  win_source;
};

static inline const char *tr(const GuiApp *app, const char *en, const char *ua) {
    return (app && app->cfg.ui_lang == LITEFX_LANG_UA) ? ua : en;
}

static const char *DARK_STUDIO_CSS =
    "* { border-radius: 0px; font-family: 'Monocraft', monospace; outline: none; box-shadow: none; }"
    "window, dialog { background-color: #1e1010; color: #f8dcdb; border: 2px solid #ff9e9e; border-radius: 0px; box-shadow: none; }"
    "headerbar { background-color: #231515; border-bottom: 2px solid #4a2c2c; border-radius: 0px; }"
    "notebook, stack, viewport, scrolledwindow { background-color: #1e1010; color: #f8dcdb; border-radius: 0px; }"
    "notebook > header { background-color: #231515; border-bottom: 2px solid #4a2c2c; padding: 0px; border-radius: 0px; }"
    "notebook > header > tabs > tab { padding: 7px 18px; min-height: 28px; color: #b97272; font-weight: 700; border: none; background-color: #231515; border-radius: 0px; }"
    "notebook > header > tabs > tab:checked { background-color: #f8adfb; color: #1e1010; border: none; font-weight: 700; border-radius: 0px; }"
    ".topbar { background-color: #231515; border-bottom: 2px solid #4a2c2c; padding: 6px 12px; border-radius: 0px; }"
    ".statusbar { background-color: #1e1010; border-top: 1px solid #3d2626; padding: 4px 12px; font-family: 'Monocraft', monospace; font-size: 11px; color: #f8adfb; border-radius: 0px; }"
    ".card { background-color: #231515; border: 1px solid #4a2c2c; padding: 10px; border-radius: 0px; }"
    ".section-title { font-weight: 700; color: #f8adfb; font-size: 13px; }"
    ".dim-label { color: #b97272; font-size: 11px; }"
    ".band-val { font-family: 'Monocraft', monospace; font-size: 10px; color: #f8dcdb; }"
    "button { background-image: none; background-color: #2b1c1b; color: #f8dcdb; border: 1px solid #4a2c2c; padding: 5px 10px; min-height: 28px; font-weight: 600; border-radius: 0px; box-shadow: none; -gtk-outline-radius: 0px; }"
    "button:hover { background-color: #3b2525; border-color: #f8adfb; color: #ffffff; }"
    "button:checked, button.accent { background-color: #f8adfb; color: #1e1010; border-color: #f8adfb; font-weight: 700; }"
    "button.win-switch { background-color: #351a37; color: #f8adfb; border: 1px solid #85378c; font-weight: 700; border-radius: 0px; }"
    "button.win-switch:hover { background-color: #f8adfb; color: #1e1010; border-color: #f8adfb; }"
    "button.mute-btn { background-color: #2b1c1b; color: #b97272; border: 1px solid #4a2c2c; min-width: 22px; min-height: 22px; padding: 2px 0px; font-size: 10px; }"
    "button.mute-btn:checked { background-color: #ff9e9e; color: #1e1010; border-color: #ff9e9e; font-weight: 700; }"
    "button.bypass-btn { background-color: #2b1c1b; color: #b97272; border: 1px solid #4a2c2c; min-width: 22px; min-height: 22px; padding: 2px 0px; font-size: 10px; }"
    "button.bypass-btn:checked { background-color: #d97706; color: #1e1010; border-color: #f59e0b; font-weight: 700; }"
    "button.warning-btn { background-color: #38271a; border-color: #92400e; color: #fde68a; font-weight: 700; border-radius: 0px; }"
    "button.warning-btn:hover { background-color: #78350f; color: #ffffff; }"
    "button.danger { background-color: #3d1a1a; border-color: #8a2525; color: #ff9e9e; font-weight: 700; border-radius: 0px; }"
    "button.danger:hover { background-color: #8a2525; color: #ffffff; }"
    "entry, combobox button { background-color: #2b1c1b; color: #f8dcdb; border: 1px solid #4a2c2c; border-radius: 0px; box-shadow: none; min-height: 28px; }"
    "entry:focus { border-color: #f8adfb; }"
    "spinbutton { background-color: #2b1c1b; color: #f8dcdb; border: 1px solid #4a2c2c; border-radius: 0px; box-shadow: none; min-height: 28px; padding: 0px; }"
    "spinbutton entry { background-color: transparent; border: none; color: #f8dcdb; min-height: 26px; min-width: 44px; padding: 2px 6px; }"
    "spinbutton button { background-color: #3b2525; border: none; border-left: 1px solid #4a2c2c; color: #f8dcdb; min-width: 20px; min-height: 26px; padding: 0px 4px; border-radius: 0px; }"
    "spinbutton button:hover { background-color: #f8adfb; color: #1e1010; }"
    "scale.horizontal trough { background-color: #2b1c1b; border: 1px solid #4a2c2c; min-height: 6px; border-radius: 0px; }"
    "scale.horizontal highlight { background-color: #f8adfb; border-radius: 0px; }"
    "scale.horizontal slider { background-color: #f8adfb; border: 1px solid #1e1010; min-width: 14px; min-height: 14px; margin: -4px 0px; border-radius: 0px; box-shadow: none; }"
    "scale.vertical trough { background-color: #2b1c1b; border: 1px solid #4a2c2c; min-width: 6px; border-radius: 0px; }"
    "scale.vertical highlight { background-color: #f8adfb; border-radius: 0px; }"
    "scale.vertical slider { background-color: #f8adfb; border: 1px solid #1e1010; min-width: 20px; min-height: 10px; margin: 0px -7px; border-radius: 0px; box-shadow: none; }"
    "switch { background-color: #2b1c1b; border: 1px solid #4a2c2c; min-width: 44px; min-height: 22px; margin: 2px 4px; border-radius: 0px; box-shadow: none; padding: 0px; }"
    "switch:checked { background-color: #6b2d72; border-color: #f8adfb; }"
    "switch slider { background-color: #f8adfb; border: 1px solid #1e1010; min-width: 18px; min-height: 18px; margin: 1px; border-radius: 0px; box-shadow: none; }"
    "checkbutton check { background-color: #2b1c1b; border: 1px solid #4a2c2c; min-width: 16px; min-height: 16px; margin: 2px 6px 2px 0px; border-radius: 0px; box-shadow: none; }"
    "checkbutton check:checked { background-color: #f8adfb; color: #1e1010; }"
    "scrollbar trough { background-color: #1e1010; border: none; border-radius: 0px; }"
    "scrollbar slider { background-color: #4a2c2c; border: none; border-radius: 0px; }"
    "scrollbar slider:hover { background-color: #f8adfb; }"
    "list { background-color: #231515; border: 1px solid #4a2c2c; border-radius: 0px; }"
    "row { padding: 5px 10px; border-bottom: 1px solid #2b1c1b; border-radius: 0px; }"
    "separator { background-color: #4a2c2c; }";

static bool connect_shm(GuiApp *app) {
    if (app->shm) return true;
    char shm_path[256];
    litefx_get_shm_path(shm_path, sizeof(shm_path));
    int fd = open(shm_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    void *map = mmap(NULL, sizeof(LiteFxShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    LiteFxShm *shm = (LiteFxShm *)map;
    if (shm->magic != LITEFX_MAGIC) {
        munmap(map, sizeof(LiteFxShm));
        close(fd);
        return false;
    }
    app->shm_fd = fd;
    app->shm = shm;
    atomic_store_explicit(&app->shm->gui_heartbeat_ns, litefx_now_ns(), memory_order_relaxed);
    return true;
}

static bool ensure_daemon_running(GuiApp *app) {
    if (connect_shm(app)) {
        if (kill(app->shm->daemon_pid, 0) == 0) return true;
        munmap(app->shm, sizeof(LiteFxShm));
        close(app->shm_fd);
        app->shm = NULL;
        app->shm_fd = -1;
    }

    char exe_path[512] = {0};
    char daemon_bin[560] = "litefxd";
    ssize_t r = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (r > 0) {
        exe_path[r] = '\0';
        char *dir = dirname(exe_path);
        char candidate[560];
        snprintf(candidate, sizeof(candidate), "%s/litefxd", dir);
        if (access(candidate, X_OK) == 0) {
            snprintf(daemon_bin, sizeof(daemon_bin), "%s", candidate);
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp(daemon_bin, "litefxd", (char *)NULL);
        _exit(127);
    }

    for (int attempt = 0; attempt < 40; ++attempt) {
        usleep(25000);
        if (connect_shm(app)) return true;
    }
    return false;
}

static gboolean save_config_debounced_cb(gpointer data) {
    GuiApp *app = (GuiApp *)data;
    app->save_timer_id = 0;
    LiteFxCmdPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = LITEFX_MAGIC;
    pkt.cmd = CMD_SAVE_CURRENT;
    litefx_send_cmd(&pkt, NULL);
    return G_SOURCE_REMOVE;
}

static void push_config_live(GuiApp *app, bool immediate_save) {
    if (app->updating_ui) return;

    if (!app->cfg.sink_eq_split_channels) {
        for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
            app->cfg.sink_eq_right[i] = app->cfg.sink_eq_left[i];
        }
    }

    LiteFxCmdPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = LITEFX_MAGIC;
    pkt.cmd = CMD_SET_CONFIG;
    pkt.arg_int = immediate_save ? 1 : 0;
    pkt.config = app->cfg;
    litefx_send_cmd(&pkt, NULL);

    if (app->shm) {
        app->last_seen_seq = atomic_load_explicit(&app->shm->config_seq, memory_order_acquire);
    }

    if (!immediate_save) {
        if (app->save_timer_id) g_source_remove(app->save_timer_id);
        app->save_timer_id = g_timeout_add(600, save_config_debounced_cb, app);
    }
}

static void format_freq_short(float f, char *buf, size_t len) {
    if (f >= 1000.0f) {
        float k = f / 1000.0f;
        if (fabsf(k - roundf(k)) < 0.05f) snprintf(buf, len, "%.0fk", k);
        else snprintf(buf, len, "%.1fk", k);
    } else {
        snprintf(buf, len, "%.0f", f);
    }
}

static EqBandConfig *get_active_eq_bands(PipelineWin *pw) {
    GuiApp *app = pw->app;
    if (pw->is_microphone) return app->cfg.source_eq;
    return (app->cfg.sink_eq_split_channels && pw->edit_right_channel)
           ? app->cfg.sink_eq_right : app->cfg.sink_eq_left;
}

static uint32_t get_active_eq_num_bands(PipelineWin *pw) {
    GuiApp *app = pw->app;
    return pw->is_microphone ? app->cfg.source_eq_num_bands : app->cfg.sink_eq_num_bands;
}

static CrystalBandConfig *get_active_cryst_bands(PipelineWin *pw) {
    GuiApp *app = pw->app;
    return pw->is_microphone ? app->cfg.source_crystal_bands : app->cfg.sink_crystal_bands;
}

static double freq_to_x(float freq, double x0, double w) {
    double f = (freq < 20.0f) ? 20.0 : ((freq > 20000.0f) ? 20000.0 : (double)freq);
    return x0 + w * (log10(f / 20.0) / 3.0);
}

static float x_to_freq(double x, double x0, double w) {
    double t = (x - x0) / (w > 1.0 ? w : 1.0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (float)(20.0 * pow(1000.0, t));
}

static double db_to_y(float db, double y0, double h) {
    double d = (db < -24.0f) ? -24.0 : ((db > 24.0f) ? 24.0 : (double)db);
    return y0 + h * (0.5 - d / 48.0);
}

static float y_to_db(double y, double y0, double h) {
    double t = 0.5 - (y - y0) / (h > 1.0 ? h : 1.0);
    double db = t * 48.0;
    if (db < -24.0) db = -24.0;
    if (db > 24.0) db = 24.0;
    return (float)db;
}

static void update_inspector(PipelineWin *pw);
static void rebuild_faders(PipelineWin *pw);
static void refresh_window_ui(PipelineWin *pw);
static void update_window_language(PipelineWin *pw);
static void reload_presets_combo(PipelineWin *pw);
static void reload_dev_profiles_list(PipelineWin *pw);

static gboolean on_bode_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;

    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    const double pad_l = 38.0, pad_r = 14.0, pad_t = 12.0, pad_b = 24.0;
    double width = W - pad_l - pad_r, height = H - pad_t - pad_b;
    if (width < 40 || height < 40) return FALSE;

    cairo_set_source_rgb(cr, 0.118, 0.063, 0.063);
    cairo_paint(cr);

    /* Real-time FFT Spectrum Analyzer (64 log-spaced bins) drawn under grid & curve */
    if (app->cfg.spectrum_enabled && app->shm) {
        const _Atomic(float) *spec = pw->is_microphone
                                     ? app->shm->source_spectrum_db
                                     : app->shm->sink_spectrum_db;
        double bar_w = width / (double)LITEFX_SPECTRUM_BINS;

        cairo_pattern_t *sgrad = cairo_pattern_create_linear(0, pad_t, 0, pad_t + height);
        cairo_pattern_add_color_stop_rgba(sgrad, 0.0, 0.973, 0.678, 0.984, 0.42);
        cairo_pattern_add_color_stop_rgba(sgrad, 0.6, 0.650, 0.250, 0.680, 0.22);
        cairo_pattern_add_color_stop_rgba(sgrad, 1.0, 0.250, 0.100, 0.260, 0.05);

        cairo_new_path(cr);
        cairo_move_to(cr, pad_l, pad_t + height);
        for (int b = 0; b < LITEFX_SPECTRUM_BINS; ++b) {
            float db = atomic_load_explicit(&spec[b], memory_order_relaxed);
            /* Map [-72 dB .. 0 dB] to [0 .. 1] of plot height */
            double norm = ((double)db + 72.0) / 72.0;
            if (norm < 0.0) norm = 0.0;
            if (norm > 1.0) norm = 1.0;
            double bx = pad_l + ((double)b + 0.5) * bar_w;
            double by = pad_t + height * (1.0 - norm);
            cairo_line_to(cr, bx, by);
        }
        cairo_line_to(cr, pad_l + width, pad_t + height);
        cairo_close_path(cr);

        cairo_set_source(cr, sgrad);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(sgrad);

        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, 0.973, 0.678, 0.984, 0.45);
        cairo_stroke(cr);
    }

    cairo_set_line_width(cr, 1.0);
    cairo_select_font_face(cr, "Monocraft", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);

    static const float grid_freqs[] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    for (size_t i = 0; i < sizeof(grid_freqs) / sizeof(grid_freqs[0]); ++i) {
        double x = round(freq_to_x(grid_freqs[i], pad_l, width)) + 0.5;
        cairo_set_source_rgba(cr, 0.40, 0.22, 0.22, 0.55);
        cairo_move_to(cr, x, pad_t);
        cairo_line_to(cr, x, pad_t + height);
        cairo_stroke(cr);

        char lbl[16];
        format_freq_short(grid_freqs[i], lbl, sizeof(lbl));
        cairo_set_source_rgba(cr, 0.725, 0.447, 0.447, 0.95);
        cairo_move_to(cr, x - 10.0, pad_t + height + 15.0);
        cairo_show_text(cr, lbl);
    }

    static const int grid_dbs[] = { -18, -12, -6, 0, 6, 12, 18 };
    for (size_t i = 0; i < sizeof(grid_dbs) / sizeof(grid_dbs[0]); ++i) {
        double y = round(db_to_y((float)grid_dbs[i], pad_t, height)) + 0.5;
        if (grid_dbs[i] == 0) cairo_set_source_rgba(cr, 0.973, 0.678, 0.984, 0.85);
        else cairo_set_source_rgba(cr, 0.40, 0.22, 0.22, 0.55);
        cairo_move_to(cr, pad_l, y);
        cairo_line_to(cr, pad_l + width, y);
        cairo_stroke(cr);

        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%+d", grid_dbs[i]);
        cairo_set_source_rgba(cr, 0.725, 0.447, 0.447, 0.95);
        cairo_move_to(cr, 4.0, y + 3.5);
        cairo_show_text(cr, lbl);
    }

    uint32_t sr = (app->shm) ? atomic_load_explicit(&app->shm->sample_rate, memory_order_relaxed) : 48000u;
    if (sr < 8000u) sr = 48000u;

    bool bypassed = pw->is_microphone
                    ? (app->cfg.source_bypass || app->cfg.source_eq_bypass)
                    : (app->cfg.sink_bypass || app->cfg.sink_eq_bypass);
    double y_zero = db_to_y(0.0f, pad_t, height);

    EqBandConfig *active_bands = get_active_eq_bands(pw);
    uint32_t num_bands = get_active_eq_num_bands(pw);

    /* Fill gradient under curve */
    int steps = (int)width;
    if (steps > 400) steps = 400;
    if (steps < 64) steps = 64;

    cairo_new_path(cr);
    cairo_move_to(cr, pad_l, y_zero);
    for (int s = 0; s <= steps; ++s) {
        double x = pad_l + width * ((double)s / (double)steps);
        float f = x_to_freq(x, pad_l, width);
        float db = dsp_eval_eq_magnitude_db(active_bands, num_bands, sr, f);
        cairo_line_to(cr, x, db_to_y(db, pad_t, height));
    }
    cairo_line_to(cr, pad_l + width, y_zero);
    cairo_close_path(cr);

    cairo_pattern_t *grad = cairo_pattern_create_linear(0, pad_t, 0, pad_t + height);
    double alpha = bypassed ? 0.20 : 1.0;
    cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.973, 0.678, 0.984, 0.35 * alpha);
    cairo_pattern_add_color_stop_rgba(grad, 0.5, 0.420, 0.176, 0.447, 0.08 * alpha);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.973, 0.678, 0.984, 0.35 * alpha);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    /* If Split L/R is active on Sink EQ, draw opposite channel in dashed style */
    if (!pw->is_microphone && app->cfg.sink_eq_split_channels) {
        EqBandConfig *other_bands = pw->edit_right_channel ? app->cfg.sink_eq_left : app->cfg.sink_eq_right;
        cairo_set_line_width(cr, 1.6);
        const double dashes[] = { 4.0, 4.0 };
        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_source_rgba(cr, 0.725, 0.447, 0.650, 0.65);

        for (int s = 0; s <= steps; ++s) {
            double x = pad_l + width * ((double)s / (double)steps);
            float f = x_to_freq(x, pad_l, width);
            float db = dsp_eval_eq_magnitude_db(other_bands, num_bands, sr, f);
            double y = db_to_y(db, pad_t, height);
            if (s == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);

        /* Channel badge on plot */
        cairo_set_source_rgba(cr, 0.973, 0.678, 0.984, 0.92);
        cairo_move_to(cr, pad_l + 8.0, pad_t + 16.0);
        char ch_msg[64];
        snprintf(ch_msg, sizeof(ch_msg), "[%s: %s | %s: %s]",
                 tr(app, "Channel", "Канал"),
                 pw->edit_right_channel ? tr(app, "RIGHT", "ПРАВИЙ") : tr(app, "LEFT", "ЛІВИЙ"),
                 tr(app, "Dashed", "Пунктир"),
                 pw->edit_right_channel ? tr(app, "Left", "Лівий") : tr(app, "Right", "Правий"));
        cairo_show_text(cr, ch_msg);
    }

    /* Stroke Active Curve */
    cairo_set_line_width(cr, 2.2);
    if (bypassed) cairo_set_source_rgba(cr, 0.60, 0.45, 0.50, 0.5);
    else cairo_set_source_rgba(cr, 0.973, 0.678, 0.984, 0.98);

    for (int s = 0; s <= steps; ++s) {
        double x = pad_l + width * ((double)s / (double)steps);
        float f = x_to_freq(x, pad_l, width);
        float db = dsp_eval_eq_magnitude_db(active_bands, num_bands, sr, f);
        double y = db_to_y(db, pad_t, height);
        if (s == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    /* Draggable Band Nodes */
    for (uint32_t i = 0; i < num_bands; ++i) {
        double bx = freq_to_x(active_bands[i].freq, pad_l, width);
        float hdb = (active_bands[i].type <= EQ_BAND_HIGH_SHELF) ? active_bands[i].gain_db : 0.0f;
        double by = db_to_y(hdb, pad_t, height);
        bool sel = ((int)i == pw->selected_eq_band);

        if (sel) {
            cairo_rectangle(cr, bx - 7.5, by - 7.5, 15.0, 15.0);
            cairo_set_source_rgba(cr, 0.973, 0.678, 0.984, 0.35);
            cairo_fill(cr);
        }

        double sz = sel ? 9.0 : 7.0;
        double half = sz * 0.5;
        cairo_rectangle(cr, bx - half, by - half, sz, sz);
        if (!active_bands[i].enabled) cairo_set_source_rgba(cr, 0.42, 0.22, 0.22, 0.8);
        else if (sel) cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        else cairo_set_source_rgb(cr, 0.973, 0.678, 0.984);

        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgb(cr, 0.12, 0.06, 0.06);
        cairo_stroke(cr);
    }

    cairo_set_source_rgb(cr, 0.29, 0.17, 0.17);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, pad_l, pad_t, width, height);
    cairo_stroke(cr);

    return FALSE;
}

static gboolean on_bode_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    const double pad_l = 38.0, pad_r = 14.0, pad_t = 12.0, pad_b = 24.0;
    double width = W - pad_l - pad_r, height = H - pad_t - pad_b;

    EqBandConfig *bands = get_active_eq_bands(pw);
    uint32_t nb = get_active_eq_num_bands(pw);
    int best = -1;
    double best_d2 = 18.0 * 18.0;

    for (uint32_t i = 0; i < nb; ++i) {
        double bx = freq_to_x(bands[i].freq, pad_l, width);
        float hdb = (bands[i].type <= EQ_BAND_HIGH_SHELF) ? bands[i].gain_db : 0.0f;
        double by = db_to_y(hdb, pad_t, height);
        double dx = ev->x - bx, dy = ev->y - by;
        double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = (int)i;
        }
    }

    if (best >= 0) {
        pw->selected_eq_band = best;
        if (ev->button == 3 || ev->type == GDK_2BUTTON_PRESS) {
            bands[best].gain_db = 0.0f;
            pw->app->updating_ui = true;
            if (pw->eq_fader_scales[best]) gtk_range_set_value(GTK_RANGE(pw->eq_fader_scales[best]), 0.0);
            pw->app->updating_ui = false;
            update_inspector(pw);
            push_config_live(pw->app, false);
        } else if (ev->button == 1) {
            pw->dragging_eq_band = best;
            update_inspector(pw);
        }
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

static gboolean on_bode_button_release(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    (void)widget; (void)ev;
    PipelineWin *pw = (PipelineWin *)data;
    pw->dragging_eq_band = -1;
    return TRUE;
}

static gboolean on_bode_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->dragging_eq_band < 0) return FALSE;

    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);
    const double pad_l = 38.0, pad_r = 14.0, pad_t = 12.0, pad_b = 24.0;
    double width = W - pad_l - pad_r, height = H - pad_t - pad_b;

    int b = pw->dragging_eq_band;
    EqBandConfig *band = &get_active_eq_bands(pw)[b];
    band->freq = roundf(x_to_freq(ev->x, pad_l, width));
    if (band->type <= EQ_BAND_HIGH_SHELF) {
        band->gain_db = roundf(y_to_db(ev->y, pad_t, height) * 10.0f) * 0.1f;
    }
    if (!pw->is_microphone && !pw->app->cfg.sink_eq_split_channels) {
        pw->app->cfg.sink_eq_right[b] = *band;
    }

    pw->app->updating_ui = true;
    if (pw->eq_fader_scales[b]) gtk_range_set_value(GTK_RANGE(pw->eq_fader_scales[b]), band->gain_db);
    if (pw->eq_fader_labels[b]) {
        char flbl[16];
        format_freq_short(band->freq, flbl, sizeof(flbl));
        gtk_label_set_text(GTK_LABEL(pw->eq_fader_labels[b]), flbl);
    }
    pw->app->updating_ui = false;

    update_inspector(pw);
    push_config_live(pw->app, false);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean on_bode_scroll(GtkWidget *widget, GdkEventScroll *ev, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    double dy = 0.0;
    if (ev->direction == GDK_SCROLL_UP) dy = -1.0;
    else if (ev->direction == GDK_SCROLL_DOWN) dy = 1.0;
    else if (ev->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent *)ev, NULL, &dy);
    if (fabs(dy) < 0.01) return FALSE;

    int b = pw->selected_eq_band;
    if (b >= 0 && b < (int)get_active_eq_num_bands(pw)) {
        EqBandConfig *band = &get_active_eq_bands(pw)[b];
        float factor = (dy < 0) ? 1.10f : (1.0f / 1.10f);
        float nq = band->q * factor;
        if (nq < 0.1f) nq = 0.1f;
        if (nq > 20.0f) nq = 20.0f;
        band->q = nq;
        if (!pw->is_microphone && !pw->app->cfg.sink_eq_split_channels) {
            pw->app->cfg.sink_eq_right[b] = *band;
        }
        update_inspector(pw);
        push_config_live(pw->app, false);
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

static float amp_to_meter_norm(float amp) {
    if (amp <= 0.0001f) return 0.0f;
    float db = 20.0f * log10f(amp);
    float norm = (db + 48.0f) / 48.0f;
    return norm < 0.0f ? 0.0f : (norm > 1.0f ? 1.0f : norm);
}

static gboolean on_meter_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;
    int W = gtk_widget_get_allocated_width(widget);
    int H = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 0.118, 0.063, 0.063);
    cairo_paint(cr);

    float out_l = 0.0f, out_r = 0.0f;
    if (app->shm) {
        if (pw->is_microphone) {
            out_l = atomic_load_explicit(&app->shm->source_peak_out_l, memory_order_relaxed);
            out_r = atomic_load_explicit(&app->shm->source_peak_out_r, memory_order_relaxed);
        } else {
            out_l = atomic_load_explicit(&app->shm->sink_peak_out_l, memory_order_relaxed);
            out_r = atomic_load_explicit(&app->shm->sink_peak_out_r, memory_order_relaxed);
        }
    }

    float vals[2] = { out_l, out_r };
    double bar_h = (H - 6.0) * 0.5;
    for (int ch = 0; ch < 2; ++ch) {
        double y = 2.0 + ch * (bar_h + 2.0);
        cairo_set_source_rgb(cr, 0.169, 0.110, 0.106);
        cairo_rectangle(cr, 2.0, y, W - 4.0, bar_h);
        cairo_fill(cr);

        float n = amp_to_meter_norm(vals[ch]);
        double fw = (W - 4.0) * (double)n;
        if (fw > 0.5) {
            if (vals[ch] >= 0.98f) cairo_set_source_rgb(cr, 1.0, 0.62, 0.62);
            else if (vals[ch] >= 0.75f) cairo_set_source_rgb(cr, 0.973, 0.678, 0.984);
            else cairo_set_source_rgb(cr, 0.725, 0.447, 0.447);

            cairo_rectangle(cr, 2.0, y, fw, bar_h);
            cairo_fill(cr);
        }

        cairo_set_source_rgb(cr, 0.29, 0.17, 0.17);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, 2.0, y, W - 4.0, bar_h);
        cairo_stroke(cr);
    }
    return FALSE;
}

static void update_inspector(PipelineWin *pw) {
    uint32_t nb = get_active_eq_num_bands(pw);
    if (nb == 0) return;
    if (pw->selected_eq_band >= (int)nb) pw->selected_eq_band = (int)nb - 1;
    if (pw->selected_eq_band < 0) pw->selected_eq_band = 0;

    bool prev = pw->app->updating_ui;
    pw->app->updating_ui = true;

    const EqBandConfig *b = &get_active_eq_bands(pw)[pw->selected_eq_band];
    char title[48];
    snprintf(title, sizeof(title), "%s #%d", tr(pw->app, "Band", "Смуга"), pw->selected_eq_band + 1);
    gtk_label_set_text(GTK_LABEL(pw->lbl_sel_band), title);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_band_enabled), b->enabled);
    gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_band_type), b->type);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pw->spin_band_freq), b->freq);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pw->spin_band_gain), b->gain_db);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pw->spin_band_q), b->q);

    pw->app->updating_ui = prev;
}

static void on_inspector_changed(GtkWidget *w, gpointer data) {
    (void)w;
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int b = pw->selected_eq_band;
    if (b < 0 || b >= (int)get_active_eq_num_bands(pw)) return;

    EqBandConfig *band = &get_active_eq_bands(pw)[b];
    band->enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pw->chk_band_enabled));
    int t = gtk_combo_box_get_active(GTK_COMBO_BOX(pw->combo_band_type));
    if (t >= 0 && t < EQ_BAND_TYPE_COUNT) band->type = (uint8_t)t;
    band->freq = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pw->spin_band_freq));
    band->gain_db = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pw->spin_band_gain));
    band->q = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pw->spin_band_q));

    if (!pw->is_microphone && !pw->app->cfg.sink_eq_split_channels) {
        pw->app->cfg.sink_eq_right[b] = *band;
    }

    pw->app->updating_ui = true;
    if (pw->eq_fader_scales[b]) gtk_range_set_value(GTK_RANGE(pw->eq_fader_scales[b]), band->gain_db);
    if (pw->eq_fader_labels[b]) {
        char flbl[16];
        format_freq_short(band->freq, flbl, sizeof(flbl));
        gtk_label_set_text(GTK_LABEL(pw->eq_fader_labels[b]), flbl);
    }
    pw->app->updating_ui = false;

    push_config_live(pw->app, false);
    gtk_widget_queue_draw(pw->bode_area);
}

static void on_eq_fader_changed(GtkRange *range, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int b = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(range), "band_idx"));
    if (b < 0 || b >= (int)get_active_eq_num_bands(pw)) return;

    EqBandConfig *band = &get_active_eq_bands(pw)[b];
    band->gain_db = (float)gtk_range_get_value(range);
    if (!pw->is_microphone && !pw->app->cfg.sink_eq_split_channels) {
        pw->app->cfg.sink_eq_right[b] = *band;
    }
    pw->selected_eq_band = b;
    update_inspector(pw);
    push_config_live(pw->app, false);
    gtk_widget_queue_draw(pw->bode_area);
}

static void rebuild_faders(PipelineWin *pw) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(pw->eq_faders_box));
    for (GList *it = children; it != NULL; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(children);
    memset(pw->eq_fader_scales, 0, sizeof(pw->eq_fader_scales));
    memset(pw->eq_fader_labels, 0, sizeof(pw->eq_fader_labels));

    const EqBandConfig *bands = get_active_eq_bands(pw);
    uint32_t nb = get_active_eq_num_bands(pw);

    for (uint32_t i = 0; i < nb; ++i) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(col, TRUE);

        GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -24.0, 24.0, 0.5);
        gtk_range_set_inverted(GTK_RANGE(sc), TRUE);
        gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
        gtk_scale_add_mark(GTK_SCALE(sc), 0.0, GTK_POS_RIGHT, NULL);
        gtk_widget_set_size_request(sc, 24, 108);
        gtk_widget_set_vexpand(sc, TRUE);
        gtk_range_set_value(GTK_RANGE(sc), bands[i].gain_db);
        g_object_set_data(G_OBJECT(sc), "band_idx", GINT_TO_POINTER((int)i));
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_eq_fader_changed), pw);

        char flbl[16];
        format_freq_short(bands[i].freq, flbl, sizeof(flbl));
        GtkWidget *lbl = gtk_label_new(flbl);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "band-val");

        gtk_box_pack_start(GTK_BOX(col), sc, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(col), lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(pw->eq_faders_box), col, TRUE, TRUE, 0);

        pw->eq_fader_scales[i] = sc;
        pw->eq_fader_labels[i] = lbl;
    }
    gtk_widget_show_all(pw->eq_faders_box);
}

static void on_device_combo_changed(GtkComboBox *cb, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int idx = gtk_combo_box_get_active(cb);
    if (idx >= 0 && (uint32_t)idx < pw->dev_count) {
        if (pw->is_microphone) {
            snprintf(pw->app->cfg.target_source, sizeof(pw->app->cfg.target_source), "%s", pw->dev_names[idx]);
        } else {
            snprintf(pw->app->cfg.target_sink, sizeof(pw->app->cfg.target_sink), "%s", pw->dev_names[idx]);
        }
        push_config_live(pw->app, true);
    }
}

static void on_global_bypass_set(GtkSwitch *sw, gboolean state, gpointer data) {
    (void)sw;
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->is_microphone) pw->app->cfg.source_bypass = !state;
    else pw->app->cfg.sink_bypass = !state;
    push_config_live(pw->app, false);
    gtk_widget_queue_draw(pw->bode_area);
}

static void on_limiter_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->is_microphone) pw->app->cfg.source_soft_limiter = gtk_toggle_button_get_active(btn);
    else pw->app->cfg.sink_soft_limiter = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_eq_bypass_set(GtkSwitch *sw, gboolean state, gpointer data) {
    (void)sw;
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->is_microphone) pw->app->cfg.source_eq_bypass = !state;
    else pw->app->cfg.sink_eq_bypass = !state;
    push_config_live(pw->app, false);
    gtk_widget_queue_draw(pw->bode_area);
}

static void on_preamp_changed(GtkRange *range, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    float val = (float)gtk_range_get_value(range);

    pw->app->updating_ui = true;
    if (pw->spin_preamp) gtk_spin_button_set_value(GTK_SPIN_BUTTON(pw->spin_preamp), val);
    pw->app->updating_ui = false;

    if (pw->is_microphone) pw->app->cfg.source_preamp_db = val;
    else pw->app->cfg.sink_preamp_db = val;
    push_config_live(pw->app, false);
}

static void on_preamp_spin_changed(GtkSpinButton *spin, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    float val = (float)gtk_spin_button_get_value(spin);

    pw->app->updating_ui = true;
    if (pw->scale_preamp) gtk_range_set_value(GTK_RANGE(pw->scale_preamp), val);
    pw->app->updating_ui = false;

    if (pw->is_microphone) pw->app->cfg.source_preamp_db = val;
    else pw->app->cfg.sink_preamp_db = val;
    push_config_live(pw->app, false);
}

static void on_split_lr_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    gboolean active = gtk_toggle_button_get_active(btn);
    pw->app->cfg.sink_eq_split_channels = active;

    if (active) {
        bool right_empty = true;
        for (uint32_t i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
            if (pw->app->cfg.sink_eq_right[i].gain_db != 0.0f) {
                right_empty = false;
                break;
            }
        }
        if (right_empty) {
            memcpy(pw->app->cfg.sink_eq_right, pw->app->cfg.sink_eq_left, sizeof(pw->app->cfg.sink_eq_right));
        }
    }

    if (pw->combo_lr_chan) {
        gtk_widget_set_sensitive(pw->combo_lr_chan, active);
    }
    refresh_window_ui(pw);
    push_config_live(pw->app, true);
}

static void on_lr_chan_changed(GtkComboBox *combo, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int idx = gtk_combo_box_get_active(combo);
    pw->edit_right_channel = (idx == 1);
    refresh_window_ui(pw);
}

static void on_auto_preamp_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    pw->app->cfg.sink_auto_preamp = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_spectrum_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    bool en = gtk_toggle_button_get_active(btn);
    pw->app->cfg.spectrum_enabled = en;

    pw->app->updating_ui = true;
    if (pw->app->win_sink.chk_spectrum)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->app->win_sink.chk_spectrum), en);
    if (pw->app->win_source.chk_spectrum)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->app->win_source.chk_spectrum), en);
    pw->app->updating_ui = false;

    push_config_live(pw->app, false);
    gtk_widget_queue_draw(pw->app->win_sink.bode_area);
    gtk_widget_queue_draw(pw->app->win_source.bode_area);
}

static void on_noise_gate_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    pw->app->cfg.source_noise_gate = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_gate_thresh_changed(GtkRange *range, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    pw->app->cfg.source_gate_threshold_db = (float)gtk_range_get_value(range);
    push_config_live(pw->app, false);
}

static void on_denoiser_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    pw->app->cfg.source_denoiser = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_denoise_strength_changed(GtkRange *range, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    pw->app->cfg.source_denoise_strength = (float)(gtk_range_get_value(range) / 100.0);
    push_config_live(pw->app, false);
}

static void on_compressor_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    pw->app->cfg.source_compressor = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_comp_amount_changed(GtkRange *range, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    pw->app->cfg.source_comp_amount = (float)(gtk_range_get_value(range) / 100.0);
    push_config_live(pw->app, false);
}

static void on_cryst_bypass_set(GtkSwitch *sw, gboolean state, gpointer data) {
    (void)sw;
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->is_microphone) pw->app->cfg.source_crystal_bypass = !state;
    else pw->app->cfg.sink_crystal_bypass = !state;
    push_config_live(pw->app, false);
}

static void on_cryst_adaptive_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->is_microphone) pw->app->cfg.source_crystal_adaptive = gtk_toggle_button_get_active(btn);
    else pw->app->cfg.sink_crystal_adaptive = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_cryst_band_scale_changed(GtkRange *range, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    int b = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(range), "band_idx"));
    float val = (float)gtk_range_get_value(range);
    char txt[16];
    snprintf(txt, sizeof(txt), "%+.1f", val);
    gtk_label_set_text(GTK_LABEL(pw->cryst_val_labels[b]), txt);

    if (pw->app->updating_ui) return;
    CrystalBandConfig *bands = get_active_cryst_bands(pw);
    bands[b].intensity_db = val;
    push_config_live(pw->app, false);
}

static void on_cryst_band_bypass_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int b = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "band_idx"));
    CrystalBandConfig *bands = get_active_cryst_bands(pw);
    bands[b].bypass = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_cryst_band_mute_toggled(GtkToggleButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int b = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "band_idx"));
    CrystalBandConfig *bands = get_active_cryst_bands(pw);
    bands[b].mute = gtk_toggle_button_get_active(btn);
    push_config_live(pw->app, false);
}

static void on_open_other_window(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *other = (PipelineWin *)data;
    gtk_widget_show_all(other->window);
    gtk_window_present(GTK_WINDOW(other->window));
}

static void on_lang_toggle_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;
    app->cfg.ui_lang = (app->cfg.ui_lang == LITEFX_LANG_UA) ? LITEFX_LANG_EN : LITEFX_LANG_UA;

    update_window_language(&app->win_sink);
    update_window_language(&app->win_source);
    refresh_window_ui(&app->win_sink);
    refresh_window_ui(&app->win_source);
    push_config_live(app, true);
}

static void on_stop_daemon_quit(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    LiteFxCmdPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = LITEFX_MAGIC;
    pkt.cmd = CMD_STOP_DAEMON;
    litefx_send_cmd(&pkt, NULL);
    gtk_main_quit();
}

static bool is_autostart_enabled(void) {
    const char *home = getenv("HOME");
    if (!home) return false;
    char dpath[512];
    snprintf(dpath, sizeof(dpath), "%s/.config/autostart/litefxd.desktop", home);
    if (access(dpath, F_OK) == 0) return true;
    int rc = system("systemctl --user is-enabled --quiet litefxd.service 2>/dev/null");
    return (rc == 0);
}

static void set_autostart_enabled(bool enable) {
    const char *home = getenv("HOME");
    if (!home) return;
    char autostart_dir[512], desktop_path[560], systemd_dir[512], service_path[560];
    snprintf(autostart_dir, sizeof(autostart_dir), "%s/.config/autostart", home);
    snprintf(desktop_path, sizeof(desktop_path), "%s/litefxd.desktop", autostart_dir);
    snprintf(systemd_dir, sizeof(systemd_dir), "%s/.config/systemd/user", home);
    snprintf(service_path, sizeof(service_path), "%s/litefxd.service", systemd_dir);

    if (enable) {
        g_mkdir_with_parents(autostart_dir, 0755);
        g_mkdir_with_parents(systemd_dir, 0755);

        FILE *fd = fopen(desktop_path, "w");
        if (fd) {
            fprintf(fd,
                    "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=LiteFX Audio DSP Daemon\n"
                    "Comment=Background PipeWire Equalizer & Crystalizer\n"
                    "Exec=%s/.local/bin/litefxd\n"
                    "Icon=litefx\n"
                    "Terminal=false\n"
                    "NoDisplay=true\n"
                    "X-GNOME-Autostart-enabled=true\n",
                    home);
            fclose(fd);
        }

        FILE *fs = fopen(service_path, "w");
        if (fs) {
            fprintf(fs,
                    "[Unit]\n"
                    "Description=LiteFX Lightweight Audio DSP Daemon (PipeWire Equalizer & Crystalizer)\n"
                    "After=pipewire.service wireplumber.service\n"
                    "Wants=pipewire.service wireplumber.service\n"
                    "PartOf=pipewire.service\n\n"
                    "[Service]\n"
                    "Type=simple\n"
                    "ExecStart=%s/.local/bin/litefxd\n"
                    "ExecStop=%s/.local/bin/litefxctl stop\n"
                    "Restart=on-failure\n"
                    "RestartSec=2\n"
                    "Slice=session.slice\n\n"
                    "[Install]\n"
                    "WantedBy=default.target\n",
                    home, home);
            fclose(fs);
        }
        int r1 = system("systemctl --user daemon-reload 2>/dev/null; systemctl --user enable litefxd.service 2>/dev/null");
        (void)r1;
    } else {
        unlink(desktop_path);
        int r2 = system("systemctl --user disable litefxd.service 2>/dev/null");
        (void)r2;
    }
}

static void apply_profile_by_name(PipelineWin *pw, const char *name) {
    if (!name || !name[0]) return;
    char ppath[512];
    litefx_get_preset_path(name, ppath, sizeof(ppath));
    LiteFxConfig loaded;
    if (litefx_config_load(&loaded, ppath)) {
        litefx_config_apply_preset_only(&pw->app->cfg, &loaded);
        snprintf(pw->app->active_profile, sizeof(pw->app->active_profile), "%s", name);
        if (pw->entry_preset_name) {
            gtk_entry_set_text(GTK_ENTRY(pw->entry_preset_name), name);
        }
        refresh_window_ui(&pw->app->win_sink);
        refresh_window_ui(&pw->app->win_source);
        reload_presets_combo(&pw->app->win_sink);
        reload_presets_combo(&pw->app->win_source);
        push_config_live(pw->app, true);
    }
}

static void on_profile_row_load_clicked(GtkButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    const char *name = (const char *)g_object_get_data(G_OBJECT(btn), "profile_name");
    apply_profile_by_name(pw, name);
}

static void on_profile_row_overwrite_clicked(GtkButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    const char *name = (const char *)g_object_get_data(G_OBJECT(btn), "profile_name");
    if (!name || !name[0]) return;
    char ppath[512];
    litefx_get_preset_path(name, ppath, sizeof(ppath));
    if (litefx_config_save(&pw->app->cfg, ppath)) {
        snprintf(pw->app->active_profile, sizeof(pw->app->active_profile), "%s", name);
        reload_presets_combo(&pw->app->win_sink);
        reload_presets_combo(&pw->app->win_source);
    }
}

static void on_profile_row_delete_clicked(GtkButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    const char *name = (const char *)g_object_get_data(G_OBJECT(btn), "profile_name");
    if (!name || !name[0]) return;
    char ppath[512];
    litefx_get_preset_path(name, ppath, sizeof(ppath));
    unlink(ppath);
    if (strcmp(pw->app->active_profile, name) == 0) {
        pw->app->active_profile[0] = '\0';
    }
    reload_presets_combo(&pw->app->win_sink);
    reload_presets_combo(&pw->app->win_source);
}

static void on_dev_profile_delete_clicked(GtkButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "rule_idx"));
    GuiApp *app = pw->app;
    if (idx < 0 || (uint32_t)idx >= app->cfg.dev_profile_count) return;
    for (uint32_t i = (uint32_t)idx; i + 1 < app->cfg.dev_profile_count; ++i) {
        app->cfg.dev_profiles[i] = app->cfg.dev_profiles[i + 1];
    }
    app->cfg.dev_profile_count--;
    reload_dev_profiles_list(&app->win_sink);
    reload_dev_profiles_list(&app->win_source);
    push_config_live(app, true);
}

static void on_bind_device_profile_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;

    /* Determine current hardware device name */
    const char *dev_name = NULL;
    int cidx = gtk_combo_box_get_active(GTK_COMBO_BOX(pw->combo_dev));
    if (cidx > 0 && (uint32_t)cidx < pw->dev_count) {
        dev_name = pw->dev_names[cidx];
    } else if (app->shm) {
        uint32_t count = pw->is_microphone ? app->shm->source_count : app->shm->sink_count;
        const LiteFxDeviceInfo *devs = pw->is_microphone ? app->shm->sources : app->shm->sinks;
        const char *res_desc = pw->is_microphone ? app->shm->resolved_source_desc : app->shm->resolved_sink_desc;
        for (uint32_t i = 0; i < count; ++i) {
            if (devs[i].is_default || (res_desc[0] && strcmp(devs[i].description, res_desc) == 0)) {
                dev_name = devs[i].name;
                break;
            }
        }
        if (!dev_name && count > 0) dev_name = devs[0].name;
    }
    if (!dev_name || !dev_name[0] || strcmp(dev_name, "auto") == 0) return;

    /* Determine profile name */
    char preset[64] = {0};
    if (app->active_profile[0]) {
        snprintf(preset, sizeof(preset), "%s", app->active_profile);
    } else if (pw->combo_presets) {
        gchar *txt = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(pw->combo_presets));
        if (txt) {
            snprintf(preset, sizeof(preset), "%s", txt);
            g_free(txt);
        }
    }
    if (!preset[0]) return;

    /* Update existing binding for this device if present, else append */
    bool updated = false;
    for (uint32_t i = 0; i < app->cfg.dev_profile_count; ++i) {
        if (strcmp(app->cfg.dev_profiles[i].device_name, dev_name) == 0) {
            snprintf(app->cfg.dev_profiles[i].preset_name, sizeof(app->cfg.dev_profiles[i].preset_name), "%s", preset);
            updated = true;
            break;
        }
    }
    if (!updated && app->cfg.dev_profile_count < LITEFX_MAX_DEV_PROFILES) {
        LiteFxDevProfileRule *r = &app->cfg.dev_profiles[app->cfg.dev_profile_count++];
        snprintf(r->device_name, sizeof(r->device_name), "%s", dev_name);
        snprintf(r->preset_name, sizeof(r->preset_name), "%s", preset);
    }

    reload_dev_profiles_list(&app->win_sink);
    reload_dev_profiles_list(&app->win_source);
    push_config_live(app, true);
}

static void reload_dev_profiles_list(PipelineWin *pw) {
    if (!pw->dev_profiles_listbox) return;
    GuiApp *app = pw->app;

    GList *children = gtk_container_get_children(GTK_CONTAINER(pw->dev_profiles_listbox));
    for (GList *it = children; it != NULL; it = it->next) {
        gtk_widget_destroy(GTK_WIDGET(it->data));
    }
    g_list_free(children);

    if (app->cfg.dev_profile_count == 0) {
        GtkWidget *lbl = gtk_label_new(tr(app,
            "No device auto-profile bindings yet. Select a profile and click 'Bind Current Device' above.",
            "Немає прив'язок профілів до пристроїв. Оберіть профіль і натисніть «Прив'язати поточний пристрій»."));
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "dim-label");
        gtk_container_add(GTK_CONTAINER(pw->dev_profiles_listbox), lbl);
    } else {
        for (uint32_t i = 0; i < app->cfg.dev_profile_count; ++i) {
            const LiteFxDevProfileRule *r = &app->cfg.dev_profiles[i];
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            char txt[256];
            snprintf(txt, sizeof(txt), "🔗 %s  ➔  [%s]", r->device_name, r->preset_name);
            GtkWidget *lbl = gtk_label_new(txt);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_widget_set_hexpand(lbl, TRUE);

            GtkWidget *btn_del = gtk_button_new_with_label(tr(app, "✖ Unbind", "✖ Відв'язати"));
            gtk_style_context_add_class(gtk_widget_get_style_context(btn_del), "danger");
            g_object_set_data(G_OBJECT(btn_del), "rule_idx", GINT_TO_POINTER((int)i));
            g_signal_connect(btn_del, "clicked", G_CALLBACK(on_dev_profile_delete_clicked), pw);

            gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
            gtk_box_pack_end(GTK_BOX(row), btn_del, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(pw->dev_profiles_listbox), row);
        }
    }
    gtk_widget_show_all(pw->dev_profiles_listbox);
}

static void reload_presets_combo(PipelineWin *pw) {
    GuiApp *app = pw->app;
    bool prev_upd = app->updating_ui;
    app->updating_ui = true;

    char names[64][64];
    int n = litefx_list_presets(names, 64);

    if (pw->combo_presets) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(pw->combo_presets));
        int active_idx = 0;
        for (int i = 0; i < n; ++i) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_presets), names[i]);
            if (app->active_profile[0] && strcmp(names[i], app->active_profile) == 0) {
                active_idx = i;
            }
        }
        if (n > 0) gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_presets), active_idx);
    }

    if (pw->combo_top_profile) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(pw->combo_top_profile));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_top_profile),
                                       tr(app, "(Custom)", "(Свій)"));
        int active_idx = 0;
        for (int i = 0; i < n; ++i) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_top_profile), names[i]);
            if (app->active_profile[0] && strcmp(names[i], app->active_profile) == 0) {
                active_idx = i + 1;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_top_profile), active_idx);
    }

    if (pw->profiles_listbox) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(pw->profiles_listbox));
        for (GList *it = children; it != NULL; it = it->next) {
            gtk_widget_destroy(GTK_WIDGET(it->data));
        }
        g_list_free(children);

        if (n == 0) {
            GtkWidget *lbl_empty = gtk_label_new(tr(app,
                "No saved profiles yet. Enter a name above and click 'Save Profile'.",
                "Немає збережених профілів. Введіть назву вище та натисніть «Зберегти профіль»."));
            gtk_style_context_add_class(gtk_widget_get_style_context(lbl_empty), "dim-label");
            gtk_container_add(GTK_CONTAINER(pw->profiles_listbox), lbl_empty);
        } else {
            for (int i = 0; i < n; ++i) {
                GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                bool is_cur = (app->active_profile[0] && strcmp(names[i], app->active_profile) == 0);

                char title_buf[96];
                snprintf(title_buf, sizeof(title_buf), "%s%.64s", is_cur ? "★ " : "  ", names[i]);
                GtkWidget *lbl = gtk_label_new(title_buf);
                gtk_style_context_add_class(gtk_widget_get_style_context(lbl),
                                            is_cur ? "section-title" : "band-val");
                gtk_widget_set_halign(lbl, GTK_ALIGN_START);
                gtk_widget_set_hexpand(lbl, TRUE);

                GtkWidget *btn_apply = gtk_button_new_with_label(
                    is_cur ? tr(app, "Active", "Активний") : tr(app, "▶ Apply", "▶ Застосувати"));
                if (is_cur) gtk_style_context_add_class(gtk_widget_get_style_context(btn_apply), "accent");
                g_object_set_data_full(G_OBJECT(btn_apply), "profile_name", g_strdup(names[i]), g_free);
                g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_profile_row_load_clicked), pw);

                GtkWidget *btn_upd = gtk_button_new_with_label(tr(app, "💾 Overwrite", "💾 Оновити поточним"));
                g_object_set_data_full(G_OBJECT(btn_upd), "profile_name", g_strdup(names[i]), g_free);
                g_signal_connect(btn_upd, "clicked", G_CALLBACK(on_profile_row_overwrite_clicked), pw);

                GtkWidget *btn_del = gtk_button_new_with_label(tr(app, "🗑 Delete", "🗑 Видалити"));
                gtk_style_context_add_class(gtk_widget_get_style_context(btn_del), "danger");
                g_object_set_data_full(G_OBJECT(btn_del), "profile_name", g_strdup(names[i]), g_free);
                g_signal_connect(btn_del, "clicked", G_CALLBACK(on_profile_row_delete_clicked), pw);

                gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
                gtk_box_pack_end(GTK_BOX(row), btn_del, FALSE, FALSE, 0);
                gtk_box_pack_end(GTK_BOX(row), btn_upd, FALSE, FALSE, 0);
                gtk_box_pack_end(GTK_BOX(row), btn_apply, FALSE, FALSE, 0);
                gtk_container_add(GTK_CONTAINER(pw->profiles_listbox), row);
            }
        }
        gtk_widget_show_all(pw->profiles_listbox);
    }

    reload_dev_profiles_list(pw);
    app->updating_ui = prev_upd;
}

static void on_top_profile_combo_changed(GtkComboBox *combo, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return;
    int active = gtk_combo_box_get_active(combo);
    if (active <= 0) return;
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (!name) return;
    apply_profile_by_name(pw, name);
    g_free(name);
}

static void on_save_preset_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    const char *name = gtk_entry_get_text(GTK_ENTRY(pw->entry_preset_name));
    if (!name || !name[0]) {
        if (pw->app->active_profile[0]) {
            name = pw->app->active_profile;
        } else {
            name = "My_Profile";
        }
    }
    char ppath[512];
    litefx_get_preset_path(name, ppath, sizeof(ppath));
    if (litefx_config_save(&pw->app->cfg, ppath)) {
        snprintf(pw->app->active_profile, sizeof(pw->app->active_profile), "%s", name);
        reload_presets_combo(&pw->app->win_sink);
        reload_presets_combo(&pw->app->win_source);
    }
}

static void on_quick_save_top_profile_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    const char *name = pw->app->active_profile[0] ? pw->app->active_profile : NULL;
    if (!name && pw->entry_preset_name) {
        const char *txt = gtk_entry_get_text(GTK_ENTRY(pw->entry_preset_name));
        if (txt && txt[0]) name = txt;
    }
    if (!name || !name[0]) name = "My_Profile";
    char ppath[512];
    litefx_get_preset_path(name, ppath, sizeof(ppath));
    if (litefx_config_save(&pw->app->cfg, ppath)) {
        snprintf(pw->app->active_profile, sizeof(pw->app->active_profile), "%s", name);
        reload_presets_combo(&pw->app->win_sink);
        reload_presets_combo(&pw->app->win_source);
    }
}

static void on_export_profile_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;
    GtkFileChooserNative *native = gtk_file_chooser_native_new(
        tr(app, "Export LiteFX Profile (.conf)", "Експорт профілю LiteFX (.conf)"),
        GTK_WINDOW(pw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        tr(app, "_Save", "_Зберегти"),
        tr(app, "_Cancel", "_Скасувати"));
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(native), TRUE);
    char def_file[128];
    snprintf(def_file, sizeof(def_file), "%s.conf",
             app->active_profile[0] ? app->active_profile : "litefx_profile");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(native), def_file);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(native)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native));
        if (filename) {
            litefx_config_save(&app->cfg, filename);
            g_free(filename);
        }
    }
    g_object_unref(native);
}

static void on_import_profile_file_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;
    GtkFileChooserNative *native = gtk_file_chooser_native_new(
        tr(app, "Import LiteFX Profile (.conf)", "Імпорт профілю LiteFX (.conf)"),
        GTK_WINDOW(pw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        tr(app, "_Open", "_Відкрити"),
        tr(app, "_Cancel", "_Скасувати"));

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(native)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native));
        if (filename) {
            LiteFxConfig loaded;
            if (litefx_config_load(&loaded, filename)) {
                litefx_config_apply_preset_only(&app->cfg, &loaded);
                char *base = g_path_get_basename(filename);
                char *dot = strrchr(base, '.');
                if (dot) *dot = '\0';
                char ppath[512];
                litefx_get_preset_path(base, ppath, sizeof(ppath));
                litefx_config_save(&app->cfg, ppath);
                snprintf(app->active_profile, sizeof(app->active_profile), "%s", base);
                g_free(base);

                refresh_window_ui(&app->win_sink);
                refresh_window_ui(&app->win_source);
                reload_presets_combo(&app->win_sink);
                reload_presets_combo(&app->win_source);
                push_config_live(app, true);
            }
            g_free(filename);
        }
    }
    g_object_unref(native);
}

static void on_import_autoeq_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    GuiApp *app = pw->app;
    GtkFileChooserNative *native = gtk_file_chooser_native_new(
        tr(app, "Import AutoEQ / EqualizerAPO (.txt)", "Імпорт AutoEQ / EqualizerAPO (.txt)"),
        GTK_WINDOW(pw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        tr(app, "_Import", "_Імпортувати"),
        tr(app, "_Cancel", "_Скасувати"));

    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "AutoEQ / EqualizerAPO (*.txt)");
    gtk_file_filter_add_pattern(flt, "*.txt");
    gtk_file_filter_add_pattern(flt, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(native), flt);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(native)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native));
        if (filename) {
            char msg[256] = {0};
            if (litefx_config_import_autoeq(&app->cfg, filename, pw->is_microphone, msg, sizeof(msg))) {
                refresh_window_ui(&app->win_sink);
                refresh_window_ui(&app->win_source);
                push_config_live(app, true);
                if (pw->lbl_status && msg[0]) {
                    gtk_label_set_text(GTK_LABEL(pw->lbl_status), msg);
                }
            }
            g_free(filename);
        }
    }
    g_object_unref(native);
}

static gboolean on_autostart_switch_set(GtkSwitch *sw, gboolean state, gpointer data) {
    (void)sw;
    PipelineWin *pw = (PipelineWin *)data;
    if (pw->app->updating_ui) return FALSE;
    set_autostart_enabled(state);
    pw->app->updating_ui = true;
    if (pw->app->win_sink.sw_autostart)
        gtk_switch_set_active(GTK_SWITCH(pw->app->win_sink.sw_autostart), state);
    if (pw->app->win_source.sw_autostart)
        gtk_switch_set_active(GTK_SWITCH(pw->app->win_source.sw_autostart), state);
    pw->app->updating_ui = false;
    return FALSE;
}

static void on_load_preset_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(pw->combo_presets));
    if (!name) return;
    apply_profile_by_name(pw, name);
    g_free(name);
}

static void on_delete_preset_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(pw->combo_presets));
    if (!name) return;
    char ppath[512];
    litefx_get_preset_path(name, ppath, sizeof(ppath));
    if (strcmp(pw->app->active_profile, name) == 0) {
        pw->app->active_profile[0] = '\0';
    }
    g_free(name);

    unlink(ppath);
    reload_presets_combo(&pw->app->win_sink);
    reload_presets_combo(&pw->app->win_source);
}

static void on_import_ee_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    char msg[256];
    if (litefx_config_import_easyeffects(&pw->app->cfg, msg, sizeof(msg))) {
        refresh_window_ui(&pw->app->win_sink);
        refresh_window_ui(&pw->app->win_source);
        push_config_live(pw->app, true);
    }
}

static void on_reset_defaults_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    LiteFxConfig def;
    litefx_config_set_defaults(&def);

    if (pw->is_microphone) {
        pw->app->cfg.source_bypass = def.source_bypass;
        pw->app->cfg.source_preamp_db = def.source_preamp_db;
        pw->app->cfg.source_postgain_db = def.source_postgain_db;
        pw->app->cfg.source_soft_limiter = def.source_soft_limiter;
        pw->app->cfg.source_noise_gate = def.source_noise_gate;
        pw->app->cfg.source_gate_threshold_db = def.source_gate_threshold_db;
        pw->app->cfg.source_compressor = def.source_compressor;
        pw->app->cfg.source_comp_amount = def.source_comp_amount;
        pw->app->cfg.source_denoiser = def.source_denoiser;
        pw->app->cfg.source_denoise_strength = def.source_denoise_strength;
        pw->app->cfg.source_eq_bypass = def.source_eq_bypass;
        pw->app->cfg.source_eq_num_bands = def.source_eq_num_bands;
        for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
            pw->app->cfg.source_eq[i] = def.source_eq[i];
        }
        pw->app->cfg.source_crystal_bypass = def.source_crystal_bypass;
        pw->app->cfg.source_crystal_adaptive = def.source_crystal_adaptive;
        for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
            pw->app->cfg.source_crystal_bands[b] = def.source_crystal_bands[b];
        }
    } else {
        pw->app->cfg.sink_bypass = def.sink_bypass;
        pw->app->cfg.sink_preamp_db = def.sink_preamp_db;
        pw->app->cfg.sink_postgain_db = def.sink_postgain_db;
        pw->app->cfg.sink_auto_preamp = def.sink_auto_preamp;
        pw->app->cfg.sink_soft_limiter = def.sink_soft_limiter;
        pw->app->cfg.sink_eq_bypass = def.sink_eq_bypass;
        pw->app->cfg.sink_eq_split_channels = def.sink_eq_split_channels;
        pw->app->cfg.sink_eq_num_bands = def.sink_eq_num_bands;
        for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
            pw->app->cfg.sink_eq_left[i] = def.sink_eq_left[i];
            pw->app->cfg.sink_eq_right[i] = def.sink_eq_right[i];
        }
        pw->app->cfg.sink_crystal_bypass = def.sink_crystal_bypass;
        pw->app->cfg.sink_crystal_adaptive = def.sink_crystal_adaptive;
        for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
            pw->app->cfg.sink_crystal_bands[b] = def.sink_crystal_bands[b];
        }
    }

    refresh_window_ui(pw);
    push_config_live(pw->app, true);
}

static void on_apply_blocklist_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    PipelineWin *pw = (PipelineWin *)data;
    if (!pw->entry_blocklist) return;
    const char *txt = gtk_entry_get_text(GTK_ENTRY(pw->entry_blocklist));
    pw->app->cfg.sink_blocklist_count = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", txt);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save);
         tok && pw->app->cfg.sink_blocklist_count < LITEFX_MAX_BLOCKLIST;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        size_t l = strlen(tok);
        while (l > 0 && tok[l - 1] == ' ') tok[--l] = '\0';
        if (tok[0]) {
            snprintf(pw->app->cfg.sink_blocklist[pw->app->cfg.sink_blocklist_count++],
                     64, "%s", tok);
        }
    }
    push_config_live(pw->app, true);
}

static void on_eq_quick_preset_clicked(GtkButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "curve_id"));
    EqBandConfig *arr = get_active_eq_bands(pw);
    uint32_t num = get_active_eq_num_bands(pw);

    if (!pw->is_microphone) {
        static const float curves[5][10] = {
            {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, /* Flat */
            {  2.0f,  1.5f,  0.5f, -0.5f, -0.5f,  0.5f,  1.0f,  1.5f,  1.0f,  0.5f }, /* Harman / V-Shape */
            {  3.0f,  2.0f,  1.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, /* Bass+ */
            { -1.0f, -0.5f,  0.0f,  0.0f,  1.0f,  2.0f,  2.0f,  1.0f,  0.0f,  0.0f }, /* Vocal */
            {  1.5f,  1.0f,  0.5f,  0.0f,  0.0f,  0.0f, -0.5f, -1.0f, -1.5f, -1.0f }, /* Warm */
        };
        for (uint32_t i = 0; i < num; ++i) {
            arr[i].gain_db = (pid == 0) ? 0.0f : curves[pid][i % 10];
        }
        if (!pw->app->cfg.sink_eq_split_channels) {
            for (int i = 0; i < LITEFX_MAX_EQ_BANDS; ++i) {
                pw->app->cfg.sink_eq_right[i] = pw->app->cfg.sink_eq_left[i];
            }
        }
    } else {
        static const float mic_curves[5][8] = {
            {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, /* Flat */
            { -2.0f, -1.0f,  0.0f,  1.0f,  2.0f,  2.5f,  1.5f,  0.5f }, /* Vocal Clarity */
            { -9.0f, -4.0f,  0.0f,  0.0f,  0.0f,  0.5f,  1.0f,  0.0f }, /* Low-Cut */
            { -1.0f,  0.0f,  0.0f,  1.0f,  2.0f,  2.0f,  1.0f,  0.0f }, /* Presence */
            {  1.5f,  1.0f,  0.5f,  0.0f,  0.0f,  0.5f,  0.0f, -1.0f }, /* Warm Voice */
        };
        for (uint32_t i = 0; i < num; ++i) {
            arr[i].gain_db = (pid == 0) ? 0.0f : mic_curves[pid][i % 8];
        }
    }

    refresh_window_ui(pw);
    push_config_live(pw->app, false);
}

static void on_cryst_quick_preset_clicked(GtkButton *btn, gpointer data) {
    PipelineWin *pw = (PipelineWin *)data;
    int cid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "cryst_curve"));
    CrystalBandConfig *bands = get_active_cryst_bands(pw);

    static const float ee_user[13]  = { -2.0f, -2.0f, -4.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -5.0f, -5.0f, -2.0f };
    static const float balanced[13] = { -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f, -2.0f };
    static const float crisp[13]    = { -3.0f, -3.0f, -2.0f, -1.0f,  0.0f,  0.0f,  0.0f, -1.0f, -1.0f, -2.0f, -4.0f, -4.0f, -2.0f };
    static const float punchy[13]   = {  0.0f,  0.0f, -1.0f, -2.0f, -2.0f, -2.0f, -2.0f, -3.0f, -3.0f, -4.0f, -5.0f, -5.0f, -3.0f };
    static const float subtle[13]   = { -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -8.0f, -8.0f, -6.0f };

    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        bands[b].bypass = false;
        bands[b].mute = false;
        if (cid == 0) bands[b].intensity_db = ee_user[b];
        else if (cid == 1) bands[b].intensity_db = balanced[b];
        else if (cid == 2) bands[b].intensity_db = crisp[b];
        else if (cid == 3) bands[b].intensity_db = punchy[b];
        else if (cid == 4) bands[b].intensity_db = subtle[b];
    }

    refresh_window_ui(pw);
    push_config_live(pw->app, false);
}

static gboolean on_window_delete(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)ev;
    PipelineWin *pw = (PipelineWin *)data;
    gtk_widget_hide(w);

    PipelineWin *other = pw->is_microphone ? &pw->app->win_sink : &pw->app->win_source;
    if (!other->window || !gtk_widget_get_visible(other->window)) {
        if (pw->app->shm) {
            atomic_store_explicit(&pw->app->shm->gui_heartbeat_ns, 0ULL, memory_order_relaxed);
        }
        gtk_main_quit();
    }
    return TRUE;
}

static void update_window_language(PipelineWin *pw) {
    if (!pw || !pw->window) return;
    GuiApp *app = pw->app;
    bool prev_upd = app->updating_ui;
    app->updating_ui = true;

    if (pw->is_microphone) {
        gtk_window_set_title(GTK_WINDOW(pw->window),
                             tr(app, "LiteFX — Microphone DSP", "LiteFX — Мікрофон (DSP)"));
    } else {
        gtk_window_set_title(GTK_WINDOW(pw->window),
                             tr(app, "LiteFX — Speakers & Headphones DSP", "LiteFX — Динаміки та навушники (DSP)"));
    }

    if (pw->lbl_brand) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_brand),
                           pw->is_microphone ? tr(app, "LiteFX Mic", "LiteFX Мікрофон")
                                             : tr(app, "LiteFX Output", "LiteFX Вихід"));
    }
    if (pw->chk_limiter) {
        gtk_button_set_label(GTK_BUTTON(pw->chk_limiter),
                             pw->is_microphone ? tr(app, "Voice Limiter", "Лімітер голосу")
                                               : tr(app, "Soft Limiter", "М'який лімітер"));
    }
    if (pw->btn_sw_speakers) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_sw_speakers), tr(app, "🎧 Speakers", "🎧 Динаміки"));
    }
    if (pw->btn_sw_mic) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_sw_mic), tr(app, "🎤 Microphone", "🎤 Мікрофон"));
    }
    if (pw->lbl_dev) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_dev),
                           pw->is_microphone ? tr(app, "Mic:", "Мікрофон:")
                                             : tr(app, "Device:", "Пристрій:"));
    }
    if (pw->lbl_top_prof) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_top_prof), tr(app, "Profile:", "Профіль:"));
    }
    if (pw->combo_top_profile) {
        gtk_widget_set_tooltip_text(pw->combo_top_profile,
                                    tr(app, "Quick-switch saved profiles", "Швидке перемикання збережених профілів"));
    }
    if (pw->btn_top_save) {
        gtk_widget_set_tooltip_text(pw->btn_top_save,
                                    tr(app, "Save changes to active profile", "Зберегти зміни у поточний профіль"));
    }
    if (pw->btn_lang) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_lang),
                             app->cfg.ui_lang == LITEFX_LANG_UA ? "🌐 UA" : "🌐 EN");
        gtk_widget_set_tooltip_text(pw->btn_lang,
                                    tr(app, "Switch language to Ukrainian (UA)", "Перемкнути мову на англійську (EN)"));
    }
    if (pw->btn_reset_top) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_reset_top), tr(app, "↺ Reset", "↺ Скинути"));
        gtk_widget_set_tooltip_text(pw->btn_reset_top,
                                    tr(app, "Reset current pipeline settings to defaults",
                                       "Скинути поточні налаштування до стандартних"));
    }
    if (pw->btn_stop) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_stop), tr(app, "Stop DSP & Quit", "Стоп DSP і вихід"));
    }

    if (pw->tab_lbl_eq) {
        gtk_label_set_text(GTK_LABEL(pw->tab_lbl_eq), tr(app, "Equalizer", "Еквалайзер"));
    }
    if (pw->tab_lbl_cryst) {
        gtk_label_set_text(GTK_LABEL(pw->tab_lbl_cryst), tr(app, "Crystalizer (13-Band)", "Кристалайзер (13 смуг)"));
    }
    if (pw->tab_lbl_presets) {
        gtk_label_set_text(GTK_LABEL(pw->tab_lbl_presets),
                           pw->is_microphone ? tr(app, "Profiles & Autostart", "Профілі та автозапуск")
                                             : tr(app, "Profiles & Routing", "Профілі та маршрутизація"));
    }

    if (pw->lbl_eq_title) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_eq_title), tr(app, "Equalizer", "Еквалайзер"));
    }
    if (pw->lbl_preamp) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_preamp),
                           pw->is_microphone ? tr(app, "Mic Gain:", "Підсилення:")
                                             : tr(app, "Preamp:", "Преамп:"));
    }
    if (pw->chk_spectrum) {
        gtk_button_set_label(GTK_BUTTON(pw->chk_spectrum), tr(app, "Spectrum", "Спектр"));
        gtk_widget_set_tooltip_text(pw->chk_spectrum,
                                    tr(app, "Real-time 64-band FFT spectrum analyzer (0% CPU when closed)",
                                       "Аналізатор спектра FFT у реальному часі (0% CPU коли вікно закрите)"));
    }
    if (pw->btn_autoeq) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_autoeq), tr(app, "📥 AutoEQ", "📥 AutoEQ"));
        gtk_widget_set_tooltip_text(pw->btn_autoeq,
                                    tr(app, "Import AutoEQ / EqualizerAPO ParametricEQ.txt or GraphicEQ.txt",
                                       "Імпортувати AutoEQ / EqualizerAPO ParametricEQ.txt або GraphicEQ.txt"));
    }

    if (!pw->is_microphone) {
        if (pw->chk_auto_preamp) {
            gtk_button_set_label(GTK_BUTTON(pw->chk_auto_preamp), tr(app, "Auto-Preamp", "Авто-преамп"));
        }
        if (pw->chk_split_lr) {
            gtk_button_set_label(GTK_BUTTON(pw->chk_split_lr), tr(app, "Split L/R", "Окремо L/R"));
        }
        if (pw->combo_lr_chan) {
            int cur = gtk_combo_box_get_active(GTK_COMBO_BOX(pw->combo_lr_chan));
            if (cur < 0) cur = pw->edit_right_channel ? 1 : 0;
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(pw->combo_lr_chan));
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_lr_chan), tr(app, "Left", "Лівий"));
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_lr_chan), tr(app, "Right", "Правий"));
            gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_lr_chan), cur);
        }
    } else {
        if (pw->chk_noise_gate) {
            gtk_button_set_label(GTK_BUTTON(pw->chk_noise_gate), tr(app, "Gate", "Шумопоріг"));
        }
        if (pw->lbl_gate_thresh) {
            gtk_label_set_text(GTK_LABEL(pw->lbl_gate_thresh), tr(app, "Thresh:", "Поріг:"));
        }
        if (pw->chk_denoiser) {
            gtk_button_set_label(GTK_BUTTON(pw->chk_denoiser), tr(app, "Denoise", "Шумодав"));
            gtk_widget_set_tooltip_text(pw->chk_denoiser,
                                        tr(app, "Adaptive stationary background noise & hum suppressor",
                                           "Адаптивне придушення фонового шуму та гудіння"));
        }
        if (pw->chk_compressor) {
            gtk_button_set_label(GTK_BUTTON(pw->chk_compressor), tr(app, "Compressor", "Компресор"));
            gtk_widget_set_tooltip_text(pw->chk_compressor,
                                        tr(app, "Broadcast voice leveler & compressor",
                                           "Студійний компресор для вирівнювання гучності голосу"));
        }
    }

    if (pw->chk_band_enabled) {
        gtk_button_set_label(GTK_BUTTON(pw->chk_band_enabled), tr(app, "On", "Увімк."));
    }
    if (pw->lbl_insp_freq) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_insp_freq), tr(app, "Freq:", "Частота:"));
    }
    if (pw->lbl_insp_gain) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_insp_gain), tr(app, "Gain:", "Підсил.:"));
    }
    if (pw->lbl_cryst_title) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_cryst_title),
                           tr(app, "13-Band Crystalizer (Dynamic Transient Enhancer)",
                              "13-смуговий Кристалайзер (динамічне підсилення транзієнтів)"));
    }
    if (pw->chk_cryst_adaptive) {
        gtk_button_set_label(GTK_BUTTON(pw->chk_cryst_adaptive),
                             tr(app, "Adaptive Intensity", "Адаптивна інтенсивність"));
    }

    /* Presets tab labels */
    if (pw->lbl_pr_title) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_pr_title), tr(app, "Profile:", "Профіль:"));
    }
    if (pw->btn_pr_load) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_load), tr(app, "Load", "Завантажити"));
    }
    if (pw->btn_pr_del) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_del), tr(app, "Delete", "Видалити"));
    }
    if (pw->entry_preset_name) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(pw->entry_preset_name),
            tr(app, "New profile name (e.g. Music, Gaming, Movie)...",
               "Назва нового профілю (напр. Музика, Ігри, Кіно)..."));
    }
    if (pw->btn_pr_save) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_save), tr(app, "💾 Save Profile", "💾 Зберегти профіль"));
    }
    if (pw->btn_pr_imp) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_imp), tr(app, "📂 Import", "📂 Імпорт"));
        gtk_widget_set_tooltip_text(pw->btn_pr_imp,
            tr(app, "Import profile from a .conf file", "Імпортувати профіль із файлу .conf"));
    }
    if (pw->btn_pr_exp) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_exp), tr(app, "📤 Export", "📤 Експорт"));
        gtk_widget_set_tooltip_text(pw->btn_pr_exp,
            tr(app, "Export current profile to a .conf file", "Експортувати поточний профіль у файл .conf"));
    }
    if (pw->btn_pr_autoeq) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_autoeq), tr(app, "📥 AutoEQ (.txt)", "📥 AutoEQ (.txt)"));
        gtk_widget_set_tooltip_text(pw->btn_pr_autoeq,
            tr(app, "Import AutoEQ / EqualizerAPO ParametricEQ.txt or GraphicEQ.txt",
               "Імпортувати AutoEQ / EqualizerAPO ParametricEQ.txt або GraphicEQ.txt"));
    }
    if (pw->btn_pr_ee) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_ee), tr(app, "EasyEffects", "EasyEffects"));
    }
    if (pw->btn_pr_reset) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_pr_reset), tr(app, "↺ Defaults", "↺ Стандартні"));
    }
    if (pw->lbl_prof_list) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_prof_list),
            tr(app, "Saved Profiles (~/.config/litefx/presets/)",
               "Мої збережені профілі (~/.config/litefx/presets/)"));
    }
    if (pw->lbl_dev_prof_title) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_dev_prof_title),
            tr(app, "🎧 Per-Device Auto-Profile (auto-loads profile when headphones/speakers connect):",
               "🎧 Авто-профіль для пристроїв (автоматично вмикає профіль при підключенні навушників/колонок):"));
    }
    if (pw->btn_bind_dev_prof) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_bind_dev_prof),
            tr(app, "🔗 Bind Active Profile to Current Device", "🔗 Прив'язати профіль до поточного пристрою"));
    }
    if (pw->lbl_autostart) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_autostart),
            tr(app, "🚀 Start background DSP daemon (litefxd) automatically on login (systemd + XDG Autostart):",
               "🚀 Автозапуск фонового аудіо-процесора (litefxd) разом із системою (systemd + XDG Autostart):"));
    }
    if (pw->lbl_blocklist) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_blocklist),
            tr(app, "App Blocklist (CSV):", "Чорний список програм (CSV):"));
    }
    if (pw->btn_blocklist) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_blocklist),
            tr(app, "Apply Blocklist", "Застосувати список"));
    }
    if (pw->lbl_streams_title) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_streams_title),
            tr(app, "Active Audio Playback Streams", "Активні потоки відтворення аудіо"));
    }
    if (pw->lbl_ram_note) {
        gtk_label_set_text(GTK_LABEL(pw->lbl_ram_note),
            tr(app, "Tip: Closing this window frees 100% of GUI memory (0 MB) while litefxd continues processing audio in the background (~2.5 MB RAM).",
               "Порада: Закриття вікна звільняє 100% пам'яті GUI (0 МБ), а litefxd продовжує обробляти звук у фоні (~2.5 МБ ОЗП)."));
    }

    app->updating_ui = prev_upd;
}

static void refresh_window_ui(PipelineWin *pw) {
    GuiApp *app = pw->app;
    pw->app->updating_ui = true;

    /* Top bar */
    bool glob_on = pw->is_microphone ? !app->cfg.source_bypass : !app->cfg.sink_bypass;
    gtk_switch_set_active(GTK_SWITCH(pw->sw_bypass), glob_on);

    bool lim_on = pw->is_microphone ? app->cfg.source_soft_limiter : app->cfg.sink_soft_limiter;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_limiter), lim_on);

    if (pw->btn_lang) {
        gtk_button_set_label(GTK_BUTTON(pw->btn_lang),
                             app->cfg.ui_lang == LITEFX_LANG_UA ? "🌐 UA" : "🌐 EN");
    }

    /* Device combo */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(pw->combo_dev));
    strcpy(pw->dev_names[0], "auto");
    char auto_lbl[192];
    if (pw->is_microphone) {
        snprintf(auto_lbl, sizeof(auto_lbl), "Auto (%s)",
                 (app->shm && app->shm->resolved_source_desc[0]) ? app->shm->resolved_source_desc
                 : tr(app, "Default Mic", "Стандартний мікрофон"));
    } else {
        snprintf(auto_lbl, sizeof(auto_lbl), "Auto (%s)",
                 (app->shm && app->shm->resolved_sink_desc[0]) ? app->shm->resolved_sink_desc
                 : tr(app, "Default Output", "Стандартний вихід"));
    }
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_dev), auto_lbl);
    pw->dev_count = 1;

    int active_idx = 0;
    if (app->shm) {
        uint32_t count = pw->is_microphone ? app->shm->source_count : app->shm->sink_count;
        const LiteFxDeviceInfo *devs = pw->is_microphone ? app->shm->sources : app->shm->sinks;
        const char *wanted = pw->is_microphone ? app->cfg.target_source : app->cfg.target_sink;

        for (uint32_t i = 0; i < count && pw->dev_count <= LITEFX_MAX_SINKS; ++i) {
            snprintf(pw->dev_names[pw->dev_count], LITEFX_NAME_LEN, "%s", devs[i].name);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_dev), devs[i].description);
            if (strcmp(wanted, devs[i].name) == 0) active_idx = (int)pw->dev_count;
            pw->dev_count++;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_dev), active_idx);

    /* Equalizer */
    bool eq_on = pw->is_microphone ? !app->cfg.source_eq_bypass : !app->cfg.sink_eq_bypass;
    gtk_switch_set_active(GTK_SWITCH(pw->sw_eq), eq_on);

    float preamp = pw->is_microphone ? app->cfg.source_preamp_db : app->cfg.sink_preamp_db;
    gtk_range_set_value(GTK_RANGE(pw->scale_preamp), preamp);
    if (pw->spin_preamp) gtk_spin_button_set_value(GTK_SPIN_BUTTON(pw->spin_preamp), preamp);
    if (pw->chk_spectrum) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_spectrum), app->cfg.spectrum_enabled);

    if (!pw->is_microphone) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_auto_preamp), app->cfg.sink_auto_preamp);
        if (pw->chk_split_lr) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_split_lr), app->cfg.sink_eq_split_channels);
        }
        if (pw->combo_lr_chan) {
            gtk_widget_set_sensitive(pw->combo_lr_chan, app->cfg.sink_eq_split_channels);
            gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_lr_chan), pw->edit_right_channel ? 1 : 0);
        }
    } else {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_noise_gate), app->cfg.source_noise_gate);
        gtk_range_set_value(GTK_RANGE(pw->scale_gate_thresh), app->cfg.source_gate_threshold_db);
        if (pw->chk_denoiser)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_denoiser), app->cfg.source_denoiser);
        if (pw->scale_denoise)
            gtk_range_set_value(GTK_RANGE(pw->scale_denoise), app->cfg.source_denoise_strength * 100.0f);
        if (pw->chk_compressor)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_compressor), app->cfg.source_compressor);
        if (pw->scale_comp)
            gtk_range_set_value(GTK_RANGE(pw->scale_comp), app->cfg.source_comp_amount * 100.0f);
    }

    rebuild_faders(pw);
    update_inspector(pw);

    /* Crystalizer */
    bool cryst_on = pw->is_microphone ? !app->cfg.source_crystal_bypass : !app->cfg.sink_crystal_bypass;
    gtk_switch_set_active(GTK_SWITCH(pw->sw_cryst), cryst_on);

    bool cryst_adapt = pw->is_microphone ? app->cfg.source_crystal_adaptive : app->cfg.sink_crystal_adaptive;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->chk_cryst_adaptive), cryst_adapt);

    CrystalBandConfig *cbands = get_active_cryst_bands(pw);
    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        gtk_range_set_value(GTK_RANGE(pw->cryst_scales[b]), cbands[b].intensity_db);
        char txt[16];
        snprintf(txt, sizeof(txt), "%+.1f", cbands[b].intensity_db);
        gtk_label_set_text(GTK_LABEL(pw->cryst_val_labels[b]), txt);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->cryst_btn_bypass[b]), cbands[b].bypass);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pw->cryst_btn_mute[b]), cbands[b].mute);
    }

    reload_presets_combo(pw);
    pw->app->updating_ui = false;
    gtk_widget_queue_draw(pw->bode_area);
}

static void update_streams_list(PipelineWin *pw) {
    if (!pw->streams_listbox || !pw->app->shm) return;
    GuiApp *app = pw->app;
    GList *children = gtk_container_get_children(GTK_CONTAINER(pw->streams_listbox));
    for (GList *it = children; it != NULL; it = it->next) {
        gtk_widget_destroy(GTK_WIDGET(it->data));
    }
    g_list_free(children);

    if (app->shm->stream_count == 0) {
        GtkWidget *lbl = gtk_label_new(tr(app,
            "No active audio playback streams detected.",
            "Активних потоків відтворення аудіо не виявлено."));
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "dim-label");
        gtk_container_add(GTK_CONTAINER(pw->streams_listbox), lbl);
    } else {
        for (uint32_t i = 0; i < app->shm->stream_count; ++i) {
            const LiteFxStreamInfo *st = &app->shm->streams[i];
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            char info[256];
            snprintf(info, sizeof(info), "%s%s%s",
                     st->app_name[0] ? st->app_name : (st->binary[0] ? st->binary : "Audio Stream"),
                     st->media_name[0] ? " — " : "",
                     st->media_name[0] ? st->media_name : "");
            GtkWidget *lbl_app = gtk_label_new(info);
            gtk_label_set_ellipsize(GTK_LABEL(lbl_app), PANGO_ELLIPSIZE_END);
            gtk_widget_set_halign(lbl_app, GTK_ALIGN_START);
            gtk_widget_set_hexpand(lbl_app, TRUE);

            const char *badge_txt = st->blocklisted ? tr(app, "BLOCKLISTED", "ЗАБЛОКОВАНО") :
                                    (st->routed_to_litefx ? "LiteFX DSP" : tr(app, "Bypassed", "В обхід"));
            GtkWidget *lbl_st = gtk_label_new(badge_txt);
            gtk_style_context_add_class(gtk_widget_get_style_context(lbl_st), "band-val");

            gtk_box_pack_start(GTK_BOX(row_box), lbl_app, TRUE, TRUE, 0);
            gtk_box_pack_end(GTK_BOX(row_box), lbl_st, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(pw->streams_listbox), row_box);
        }
    }
    gtk_widget_show_all(pw->streams_listbox);
}

static GtkWidget *build_presets_tab(PipelineWin *pw) {
    GuiApp *app = pw->app;
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    /* Card 1: Create / Save / Import / Export User Profiles */
    GtkWidget *card_pr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_pr), "card");

    pw->lbl_pr_title = gtk_label_new(tr(app, "Profile:", "Профіль:"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_pr_title), "section-title");

    pw->combo_presets = gtk_combo_box_text_new();
    gtk_widget_set_size_request(pw->combo_presets, 135, -1);

    pw->btn_pr_load = gtk_button_new_with_label(tr(app, "Load", "Завантажити"));
    g_signal_connect(pw->btn_pr_load, "clicked", G_CALLBACK(on_load_preset_clicked), pw);

    pw->btn_pr_del = gtk_button_new_with_label(tr(app, "Delete", "Видалити"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_pr_del), "danger");
    g_signal_connect(pw->btn_pr_del, "clicked", G_CALLBACK(on_delete_preset_clicked), pw);

    GtkWidget *sep_pr = gtk_separator_new(GTK_ORIENTATION_VERTICAL);

    pw->entry_preset_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pw->entry_preset_name),
        tr(app, "New profile name (e.g. Music, Gaming, Movie)...",
           "Назва нового профілю (напр. Музика, Ігри, Кіно)..."));
    gtk_widget_set_hexpand(pw->entry_preset_name, TRUE);
    g_signal_connect(pw->entry_preset_name, "activate", G_CALLBACK(on_save_preset_clicked), pw);

    pw->btn_pr_save = gtk_button_new_with_label(tr(app, "💾 Save Profile", "💾 Зберегти профіль"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_pr_save), "accent");
    g_signal_connect(pw->btn_pr_save, "clicked", G_CALLBACK(on_save_preset_clicked), pw);

    pw->btn_pr_imp = gtk_button_new_with_label(tr(app, "📂 Import", "📂 Імпорт"));
    g_signal_connect(pw->btn_pr_imp, "clicked", G_CALLBACK(on_import_profile_file_clicked), pw);

    pw->btn_pr_exp = gtk_button_new_with_label(tr(app, "📤 Export", "📤 Експорт"));
    g_signal_connect(pw->btn_pr_exp, "clicked", G_CALLBACK(on_export_profile_clicked), pw);

    pw->btn_pr_autoeq = gtk_button_new_with_label(tr(app, "📥 AutoEQ (.txt)", "📥 AutoEQ (.txt)"));
    g_signal_connect(pw->btn_pr_autoeq, "clicked", G_CALLBACK(on_import_autoeq_clicked), pw);

    pw->btn_pr_ee = gtk_button_new_with_label("EasyEffects");
    gtk_widget_set_tooltip_text(pw->btn_pr_ee, "Import active presets from ~/.config/easyeffects/db/");
    g_signal_connect(pw->btn_pr_ee, "clicked", G_CALLBACK(on_import_ee_clicked), pw);

    pw->btn_pr_reset = gtk_button_new_with_label(tr(app, "↺ Defaults", "↺ Стандартні"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_pr_reset), "warning-btn");
    g_signal_connect(pw->btn_pr_reset, "clicked", G_CALLBACK(on_reset_defaults_clicked), pw);

    gtk_box_pack_start(GTK_BOX(card_pr), pw->lbl_pr_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->combo_presets, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->btn_pr_load, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->btn_pr_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), sep_pr, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->entry_preset_name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->btn_pr_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->btn_pr_imp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->btn_pr_exp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_pr), pw->btn_pr_autoeq, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(card_pr), pw->btn_pr_reset, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(card_pr), pw->btn_pr_ee, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(vbox), card_pr, FALSE, FALSE, 0);

    /* Card 2: Interactive Saved Profiles List */
    pw->lbl_prof_list = gtk_label_new(tr(app,
        "Saved Profiles (~/.config/litefx/presets/)",
        "Мої збережені профілі (~/.config/litefx/presets/)"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_prof_list), "section-title");
    gtk_widget_set_halign(pw->lbl_prof_list, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), pw->lbl_prof_list, FALSE, FALSE, 0);

    GtkWidget *prof_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(prof_scroll, -1, 105);
    pw->profiles_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(pw->profiles_listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(prof_scroll), pw->profiles_listbox);
    gtk_box_pack_start(GTK_BOX(vbox), prof_scroll, FALSE, FALSE, 0);

    /* Card 3: Per-Device Auto-Profile Binding */
    GtkWidget *dev_prof_hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    pw->lbl_dev_prof_title = gtk_label_new(tr(app,
        "🎧 Per-Device Auto-Profile (auto-loads profile when headphones/speakers connect):",
        "🎧 Авто-профіль для пристроїв (автоматично вмикає профіль при підключенні навушників/колонок):"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_dev_prof_title), "section-title");
    gtk_widget_set_halign(pw->lbl_dev_prof_title, GTK_ALIGN_START);

    pw->btn_bind_dev_prof = gtk_button_new_with_label(tr(app,
        "🔗 Bind Active Profile to Current Device",
        "🔗 Прив'язати профіль до поточного пристрою"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_bind_dev_prof), "accent");
    g_signal_connect(pw->btn_bind_dev_prof, "clicked", G_CALLBACK(on_bind_device_profile_clicked), pw);

    gtk_box_pack_start(GTK_BOX(dev_prof_hbar), pw->lbl_dev_prof_title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(dev_prof_hbar), pw->btn_bind_dev_prof, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), dev_prof_hbar, FALSE, FALSE, 0);

    GtkWidget *dev_prof_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(dev_prof_scroll, -1, 68);
    pw->dev_profiles_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(pw->dev_profiles_listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(dev_prof_scroll), pw->dev_profiles_listbox);
    gtk_box_pack_start(GTK_BOX(vbox), dev_prof_scroll, FALSE, FALSE, 0);

    /* Card 4: System Autostart Toggle */
    GtkWidget *card_auto = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(card_auto), "card");
    pw->lbl_autostart = gtk_label_new(tr(app,
        "🚀 Start background DSP daemon (litefxd) automatically on login (systemd + XDG Autostart):",
        "🚀 Автозапуск фонового аудіо-процесора (litefxd) разом із системою (systemd + XDG Autostart):"));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_autostart), "section-title");
    pw->sw_autostart = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(pw->sw_autostart), is_autostart_enabled());
    gtk_widget_set_valign(pw->sw_autostart, GTK_ALIGN_CENTER);
    g_signal_connect(pw->sw_autostart, "state-set", G_CALLBACK(on_autostart_switch_set), pw);
    gtk_box_pack_start(GTK_BOX(card_auto), pw->lbl_autostart, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card_auto), pw->sw_autostart, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), card_auto, FALSE, FALSE, 0);

    /* If speakers window, add Blocklist & Active Streams list */
    if (!pw->is_microphone) {
        GtkWidget *card_rt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(card_rt), "card");

        pw->lbl_blocklist = gtk_label_new(tr(app, "App Blocklist (CSV):", "Чорний список програм (CSV):"));
        gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_blocklist), "dim-label");

        pw->entry_blocklist = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(pw->entry_blocklist), "e.g. mpv, vlc");
        gtk_widget_set_hexpand(pw->entry_blocklist, TRUE);

        char bl_csv[512] = {0};
        for (uint32_t i = 0; i < app->cfg.sink_blocklist_count; ++i) {
            if (i > 0) strncat(bl_csv, ", ", sizeof(bl_csv) - strlen(bl_csv) - 1);
            strncat(bl_csv, app->cfg.sink_blocklist[i], sizeof(bl_csv) - strlen(bl_csv) - 1);
        }
        gtk_entry_set_text(GTK_ENTRY(pw->entry_blocklist), bl_csv);

        pw->btn_blocklist = gtk_button_new_with_label(tr(app, "Apply Blocklist", "Застосувати список"));
        g_signal_connect(pw->btn_blocklist, "clicked", G_CALLBACK(on_apply_blocklist_clicked), pw);

        gtk_box_pack_start(GTK_BOX(card_rt), pw->lbl_blocklist, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card_rt), pw->entry_blocklist, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(card_rt), pw->btn_blocklist, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), card_rt, FALSE, FALSE, 0);

        pw->lbl_streams_title = gtk_label_new(tr(app,
            "Active Audio Playback Streams", "Активні потоки відтворення аудіо"));
        gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_streams_title), "section-title");
        gtk_widget_set_halign(pw->lbl_streams_title, GTK_ALIGN_START);

        GtkWidget *st_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_widget_set_vexpand(st_scroll, TRUE);
        pw->streams_listbox = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(pw->streams_listbox), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(st_scroll), pw->streams_listbox);

        gtk_box_pack_start(GTK_BOX(vbox), pw->lbl_streams_title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), st_scroll, TRUE, TRUE, 0);
    }

    pw->lbl_ram_note = gtk_label_new(tr(app,
        "Tip: Closing this window frees 100% of GUI memory (0 MB) while litefxd continues processing audio in the background (~2.5 MB RAM).",
        "Порада: Закриття вікна звільняє 100% пам'яті GUI (0 МБ), а litefxd продовжує обробляти звук у фоні (~2.5 МБ ОЗП)."));
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_ram_note), "dim-label");
    gtk_widget_set_halign(pw->lbl_ram_note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), pw->lbl_ram_note, FALSE, FALSE, 0);

    reload_presets_combo(pw);
    return vbox;
}

static GtkWidget *build_pipeline_window(PipelineWin *pw, bool is_mic) {
    GuiApp *app = pw->app;
    pw->is_microphone = is_mic;
    pw->dragging_eq_band = -1;

    pw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(pw->window), 1120, 650);
    gtk_window_set_icon_name(GTK_WINDOW(pw->window), "litefx");
    g_signal_connect(pw->window, "delete-event", G_CALLBACK(on_window_delete), pw);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(pw->window), root);

    /* Master Top Bar */
    GtkWidget *topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(topbar), "topbar");

    pw->lbl_brand = gtk_label_new(is_mic ? "LiteFX Mic" : "LiteFX Output");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_brand), "section-title");

    pw->sw_bypass = gtk_switch_new();
    gtk_widget_set_valign(pw->sw_bypass, GTK_ALIGN_CENTER);
    g_signal_connect(pw->sw_bypass, "state-set", G_CALLBACK(on_global_bypass_set), pw);

    pw->chk_limiter = gtk_check_button_new_with_label(is_mic ? "Voice Limiter" : "Soft Limiter");
    gtk_widget_set_valign(pw->chk_limiter, GTK_ALIGN_CENTER);
    g_signal_connect(pw->chk_limiter, "toggled", G_CALLBACK(on_limiter_toggled), pw);

    /* Device Mode Switcher Buttons */
    GtkWidget *sw_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    pw->btn_sw_speakers = gtk_button_new_with_label("🎧 Speakers");
    pw->btn_sw_mic = gtk_button_new_with_label("🎤 Microphone");
    if (!is_mic) {
        gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_sw_speakers), "accent");
        gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_sw_mic), "win-switch");
    } else {
        gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_sw_speakers), "win-switch");
        gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_sw_mic), "accent");
    }
    g_signal_connect(pw->btn_sw_speakers, "clicked", G_CALLBACK(on_open_other_window), &app->win_sink);
    g_signal_connect(pw->btn_sw_mic, "clicked", G_CALLBACK(on_open_other_window), &app->win_source);
    gtk_box_pack_start(GTK_BOX(sw_box), pw->btn_sw_speakers, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sw_box), pw->btn_sw_mic, FALSE, FALSE, 0);

    pw->lbl_dev = gtk_label_new(is_mic ? "Mic:" : "Device:");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_dev), "dim-label");
    pw->combo_dev = gtk_combo_box_text_new();
    gtk_widget_set_size_request(pw->combo_dev, 180, -1);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(pw->combo_dev));
    if (cells) {
        g_object_set(G_OBJECT(cells->data), "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        g_list_free(cells);
    }
    g_signal_connect(pw->combo_dev, "changed", G_CALLBACK(on_device_combo_changed), pw);

    pw->lbl_top_prof = gtk_label_new("Profile:");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_top_prof), "dim-label");
    pw->combo_top_profile = gtk_combo_box_text_new();
    gtk_widget_set_size_request(pw->combo_top_profile, 135, -1);
    g_signal_connect(pw->combo_top_profile, "changed", G_CALLBACK(on_top_profile_combo_changed), pw);

    pw->btn_top_save = gtk_button_new_with_label("💾");
    g_signal_connect(pw->btn_top_save, "clicked", G_CALLBACK(on_quick_save_top_profile_clicked), pw);

    pw->meter_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(pw->meter_area, 90, 18);
    g_signal_connect(pw->meter_area, "draw", G_CALLBACK(on_meter_draw), pw);

    pw->btn_lang = gtk_button_new_with_label("🌐 EN");
    g_signal_connect(pw->btn_lang, "clicked", G_CALLBACK(on_lang_toggle_clicked), pw);

    pw->btn_stop = gtk_button_new_with_label("Stop DSP & Quit");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_stop), "danger");
    g_signal_connect(pw->btn_stop, "clicked", G_CALLBACK(on_stop_daemon_quit), pw);

    pw->btn_reset_top = gtk_button_new_with_label("↺ Reset");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->btn_reset_top), "warning-btn");
    g_signal_connect(pw->btn_reset_top, "clicked", G_CALLBACK(on_reset_defaults_clicked), pw);

    gtk_box_pack_start(GTK_BOX(topbar), pw->lbl_brand, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->sw_bypass, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->chk_limiter, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(topbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), sw_box, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->lbl_dev, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->combo_dev, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->lbl_top_prof, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->combo_top_profile, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(topbar), pw->btn_top_save, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(topbar), pw->btn_stop, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(topbar), pw->btn_reset_top, FALSE, FALSE, 2);
    gtk_box_pack_end(GTK_BOX(topbar), pw->btn_lang, FALSE, FALSE, 2);
    gtk_box_pack_end(GTK_BOX(topbar), pw->meter_area, FALSE, FALSE, 2);

    /* Main Tabs */
    pw->notebook = gtk_notebook_new();

    /* Tab 1: Equalizer */
    GtkWidget *eq_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(eq_box), 10);

    GtkWidget *eq_hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    pw->lbl_eq_title = gtk_label_new("Equalizer");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_eq_title), "section-title");
    pw->sw_eq = gtk_switch_new();
    gtk_widget_set_valign(pw->sw_eq, GTK_ALIGN_CENTER);
    g_signal_connect(pw->sw_eq, "state-set", G_CALLBACK(on_eq_bypass_set), pw);

    pw->lbl_preamp = gtk_label_new(is_mic ? "Mic Gain:" : "Preamp:");
    gtk_widget_set_valign(pw->lbl_preamp, GTK_ALIGN_CENTER);
    pw->scale_preamp = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -18.0, 18.0, 0.5);
    gtk_widget_set_valign(pw->scale_preamp, GTK_ALIGN_CENTER);
    gtk_scale_set_draw_value(GTK_SCALE(pw->scale_preamp), FALSE);
    gtk_widget_set_size_request(pw->scale_preamp, 75, -1);
    g_signal_connect(pw->scale_preamp, "value-changed", G_CALLBACK(on_preamp_changed), pw);

    pw->spin_preamp = gtk_spin_button_new_with_range(-18.0, 18.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(pw->spin_preamp), 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(pw->spin_preamp), TRUE);
    gtk_widget_set_valign(pw->spin_preamp, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(pw->spin_preamp, 64, -1);
    g_signal_connect(pw->spin_preamp, "value-changed", G_CALLBACK(on_preamp_spin_changed), pw);

    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->lbl_eq_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->sw_eq, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(eq_hbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->lbl_preamp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->scale_preamp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->spin_preamp, FALSE, FALSE, 0);

    if (is_mic) {
        pw->chk_noise_gate = gtk_check_button_new_with_label("Gate");
        gtk_widget_set_valign(pw->chk_noise_gate, GTK_ALIGN_CENTER);
        g_signal_connect(pw->chk_noise_gate, "toggled", G_CALLBACK(on_noise_gate_toggled), pw);

        pw->lbl_gate_thresh = gtk_label_new("Thresh:");
        gtk_widget_set_valign(pw->lbl_gate_thresh, GTK_ALIGN_CENTER);
        pw->scale_gate_thresh = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -65.0, -15.0, 1.0);
        gtk_widget_set_valign(pw->scale_gate_thresh, GTK_ALIGN_CENTER);
        gtk_scale_set_draw_value(GTK_SCALE(pw->scale_gate_thresh), FALSE);
        gtk_widget_set_size_request(pw->scale_gate_thresh, 65, -1);
        g_signal_connect(pw->scale_gate_thresh, "value-changed", G_CALLBACK(on_gate_thresh_changed), pw);

        pw->chk_denoiser = gtk_check_button_new_with_label("Denoise");
        gtk_widget_set_valign(pw->chk_denoiser, GTK_ALIGN_CENTER);
        g_signal_connect(pw->chk_denoiser, "toggled", G_CALLBACK(on_denoiser_toggled), pw);

        pw->scale_denoise = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 5.0);
        gtk_widget_set_valign(pw->scale_denoise, GTK_ALIGN_CENTER);
        gtk_scale_set_draw_value(GTK_SCALE(pw->scale_denoise), FALSE);
        gtk_widget_set_size_request(pw->scale_denoise, 60, -1);
        g_signal_connect(pw->scale_denoise, "value-changed", G_CALLBACK(on_denoise_strength_changed), pw);

        pw->chk_compressor = gtk_check_button_new_with_label("Compressor");
        gtk_widget_set_valign(pw->chk_compressor, GTK_ALIGN_CENTER);
        g_signal_connect(pw->chk_compressor, "toggled", G_CALLBACK(on_compressor_toggled), pw);

        pw->scale_comp = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 5.0);
        gtk_widget_set_valign(pw->scale_comp, GTK_ALIGN_CENTER);
        gtk_scale_set_draw_value(GTK_SCALE(pw->scale_comp), FALSE);
        gtk_widget_set_size_request(pw->scale_comp, 60, -1);
        g_signal_connect(pw->scale_comp, "value-changed", G_CALLBACK(on_comp_amount_changed), pw);

        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->chk_noise_gate, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->lbl_gate_thresh, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->scale_gate_thresh, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->chk_denoiser, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->scale_denoise, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->chk_compressor, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->scale_comp, FALSE, FALSE, 0);
    } else {
        pw->chk_auto_preamp = gtk_check_button_new_with_label("Auto-Preamp");
        gtk_widget_set_valign(pw->chk_auto_preamp, GTK_ALIGN_CENTER);
        g_signal_connect(pw->chk_auto_preamp, "toggled", G_CALLBACK(on_auto_preamp_toggled), pw);

        pw->chk_split_lr = gtk_check_button_new_with_label("Split L/R");
        gtk_widget_set_valign(pw->chk_split_lr, GTK_ALIGN_CENTER);
        g_signal_connect(pw->chk_split_lr, "toggled", G_CALLBACK(on_split_lr_toggled), pw);

        pw->combo_lr_chan = gtk_combo_box_text_new();
        gtk_widget_set_valign(pw->combo_lr_chan, GTK_ALIGN_CENTER);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_lr_chan), "Left");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_lr_chan), "Right");
        gtk_combo_box_set_active(GTK_COMBO_BOX(pw->combo_lr_chan), pw->edit_right_channel ? 1 : 0);
        gtk_widget_set_sensitive(pw->combo_lr_chan, app->cfg.sink_eq_split_channels);
        g_signal_connect(pw->combo_lr_chan, "changed", G_CALLBACK(on_lr_chan_changed), pw);

        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->chk_auto_preamp, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->chk_split_lr, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(eq_hbar), pw->combo_lr_chan, FALSE, FALSE, 2);
    }

    pw->chk_spectrum = gtk_check_button_new_with_label("Spectrum");
    gtk_widget_set_valign(pw->chk_spectrum, GTK_ALIGN_CENTER);
    g_signal_connect(pw->chk_spectrum, "toggled", G_CALLBACK(on_spectrum_toggled), pw);
    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->chk_spectrum, FALSE, FALSE, 2);

    pw->btn_autoeq = gtk_button_new_with_label("📥 AutoEQ");
    g_signal_connect(pw->btn_autoeq, "clicked", G_CALLBACK(on_import_autoeq_clicked), pw);
    gtk_box_pack_start(GTK_BOX(eq_hbar), pw->btn_autoeq, FALSE, FALSE, 2);

    GtkWidget *sep_pr = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(eq_hbar), sep_pr, FALSE, FALSE, 2);

    const char *qnames[5];
    if (!is_mic) {
        static const char *s_names[5] = { "Flat", "V-Shape", "Bass+", "Vocal", "Warm" };
        for (int i = 0; i < 5; ++i) qnames[i] = s_names[i];
    } else {
        static const char *m_names[5] = { "Flat", "Vocal", "Low-Cut", "Presence", "Warm" };
        for (int i = 0; i < 5; ++i) qnames[i] = m_names[i];
    }

    for (int i = 0; i < 5; ++i) {
        GtkWidget *qb = gtk_button_new_with_label(qnames[i]);
        g_object_set_data(G_OBJECT(qb), "curve_id", GINT_TO_POINTER(i));
        g_signal_connect(qb, "clicked", G_CALLBACK(on_eq_quick_preset_clicked), pw);
        gtk_box_pack_start(GTK_BOX(eq_hbar), qb, FALSE, FALSE, 1);
    }

    pw->bode_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(pw->bode_area, 480, 200);
    gtk_widget_set_vexpand(pw->bode_area, TRUE);
    gtk_widget_add_events(pw->bode_area,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(pw->bode_area, "draw", G_CALLBACK(on_bode_draw), pw);
    g_signal_connect(pw->bode_area, "button-press-event", G_CALLBACK(on_bode_button_press), pw);
    g_signal_connect(pw->bode_area, "button-release-event", G_CALLBACK(on_bode_button_release), pw);
    g_signal_connect(pw->bode_area, "motion-notify-event", G_CALLBACK(on_bode_motion), pw);
    g_signal_connect(pw->bode_area, "scroll-event", G_CALLBACK(on_bode_scroll), pw);

    GtkWidget *insp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(insp), "card");

    pw->lbl_sel_band = gtk_label_new("Band #1");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_sel_band), "section-title");
    pw->chk_band_enabled = gtk_check_button_new_with_label("On");
    g_signal_connect(pw->chk_band_enabled, "toggled", G_CALLBACK(on_inspector_changed), pw);

    pw->combo_band_type = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_band_type), "Peak / Bell");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_band_type), "Low Shelf");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_band_type), "High Shelf");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_band_type), "Low Pass");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_band_type), "High Pass");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pw->combo_band_type), "Notch");
    g_signal_connect(pw->combo_band_type, "changed", G_CALLBACK(on_inspector_changed), pw);

    pw->lbl_insp_freq = gtk_label_new("Freq:");
    pw->spin_band_freq = gtk_spin_button_new_with_range(20.0, 20000.0, 5.0);
    g_signal_connect(pw->spin_band_freq, "value-changed", G_CALLBACK(on_inspector_changed), pw);
    pw->lbl_insp_gain = gtk_label_new("Gain:");
    pw->spin_band_gain = gtk_spin_button_new_with_range(-24.0, 24.0, 0.25);
    g_signal_connect(pw->spin_band_gain, "value-changed", G_CALLBACK(on_inspector_changed), pw);
    pw->lbl_insp_q = gtk_label_new("Q:");
    pw->spin_band_q = gtk_spin_button_new_with_range(0.1, 20.0, 0.05);
    g_signal_connect(pw->spin_band_q, "value-changed", G_CALLBACK(on_inspector_changed), pw);

    gtk_box_pack_start(GTK_BOX(insp), pw->lbl_sel_band, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(insp), pw->chk_band_enabled, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(insp), pw->combo_band_type, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(insp), pw->lbl_insp_freq, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(insp), pw->spin_band_freq, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(insp), pw->lbl_insp_gain, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(insp), pw->spin_band_gain, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(insp), pw->lbl_insp_q, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(insp), pw->spin_band_q, FALSE, FALSE, 0);

    GtkWidget *fscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_size_request(fscroll, -1, 138);
    pw->eq_faders_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->eq_faders_box), "card");
    gtk_container_add(GTK_CONTAINER(fscroll), pw->eq_faders_box);

    gtk_box_pack_start(GTK_BOX(eq_box), eq_hbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(eq_box), pw->bode_area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(eq_box), insp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(eq_box), fscroll, FALSE, FALSE, 0);

    /* Tab 2: Crystalizer */
    GtkWidget *cr_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(cr_box), 10);

    GtkWidget *cr_hbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    pw->lbl_cryst_title = gtk_label_new("13-Band Crystalizer (Dynamic Transient Enhancer)");
    gtk_style_context_add_class(gtk_widget_get_style_context(pw->lbl_cryst_title), "section-title");

    pw->sw_cryst = gtk_switch_new();
    gtk_widget_set_valign(pw->sw_cryst, GTK_ALIGN_CENTER);
    g_signal_connect(pw->sw_cryst, "state-set", G_CALLBACK(on_cryst_bypass_set), pw);

    pw->chk_cryst_adaptive = gtk_check_button_new_with_label("Adaptive Intensity");
    gtk_widget_set_valign(pw->chk_cryst_adaptive, GTK_ALIGN_CENTER);
    g_signal_connect(pw->chk_cryst_adaptive, "toggled", G_CALLBACK(on_cryst_adaptive_toggled), pw);

    gtk_box_pack_start(GTK_BOX(cr_hbar), pw->lbl_cryst_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cr_hbar), pw->sw_cryst, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cr_hbar), pw->chk_cryst_adaptive, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(cr_hbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);

    static const char *cr_qnames[5] = { "EasyEffects", "Balanced", "Crisp", "Punchy", "Subtle" };
    for (int i = 0; i < 5; ++i) {
        GtkWidget *qb = gtk_button_new_with_label(cr_qnames[i]);
        g_object_set_data(G_OBJECT(qb), "cryst_curve", GINT_TO_POINTER(i));
        g_signal_connect(qb, "clicked", G_CALLBACK(on_cryst_quick_preset_clicked), pw);
        gtk_box_pack_start(GTK_BOX(cr_hbar), qb, FALSE, FALSE, 2);
    }

    GtkWidget *rack = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(rack), "card");
    gtk_widget_set_vexpand(rack, TRUE);

    for (int b = 0; b < LITEFX_CRYSTAL_BANDS; ++b) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(col, TRUE);

        GtkWidget *vlbl = gtk_label_new("0.0");
        gtk_style_context_add_class(gtk_widget_get_style_context(vlbl), "band-val");

        GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -36.0, 15.0, 0.5);
        gtk_range_set_inverted(GTK_RANGE(sc), TRUE);
        gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
        gtk_scale_add_mark(GTK_SCALE(sc), 0.0, GTK_POS_RIGHT, NULL);
        gtk_widget_set_vexpand(sc, TRUE);
        gtk_widget_set_size_request(sc, 32, 220);
        g_object_set_data(G_OBJECT(sc), "band_idx", GINT_TO_POINTER(b));
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_cryst_band_scale_changed), pw);

        GtkWidget *flbl = gtk_label_new(LITEFX_CRYSTAL_LABELS[b]);
        gtk_style_context_add_class(gtk_widget_get_style_context(flbl), "section-title");

        GtkWidget *bm_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_halign(bm_row, GTK_ALIGN_CENTER);

        GtkWidget *btn_b = gtk_toggle_button_new_with_label("B");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_b), "bypass-btn");
        g_object_set_data(G_OBJECT(btn_b), "band_idx", GINT_TO_POINTER(b));
        g_signal_connect(btn_b, "toggled", G_CALLBACK(on_cryst_band_bypass_toggled), pw);

        GtkWidget *btn_m = gtk_toggle_button_new_with_label("M");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_m), "mute-btn");
        g_object_set_data(G_OBJECT(btn_m), "band_idx", GINT_TO_POINTER(b));
        g_signal_connect(btn_m, "toggled", G_CALLBACK(on_cryst_band_mute_toggled), pw);

        gtk_box_pack_start(GTK_BOX(bm_row), btn_b, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bm_row), btn_m, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(col), vlbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(col), sc, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(col), flbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(col), bm_row, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rack), col, TRUE, TRUE, 0);

        pw->cryst_scales[b] = sc;
        pw->cryst_val_labels[b] = vlbl;
        pw->cryst_btn_bypass[b] = btn_b;
        pw->cryst_btn_mute[b] = btn_m;
    }

    gtk_box_pack_start(GTK_BOX(cr_box), cr_hbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cr_box), rack, TRUE, TRUE, 0);

    pw->tab_lbl_eq = gtk_label_new("Equalizer");
    pw->tab_lbl_cryst = gtk_label_new("Crystalizer (13-Band)");
    pw->tab_lbl_presets = gtk_label_new(is_mic ? "Profiles & Autostart" : "Profiles & Routing");

    gtk_notebook_append_page(GTK_NOTEBOOK(pw->notebook), eq_box, pw->tab_lbl_eq);
    gtk_notebook_append_page(GTK_NOTEBOOK(pw->notebook), cr_box, pw->tab_lbl_cryst);
    gtk_notebook_append_page(GTK_NOTEBOOK(pw->notebook), build_presets_tab(pw), pw->tab_lbl_presets);

    /* Bottom status */
    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(sbar), "statusbar");
    pw->lbl_status = gtk_label_new("Connecting to litefxd...");
    gtk_widget_set_halign(pw->lbl_status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sbar), pw->lbl_status, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(root), topbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), pw->notebook, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), sbar, FALSE, FALSE, 0);

    update_window_language(pw);
    return pw->window;
}

static gboolean on_telemetry_tick(gpointer data) {
    GuiApp *app = (GuiApp *)data;
    if (!app->shm) connect_shm(app);

    if (app->shm) {
        atomic_store_explicit(&app->shm->gui_heartbeat_ns, litefx_now_ns(), memory_order_relaxed);
        uint32_t seq = atomic_load_explicit(&app->shm->config_seq, memory_order_acquire);
        if (seq != app->last_seen_seq) {
            app->last_seen_seq = seq;
            app->cfg = app->shm->config;
            update_window_language(&app->win_sink);
            update_window_language(&app->win_source);
            refresh_window_ui(&app->win_sink);
            refresh_window_ui(&app->win_source);
        }

        uint32_t sr = atomic_load_explicit(&app->shm->sample_rate, memory_order_relaxed);
        uint32_t qf = atomic_load_explicit(&app->shm->quantum_frames, memory_order_relaxed);
        bool sink_stream = atomic_load_explicit(&app->shm->sink_dsp_streaming, memory_order_relaxed);
        bool src_stream = atomic_load_explicit(&app->shm->source_dsp_streaming, memory_order_relaxed);

        char st_sink[256];
        snprintf(st_sink, sizeof(st_sink),
                 "litefxd PID %d | %s: %s | %u Hz / %uf | %s: %s",
                 app->shm->daemon_pid,
                 tr(app, "Speakers", "Динаміки"),
                 sink_stream ? tr(app, "STREAMING", "АКТИВНО") : tr(app, "Idle", "Очікування"),
                 sr, qf,
                 tr(app, "Target", "Ціль"),
                 app->shm->resolved_sink_desc[0] ? app->shm->resolved_sink_desc : "Auto");
        gtk_label_set_text(GTK_LABEL(app->win_sink.lbl_status), st_sink);
        gtk_widget_queue_draw(app->win_sink.meter_area);
        if (app->cfg.spectrum_enabled && sink_stream && gtk_widget_get_visible(app->win_sink.window)) {
            gtk_widget_queue_draw(app->win_sink.bode_area);
        }

        char st_src[256];
        snprintf(st_src, sizeof(st_src),
                 "litefxd PID %d | %s: %s | %u Hz / %uf | %s: %s",
                 app->shm->daemon_pid,
                 tr(app, "Mic", "Мікрофон"),
                 src_stream ? tr(app, "STREAMING", "АКТИВНО") : tr(app, "Idle", "Очікування"),
                 sr, qf,
                 tr(app, "Target", "Ціль"),
                 app->shm->resolved_source_desc[0] ? app->shm->resolved_source_desc : "Auto");
        gtk_label_set_text(GTK_LABEL(app->win_source.lbl_status), st_src);
        gtk_widget_queue_draw(app->win_source.meter_area);
        if (app->cfg.spectrum_enabled && src_stream && gtk_widget_get_visible(app->win_source.window)) {
            gtk_widget_queue_draw(app->win_source.bode_area);
        }

        static int stream_tick = 0;
        if ((++stream_tick % 10) == 0) {
            update_streams_list(&app->win_sink);
        }
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    bool open_mic = false;
    bool open_speakers = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: litefx [OPTIONS]\n"
                   "  --speakers, -s  Open Speakers / Headphones window (default)\n"
                   "  --mic, -m       Open Microphone window\n"
                   "  --both, -b      Open both windows\n"
                   "  --help, -h      Show this help message\n");
            return 0;
        }
        if (strcmp(argv[i], "--mic") == 0 || strcmp(argv[i], "-m") == 0) open_mic = true;
        else if (strcmp(argv[i], "--speakers") == 0 || strcmp(argv[i], "-s") == 0) open_speakers = true;
        else if (strcmp(argv[i], "--both") == 0 || strcmp(argv[i], "-b") == 0) {
            open_mic = true; open_speakers = true;
        }
    }
    if (!open_mic && !open_speakers) open_speakers = true;

    const char *home = getenv("HOME");
    if (home && home[0]) {
        const char *p = getenv("PATH");
        char new_path[2048];
        snprintf(new_path, sizeof(new_path), "%s/.local/bin:%s", home, p ? p : "/usr/bin");
        setenv("PATH", new_path, 1);
    }

    g_set_prgname("litefx");
    g_set_application_name("LiteFX");
    gtk_init(&argc, &argv);

    char user_icon1[512] = {0};
    char user_icon2[512] = {0};
    if (home && home[0]) {
        snprintf(user_icon1, sizeof(user_icon1), "%s/.local/share/icons/hicolor/512x512/apps/litefx.png", home);
        snprintf(user_icon2, sizeof(user_icon2), "%s/.local/share/icons/litefx.png", home);
    }
    const char *icon_paths[] = {
        user_icon1[0] ? user_icon1 : "/usr/share/icons/hicolor/512x512/apps/litefx.png",
        user_icon2[0] ? user_icon2 : "/usr/share/icons/hicolor/512x512/apps/litefx.png",
        "/usr/share/icons/hicolor/512x512/apps/litefx.png",
        "data/litefx.png",
        NULL
    };
    for (int i = 0; icon_paths[i]; ++i) {
        if (access(icon_paths[i], R_OK) == 0) {
            gtk_window_set_default_icon_from_file(icon_paths[i], NULL);
            break;
        }
    }
    gtk_window_set_default_icon_name("litefx");

    GuiApp *app = (GuiApp *)calloc(1, sizeof(GuiApp));
    if (!app) return 1;
    app->shm_fd = -1;
    app->win_sink.app = app;
    app->win_source.app = app;

    ensure_daemon_running(app);

    if (app->shm) {
        app->cfg = app->shm->config;
        app->last_seen_seq = atomic_load_explicit(&app->shm->config_seq, memory_order_acquire);
    } else {
        char cpath[512];
        litefx_get_default_config_path(cpath, sizeof(cpath));
        if (!litefx_config_load(&app->cfg, cpath)) litefx_config_set_defaults(&app->cfg);
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, DARK_STUDIO_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    build_pipeline_window(&app->win_sink, false);
    build_pipeline_window(&app->win_source, true);

    refresh_window_ui(&app->win_sink);
    refresh_window_ui(&app->win_source);

    if (open_speakers) {
        gtk_widget_show_all(app->win_sink.window);
        gtk_window_present(GTK_WINDOW(app->win_sink.window));
    }
    if (open_mic) {
        gtk_widget_show_all(app->win_source.window);
        gtk_window_present(GTK_WINDOW(app->win_source.window));
    }

    on_telemetry_tick(app);
    app->telemetry_timer_id = g_timeout_add(80, on_telemetry_tick, app);

    gtk_main();

    if (app->shm) munmap(app->shm, sizeof(LiteFxShm));
    if (app->shm_fd >= 0) close(app->shm_fd);
    free(app);
    return 0;
}
