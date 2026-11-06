#define _GNU_SOURCE
#include "common.h"
#include "config.h"
#include "dsp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>

#define LITEFX_SINK_NAME         "litefx_sink"
#define LITEFX_SINK_DESC         "LiteFX Sink (Speakers)"
#define LITEFX_SOURCE_NAME       "litefx_source"
#define LITEFX_SOURCE_DESC       "LiteFX Source (Microphone)"
#define LITEFX_DSP_SINK_NAME     "litefx_sink_dsp"
#define LITEFX_DSP_SOURCE_NAME   "litefx_source_dsp"

#define MAX_TRACKED_PORTS 512

typedef struct {
    uint32_t global_id;
    uint32_t node_id;
    int      channel;   /* 0 = FL/MONO, 1 = FR, -1 = other */
    bool     is_input;  /* playback/in = true, capture/monitor/out = false */
    bool     is_monitor;
} TrackedPort;

typedef struct {
    uint32_t id;
    uint32_t serial;
    char     name[LITEFX_NAME_LEN];
    char     desc[LITEFX_DESC_LEN];
} TrackedDev;

typedef struct {
    uint32_t id;
    uint32_t pid;
    char     node_name[64];
    char     app_name[64];
    char     binary[64];
    char     media_name[96];
    bool     routed_by_us;
    bool     is_input;
    bool     is_sink_capture;
} TrackedStream;

typedef struct {
    struct pw_main_loop  *main_loop;
    struct pw_loop       *loop;
    struct pw_context    *context;
    struct pw_core       *core;
    struct spa_hook       core_listener;
    struct pw_registry   *registry;
    struct spa_hook       registry_listener;

    /* Output Pipeline Nodes */
    struct pw_proxy      *virtual_sink_proxy;
    uint32_t              virtual_sink_id;
    uint32_t              virtual_sink_serial;

    struct pw_filter     *sink_filter;
    struct spa_hook       sink_filter_listener;
    void                 *port_sink_in_l;
    void                 *port_sink_in_r;
    void                 *port_sink_out_l;
    void                 *port_sink_out_r;
    uint32_t              sink_dsp_node_id;

    /* Input Pipeline Nodes */
    struct pw_proxy      *virtual_source_proxy;
    uint32_t              virtual_source_id;
    uint32_t              virtual_source_serial;

    struct pw_filter     *source_filter;
    struct spa_hook       source_filter_listener;
    void                 *port_source_in_l;
    void                 *port_source_in_r;
    void                 *port_source_out_l;
    void                 *port_source_out_r;
    uint32_t              source_dsp_node_id;

    /* WirePlumber Metadata */
    struct pw_metadata   *default_meta;
    struct spa_hook       meta_listener;
    uint32_t              default_meta_id;
    char                  system_default_sink[LITEFX_NAME_LEN];
    char                  system_default_source[LITEFX_NAME_LEN];
    bool                  sink_meta_set_by_us;
    bool                  source_meta_set_by_us;

    /* Active PipeWire Link Proxies */
    struct pw_proxy      *link_sink_mon_l;
    struct pw_proxy      *link_sink_mon_r;
    struct pw_proxy      *link_sink_hw_l;
    struct pw_proxy      *link_sink_hw_r;
    uint32_t              linked_hw_sink_id;

    struct pw_proxy      *link_source_hw_l;
    struct pw_proxy      *link_source_hw_r;
    struct pw_proxy      *link_source_play_l;
    struct pw_proxy      *link_source_play_r;
    uint32_t              linked_hw_source_id;

    /* Hardware Devices */
    TrackedDev            sinks[LITEFX_MAX_SINKS];
    uint32_t              sink_count;

    TrackedDev            sources[LITEFX_MAX_SOURCES];
    uint32_t              source_count;

    TrackedStream         streams[LITEFX_MAX_STREAMS];
    uint32_t              stream_count;

    TrackedPort           ports[MAX_TRACKED_PORTS];
    uint32_t              port_count;

    /* DSP Engines & Config */
    LiteFxConfig          cfg;
    char                  cfg_path[512];
    DspEngine             dsp_sink;
    DspEngine             dsp_source;

    /* IPC & SHM */
    int                   shm_fd;
    char                  shm_path[256];
    LiteFxShm            *shm;

    int                   sock_fd;
    char                  sock_path[108];
    struct spa_source    *sock_source;
    struct spa_source    *sigint_source;
    struct spa_source    *sigterm_source;

    bool                  verbose;
    bool                  shutting_down;
    bool                  sink_was_silent;
    bool                  source_was_silent;
    char                  last_auto_sink[LITEFX_NAME_LEN];
    char                  last_auto_source[LITEFX_NAME_LEN];
} DaemonCtx;

static bool is_virtual_or_own(const char *name) {
    if (!name || !name[0]) return true;
    if (strncmp(name, "litefx", 6) == 0) return true;
    if (strncmp(name, "easyeffects", 11) == 0) return true;
    if (strncmp(name, "ee_", 3) == 0) return true;
    return false;
}

static const TrackedDev *resolve_target_sink(const DaemonCtx *ctx) {
    if (ctx->sink_count == 0) return NULL;
    if (ctx->cfg.target_sink[0] && strcmp(ctx->cfg.target_sink, "auto") != 0) {
        for (uint32_t i = 0; i < ctx->sink_count; ++i) {
            if (strcmp(ctx->sinks[i].name, ctx->cfg.target_sink) == 0) return &ctx->sinks[i];
        }
    }
    if (ctx->system_default_sink[0]) {
        for (uint32_t i = 0; i < ctx->sink_count; ++i) {
            if (strcmp(ctx->sinks[i].name, ctx->system_default_sink) == 0) return &ctx->sinks[i];
        }
    }
    return &ctx->sinks[0];
}

static const TrackedDev *resolve_target_source(const DaemonCtx *ctx) {
    if (ctx->source_count == 0) return NULL;
    if (ctx->cfg.target_source[0] && strcmp(ctx->cfg.target_source, "auto") != 0) {
        for (uint32_t i = 0; i < ctx->source_count; ++i) {
            if (strcmp(ctx->sources[i].name, ctx->cfg.target_source) == 0) return &ctx->sources[i];
        }
    }
    if (ctx->system_default_source[0]) {
        for (uint32_t i = 0; i < ctx->source_count; ++i) {
            if (strcmp(ctx->sources[i].name, ctx->system_default_source) == 0) return &ctx->sources[i];
        }
    }
    return &ctx->sources[0];
}

static bool is_stream_blocklisted(const LiteFxConfig *cfg, const TrackedStream *st) {
    for (uint32_t i = 0; i < cfg->sink_blocklist_count; ++i) {
        const char *b = cfg->sink_blocklist[i];
        if (!b[0]) continue;
        if (st->app_name[0] && strcasestr(st->app_name, b) != NULL) return true;
        if (st->binary[0] && strcasestr(st->binary, b) != NULL) return true;
        if (st->node_name[0] && strcasestr(st->node_name, b) != NULL) return true;
    }
    return false;
}

static void maybe_apply_device_profile(DaemonCtx *ctx, const char *dev_name) {
    if (!dev_name || !dev_name[0]) return;
    for (uint32_t i = 0; i < ctx->cfg.dev_profile_count && i < LITEFX_MAX_DEV_PROFILES; ++i) {
        if (strcmp(ctx->cfg.dev_profiles[i].device_name, dev_name) == 0 &&
            ctx->cfg.dev_profiles[i].preset_name[0] != '\0') {
            char ppath[512];
            litefx_get_preset_path(ctx->cfg.dev_profiles[i].preset_name, ppath, sizeof(ppath));
            LiteFxConfig loaded;
            if (litefx_config_load(&loaded, ppath)) {
                litefx_config_apply_preset_only(&ctx->cfg, &loaded);
                uint32_t sr = ctx->shm
                    ? atomic_load_explicit(&ctx->shm->sample_rate, memory_order_relaxed)
                    : 48000u;
                if (sr < 8000u) sr = 48000u;
                dsp_engine_update_sink(&ctx->dsp_sink, &ctx->cfg, sr);
                dsp_engine_update_source(&ctx->dsp_source, &ctx->cfg, sr);
                litefx_config_save(&ctx->cfg, ctx->cfg_path);
            }
            break;
        }
    }
}

static void sync_shm_snapshot(DaemonCtx *ctx) {
    if (!ctx->shm) return;
    LiteFxShm *shm = ctx->shm;

    const TrackedDev *hw_sink = resolve_target_sink(ctx);
    if (hw_sink && hw_sink->name[0] && strcmp(ctx->last_auto_sink, hw_sink->name) != 0) {
        snprintf(ctx->last_auto_sink, sizeof(ctx->last_auto_sink), "%s", hw_sink->name);
        maybe_apply_device_profile(ctx, hw_sink->name);
    }

    const TrackedDev *hw_source = resolve_target_source(ctx);
    if (hw_source && hw_source->name[0] && strcmp(ctx->last_auto_source, hw_source->name) != 0) {
        snprintf(ctx->last_auto_source, sizeof(ctx->last_auto_source), "%s", hw_source->name);
        maybe_apply_device_profile(ctx, hw_source->name);
    }

    shm->config = ctx->cfg;

    shm->resolved_sink_desc[0] = '\0';
    if (hw_sink) {
        snprintf(shm->resolved_sink_desc, sizeof(shm->resolved_sink_desc),
                 "%s", hw_sink->desc[0] ? hw_sink->desc : hw_sink->name);
    }

    shm->resolved_source_desc[0] = '\0';
    if (hw_source) {
        snprintf(shm->resolved_source_desc, sizeof(shm->resolved_source_desc),
                 "%s", hw_source->desc[0] ? hw_source->desc : hw_source->name);
    }

    shm->sink_count = ctx->sink_count;
    for (uint32_t i = 0; i < ctx->sink_count; ++i) {
        LiteFxDeviceInfo *dst = &shm->sinks[i];
        dst->id = ctx->sinks[i].id;
        dst->serial = ctx->sinks[i].serial;
        snprintf(dst->name, sizeof(dst->name), "%s", ctx->sinks[i].name);
        snprintf(dst->description, sizeof(dst->description), "%s",
                 ctx->sinks[i].desc[0] ? ctx->sinks[i].desc : ctx->sinks[i].name);
        dst->is_default = (ctx->system_default_sink[0] &&
                           strcmp(ctx->sinks[i].name, ctx->system_default_sink) == 0);
        dst->is_active_target = (hw_sink == &ctx->sinks[i]);
    }

    shm->source_count = ctx->source_count;
    for (uint32_t i = 0; i < ctx->source_count; ++i) {
        LiteFxDeviceInfo *dst = &shm->sources[i];
        dst->id = ctx->sources[i].id;
        dst->serial = ctx->sources[i].serial;
        snprintf(dst->name, sizeof(dst->name), "%s", ctx->sources[i].name);
        snprintf(dst->description, sizeof(dst->description), "%s",
                 ctx->sources[i].desc[0] ? ctx->sources[i].desc : ctx->sources[i].name);
        dst->is_default = (ctx->system_default_source[0] &&
                           strcmp(ctx->sources[i].name, ctx->system_default_source) == 0);
        dst->is_active_target = (hw_source == &ctx->sources[i]);
    }

    shm->stream_count = ctx->stream_count;
    for (uint32_t i = 0; i < ctx->stream_count; ++i) {
        LiteFxStreamInfo *dst = &shm->streams[i];
        const TrackedStream *src = &ctx->streams[i];
        dst->id = src->id;
        snprintf(dst->app_name, sizeof(dst->app_name), "%s",
                 src->app_name[0] ? src->app_name : src->node_name);
        snprintf(dst->binary, sizeof(dst->binary), "%s", src->binary);
        snprintf(dst->media_name, sizeof(dst->media_name), "%s", src->media_name);
        dst->blocklisted = is_stream_blocklisted(&ctx->cfg, src);
        dst->routed_to_litefx = src->routed_by_us;
    }

    atomic_fetch_add_explicit(&shm->config_seq, 1u, memory_order_release);
}

static uint32_t find_port_id(const DaemonCtx *ctx, uint32_t node_id,
                             bool want_input, bool want_monitor,
                             int channel, bool allow_mono_fallback) {
    if (node_id == 0 || node_id == SPA_ID_INVALID) return 0;
    uint32_t fallback_mono = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->port_count; ++i) {
        const TrackedPort *p = &ctx->ports[i];
        if (p->node_id != node_id) continue;
        if (p->is_input != want_input) continue;
        if (p->is_monitor != want_monitor) continue;
        count++;
        if (p->channel == channel) return p->global_id;
        if (p->channel == 0) fallback_mono = p->global_id;
    }
    if (allow_mono_fallback && count == 1) return fallback_mono;
    return 0;
}

static struct pw_proxy *create_pw_link(DaemonCtx *ctx,
                                       uint32_t out_node, uint32_t out_port,
                                       uint32_t in_node, uint32_t in_port) {
    if (!out_node || !out_port || !in_node || !in_port) return NULL;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_OBJECT_LINGER, "false",
        NULL
    );
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out_node);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_port);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in_node);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in_port);

    struct pw_proxy *proxy = (struct pw_proxy *)pw_core_create_object(
        ctx->core,
        "link-factory",
        PW_TYPE_INTERFACE_Link,
        PW_VERSION_LINK,
        &props->dict,
        0
    );
    pw_properties_free(props);
    return proxy;
}

static void destroy_proxy_safe(struct pw_proxy **p) {
    if (*p) {
        pw_proxy_destroy(*p);
        *p = NULL;
    }
}

static void reconcile_links(DaemonCtx *ctx) {
    if (ctx->shutting_down) return;

    if (ctx->sink_dsp_node_id == 0 && ctx->sink_filter) {
        uint32_t fid = pw_filter_get_node_id(ctx->sink_filter);
        if (fid != SPA_ID_INVALID && fid != 0) ctx->sink_dsp_node_id = fid;
    }
    if (ctx->source_dsp_node_id == 0 && ctx->source_filter) {
        uint32_t fid = pw_filter_get_node_id(ctx->source_filter);
        if (fid != SPA_ID_INVALID && fid != 0) ctx->source_dsp_node_id = fid;
    }

    /* 1. Output Pipeline: litefx_sink:monitor -> litefx_sink_dsp:input */
    if (ctx->virtual_sink_id != 0 && ctx->sink_dsp_node_id != 0) {
        if (!ctx->link_sink_mon_l || !ctx->link_sink_mon_r) {
            uint32_t mon_l = find_port_id(ctx, ctx->virtual_sink_id, false, true, 0, false);
            uint32_t mon_r = find_port_id(ctx, ctx->virtual_sink_id, false, true, 1, false);
            uint32_t dsp_in_l = find_port_id(ctx, ctx->sink_dsp_node_id, true, false, 0, false);
            uint32_t dsp_in_r = find_port_id(ctx, ctx->sink_dsp_node_id, true, false, 1, false);

            if (mon_l && mon_r && dsp_in_l && dsp_in_r) {
                destroy_proxy_safe(&ctx->link_sink_mon_l);
                destroy_proxy_safe(&ctx->link_sink_mon_r);
                ctx->link_sink_mon_l = create_pw_link(ctx, ctx->virtual_sink_id, mon_l,
                                                      ctx->sink_dsp_node_id, dsp_in_l);
                ctx->link_sink_mon_r = create_pw_link(ctx, ctx->virtual_sink_id, mon_r,
                                                      ctx->sink_dsp_node_id, dsp_in_r);
            }
        }
    }

    /* 2. Output Pipeline: litefx_sink_dsp:output -> target_hw_sink:playback */
    const TrackedDev *hw_sink = resolve_target_sink(ctx);
    if (hw_sink && ctx->sink_dsp_node_id != 0) {
        if (!ctx->link_sink_hw_l || !ctx->link_sink_hw_r || ctx->linked_hw_sink_id != hw_sink->id) {
            uint32_t dsp_out_l = find_port_id(ctx, ctx->sink_dsp_node_id, false, false, 0, false);
            uint32_t dsp_out_r = find_port_id(ctx, ctx->sink_dsp_node_id, false, false, 1, false);
            uint32_t hw_in_l = find_port_id(ctx, hw_sink->id, true, false, 0, false);
            uint32_t hw_in_r = find_port_id(ctx, hw_sink->id, true, false, 1, true);

            if (dsp_out_l && dsp_out_r && hw_in_l && hw_in_r) {
                destroy_proxy_safe(&ctx->link_sink_hw_l);
                destroy_proxy_safe(&ctx->link_sink_hw_r);
                ctx->link_sink_hw_l = create_pw_link(ctx, ctx->sink_dsp_node_id, dsp_out_l,
                                                     hw_sink->id, hw_in_l);
                ctx->link_sink_hw_r = create_pw_link(ctx, ctx->sink_dsp_node_id, dsp_out_r,
                                                     hw_sink->id, hw_in_r);
                ctx->linked_hw_sink_id = hw_sink->id;
            }
        }
    }

    /* 3. Input Pipeline: target_hw_source:capture -> litefx_source_dsp:input */
    const TrackedDev *hw_source = resolve_target_source(ctx);
    if (hw_source && ctx->source_dsp_node_id != 0) {
        if (!ctx->link_source_hw_l || !ctx->link_source_hw_r || ctx->linked_hw_source_id != hw_source->id) {
            uint32_t hw_out_l = find_port_id(ctx, hw_source->id, false, false, 0, false);
            uint32_t hw_out_r = find_port_id(ctx, hw_source->id, false, false, 1, true);
            uint32_t dsp_in_l = find_port_id(ctx, ctx->source_dsp_node_id, true, false, 0, false);
            uint32_t dsp_in_r = find_port_id(ctx, ctx->source_dsp_node_id, true, false, 1, false);

            if (hw_out_l && hw_out_r && dsp_in_l && dsp_in_r) {
                destroy_proxy_safe(&ctx->link_source_hw_l);
                destroy_proxy_safe(&ctx->link_source_hw_r);
                ctx->link_source_hw_l = create_pw_link(ctx, hw_source->id, hw_out_l,
                                                       ctx->source_dsp_node_id, dsp_in_l);
                ctx->link_source_hw_r = create_pw_link(ctx, hw_source->id, hw_out_r,
                                                       ctx->source_dsp_node_id, dsp_in_r);
                ctx->linked_hw_source_id = hw_source->id;
            }
        }
    }

    /* 4. Input Pipeline: litefx_source_dsp:output -> litefx_source:playback */
    if (ctx->virtual_source_id != 0 && ctx->source_dsp_node_id != 0) {
        if (!ctx->link_source_play_l || !ctx->link_source_play_r) {
            uint32_t dsp_out_l = find_port_id(ctx, ctx->source_dsp_node_id, false, false, 0, false);
            uint32_t dsp_out_r = find_port_id(ctx, ctx->source_dsp_node_id, false, false, 1, false);
            uint32_t src_in_l = find_port_id(ctx, ctx->virtual_source_id, true, false, 0, false);
            uint32_t src_in_r = find_port_id(ctx, ctx->virtual_source_id, true, false, 1, false);

            if (dsp_out_l && dsp_out_r && src_in_l && src_in_r) {
                destroy_proxy_safe(&ctx->link_source_play_l);
                destroy_proxy_safe(&ctx->link_source_play_r);
                ctx->link_source_play_l = create_pw_link(ctx, ctx->source_dsp_node_id, dsp_out_l,
                                                         ctx->virtual_source_id, src_in_l);
                ctx->link_source_play_r = create_pw_link(ctx, ctx->source_dsp_node_id, dsp_out_r,
                                                         ctx->virtual_source_id, src_in_r);
            }
        }
    }
}

static void apply_pipewire_metadata_defaults(DaemonCtx *ctx) {
    if (!ctx->default_meta || ctx->shutting_down) return;

    /*
     * We intentionally DO NOT set default.audio.sink or default.audio.source.
     * In WirePlumber, volume shortcuts and desktop shells (such as Noctalia) control
     * @DEFAULT_AUDIO_SINK@ directly via hardware DAC / ALSA master volume.
     * If LiteFX hijacked default.audio.sink in metadata, desktop volume controls
     * would target litefx_sink instead of the hardware sound card, causing volume
     * keys and sliders to have no physical effect.
     *
     * Instead, system default devices remain pointing to physical hardware,
     * litefx nodes have priority.session = "0", and active streams are dynamically
     * routed into litefx_sink/litefx_source via per-stream metadata (target.node/object).
     */

    /* Auto-heal in case an earlier crash or old version left default.audio.sink pointing to litefx */
    const TrackedDev *hw_sink = resolve_target_sink(ctx);
    if (hw_sink && ctx->system_default_sink[0] &&
        strncmp(ctx->system_default_sink, "litefx", 6) == 0) {
        char jbuf[192];
        snprintf(jbuf, sizeof(jbuf), "{\"name\":\"%s\"}", hw_sink->name);
        pw_metadata_set_property(ctx->default_meta, 0, "default.audio.sink", "Spa:String:JSON", jbuf);
        pw_metadata_set_property(ctx->default_meta, 0, "default.configured.audio.sink", "Spa:String:JSON", jbuf);
        snprintf(ctx->system_default_sink, sizeof(ctx->system_default_sink), "%s", hw_sink->name);
    }

    const TrackedDev *hw_source = resolve_target_source(ctx);
    if (hw_source && ctx->system_default_source[0] &&
        strncmp(ctx->system_default_source, "litefx", 6) == 0) {
        char jbuf[192];
        snprintf(jbuf, sizeof(jbuf), "{\"name\":\"%s\"}", hw_source->name);
        pw_metadata_set_property(ctx->default_meta, 0, "default.audio.source", "Spa:String:JSON", jbuf);
        pw_metadata_set_property(ctx->default_meta, 0, "default.configured.audio.source", "Spa:String:JSON", jbuf);
        snprintf(ctx->system_default_source, sizeof(ctx->system_default_source), "%s", hw_source->name);
    }
}

static void route_stream_to_sink(DaemonCtx *ctx, TrackedStream *st, bool to_litefx) {
    if (!ctx->default_meta) return;

    if (to_litefx) {
        uint32_t target_id = 0;
        uint32_t target_serial = 0;

        if (st->is_input) {
            /* Microphone capture stream -> virtual source */
            target_id = ctx->virtual_source_id;
            target_serial = ctx->virtual_source_serial;
        } else {
            /* Playback stream or desktop/screen recording sink capture -> virtual sink */
            target_id = ctx->virtual_sink_id;
            target_serial = ctx->virtual_sink_serial;
        }

        if (target_id == 0) return;

        char id_str[32];
        char serial_str[32];
        snprintf(id_str, sizeof(id_str), "%u", target_id);
        snprintf(serial_str, sizeof(serial_str), "%u", target_serial ? target_serial : target_id);
        pw_metadata_set_property(ctx->default_meta, st->id, "target.node", "Spa:Id", id_str);
        pw_metadata_set_property(ctx->default_meta, st->id, "target.object", "Spa:Id", serial_str);
        st->routed_by_us = true;
    } else if (st->routed_by_us) {
        pw_metadata_set_property(ctx->default_meta, st->id, "target.object", NULL, NULL);
        pw_metadata_set_property(ctx->default_meta, st->id, "target.node", NULL, NULL);
        st->routed_by_us = false;
    }
}

static void reconcile_streams(DaemonCtx *ctx) {
    if (ctx->shutting_down || !ctx->default_meta) return;

    for (uint32_t i = 0; i < ctx->stream_count; ++i) {
        TrackedStream *st = &ctx->streams[i];
        bool blocked = is_stream_blocklisted(&ctx->cfg, st);
        bool route_active = false;
        if (st->is_input) {
            route_active = (ctx->cfg.auto_route_source_apps && ctx->virtual_source_id != 0);
        } else {
            route_active = (ctx->cfg.auto_route_sink_apps && ctx->virtual_sink_id != 0);
        }
        bool want = route_active && !blocked;

        if (want && !st->routed_by_us) {
            route_stream_to_sink(ctx, st, true);
        } else if (!want && st->routed_by_us) {
            route_stream_to_sink(ctx, st, false);
        }
    }
    sync_shm_snapshot(ctx);
}

/* Output DSP Filter Process Callback */
static void on_sink_filter_process(void *userdata, struct spa_io_position *position) {
    DaemonCtx *ctx = (DaemonCtx *)userdata;
    uint32_t n_samples = position ? position->clock.duration : 0;
    if (n_samples == 0) return;

    uint32_t rate = (position && position->clock.rate.denom >= 8000u)
                    ? position->clock.rate.denom : 48000u;

    float *in_l = (float *)pw_filter_get_dsp_buffer(ctx->port_sink_in_l, n_samples);
    float *in_r = (float *)pw_filter_get_dsp_buffer(ctx->port_sink_in_r, n_samples);
    float *out_l = (float *)pw_filter_get_dsp_buffer(ctx->port_sink_out_l, n_samples);
    float *out_r = (float *)pw_filter_get_dsp_buffer(ctx->port_sink_out_r, n_samples);

    if (!out_l || !out_r) return;
    if (!in_l && !in_r) {
        memset(out_l, 0, n_samples * sizeof(float));
        memset(out_r, 0, n_samples * sizeof(float));
        ctx->sink_was_silent = true;
        return;
    }

    if (ctx->sink_was_silent) {
        ctx->sink_was_silent = false;
        dsp_engine_reset_state(&ctx->dsp_sink);
    }

    LiteFxShm *shm = ctx->shm;
    bool gui_watching = false;
    uint64_t t_start = 0;

    if (shm) {
        atomic_store_explicit(&shm->sample_rate, rate, memory_order_relaxed);
        atomic_store_explicit(&shm->quantum_frames, n_samples, memory_order_relaxed);
        uint64_t hb = atomic_load_explicit(&shm->gui_heartbeat_ns, memory_order_relaxed);
        if (hb != 0) {
            t_start = litefx_now_ns();
            if (t_start >= hb && (t_start - hb) < 1500000000ULL) {
                gui_watching = true;
            }
        }
    }

    float pin_l = 0.0f, pin_r = 0.0f, pout_l = 0.0f, pout_r = 0.0f;
    dsp_engine_process_ex(&ctx->dsp_sink,
                          in_l ? in_l : in_r,
                          in_r ? in_r : in_l,
                          out_l, out_r,
                          n_samples,
                          gui_watching,
                          &pin_l, &pin_r, &pout_l, &pout_r,
                          gui_watching && ctx->cfg.spectrum_enabled && shm != NULL,
                          shm ? shm->sink_spectrum_db : NULL);

    if (gui_watching && shm) {
        uint64_t dt = litefx_now_ns() - t_start;
        atomic_store_explicit(&shm->sink_dsp_proc_ns, (uint32_t)dt, memory_order_relaxed);

        float prev_ol = atomic_load_explicit(&shm->sink_peak_out_l, memory_order_relaxed) * 0.82f;
        float prev_or = atomic_load_explicit(&shm->sink_peak_out_r, memory_order_relaxed) * 0.82f;
        atomic_store_explicit(&shm->sink_peak_out_l, pout_l > prev_ol ? pout_l : prev_ol, memory_order_relaxed);
        atomic_store_explicit(&shm->sink_peak_out_r, pout_r > prev_or ? pout_r : prev_or, memory_order_relaxed);
    }
}

static void on_sink_filter_state_changed(void *userdata,
                                         enum pw_filter_state old_state,
                                         enum pw_filter_state state,
                                         const char *error) {
    (void)old_state;
    DaemonCtx *ctx = (DaemonCtx *)userdata;
    if (ctx->shm) {
        atomic_store_explicit(&ctx->shm->sink_dsp_streaming,
                              state == PW_FILTER_STATE_STREAMING,
                              memory_order_relaxed);
        if (state != PW_FILTER_STATE_STREAMING) {
            ctx->sink_was_silent = true;
            atomic_store_explicit(&ctx->shm->sink_peak_out_l, 0.0f, memory_order_relaxed);
            atomic_store_explicit(&ctx->shm->sink_peak_out_r, 0.0f, memory_order_relaxed);
        }
    }
    if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) {
        reconcile_links(ctx);
    } else if (state == PW_FILTER_STATE_ERROR && error) {
        fprintf(stderr, "[litefxd] Sink filter error: %s\n", error);
    }
}

static const struct pw_filter_events sink_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = on_sink_filter_state_changed,
    .process = on_sink_filter_process,
};

/* Input / Microphone DSP Filter Process Callback */
static void on_source_filter_process(void *userdata, struct spa_io_position *position) {
    DaemonCtx *ctx = (DaemonCtx *)userdata;
    uint32_t n_samples = position ? position->clock.duration : 0;
    if (n_samples == 0) return;

    float *in_l = (float *)pw_filter_get_dsp_buffer(ctx->port_source_in_l, n_samples);
    float *in_r = (float *)pw_filter_get_dsp_buffer(ctx->port_source_in_r, n_samples);
    float *out_l = (float *)pw_filter_get_dsp_buffer(ctx->port_source_out_l, n_samples);
    float *out_r = (float *)pw_filter_get_dsp_buffer(ctx->port_source_out_r, n_samples);

    if (!out_l || !out_r) return;
    if (!in_l && !in_r) {
        memset(out_l, 0, n_samples * sizeof(float));
        memset(out_r, 0, n_samples * sizeof(float));
        ctx->source_was_silent = true;
        return;
    }

    if (ctx->source_was_silent) {
        ctx->source_was_silent = false;
        dsp_engine_reset_state(&ctx->dsp_source);
    }

    LiteFxShm *shm = ctx->shm;
    bool gui_watching = false;
    uint64_t t_start = 0;

    if (shm) {
        uint64_t hb = atomic_load_explicit(&shm->gui_heartbeat_ns, memory_order_relaxed);
        if (hb != 0) {
            t_start = litefx_now_ns();
            if (t_start >= hb && (t_start - hb) < 1500000000ULL) {
                gui_watching = true;
            }
        }
    }

    float pin_l = 0.0f, pin_r = 0.0f, pout_l = 0.0f, pout_r = 0.0f;
    dsp_engine_process_ex(&ctx->dsp_source,
                          in_l ? in_l : in_r,
                          in_r ? in_r : in_l,
                          out_l, out_r,
                          n_samples,
                          gui_watching,
                          &pin_l, &pin_r, &pout_l, &pout_r,
                          gui_watching && ctx->cfg.spectrum_enabled && shm != NULL,
                          shm ? shm->source_spectrum_db : NULL);

    if (gui_watching && shm) {
        uint64_t dt = litefx_now_ns() - t_start;
        atomic_store_explicit(&shm->source_dsp_proc_ns, (uint32_t)dt, memory_order_relaxed);

        float prev_ol = atomic_load_explicit(&shm->source_peak_out_l, memory_order_relaxed) * 0.82f;
        float prev_or = atomic_load_explicit(&shm->source_peak_out_r, memory_order_relaxed) * 0.82f;
        atomic_store_explicit(&shm->source_peak_out_l, pout_l > prev_ol ? pout_l : prev_ol, memory_order_relaxed);
        atomic_store_explicit(&shm->source_peak_out_r, pout_r > prev_or ? pout_r : prev_or, memory_order_relaxed);
    }
}

static void on_source_filter_state_changed(void *userdata,
                                           enum pw_filter_state old_state,
                                           enum pw_filter_state state,
                                           const char *error) {
    (void)old_state;
    DaemonCtx *ctx = (DaemonCtx *)userdata;
    if (ctx->shm) {
        atomic_store_explicit(&ctx->shm->source_dsp_streaming,
                              state == PW_FILTER_STATE_STREAMING,
                              memory_order_relaxed);
        if (state != PW_FILTER_STATE_STREAMING) {
            ctx->source_was_silent = true;
            atomic_store_explicit(&ctx->shm->source_peak_out_l, 0.0f, memory_order_relaxed);
            atomic_store_explicit(&ctx->shm->source_peak_out_r, 0.0f, memory_order_relaxed);
        }
    }
    if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) {
        reconcile_links(ctx);
    } else if (state == PW_FILTER_STATE_ERROR && error) {
        fprintf(stderr, "[litefxd] Source filter error: %s\n", error);
    }
}

static const struct pw_filter_events source_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = on_source_filter_state_changed,
    .process = on_source_filter_process,
};

static bool extract_json_name(const char *json, char *out, size_t out_len) {
    if (!json) return false;
    const char *p = strstr(json, "\"name\"");
    if (!p) return false;
    p = strchr(p + 6, ':');
    if (!p) return false;
    p = strchr(p + 1, '"');
    if (!p) return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_len) return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static int on_metadata_property(void *data, uint32_t subject,
                                const char *key, const char *type, const char *value) {
    (void)type;
    DaemonCtx *ctx = (DaemonCtx *)data;
    if (subject == 0 && key && value) {
        if (strcmp(key, "default.audio.sink") == 0 ||
            strcmp(key, "default.configured.audio.sink") == 0) {
            char name[LITEFX_NAME_LEN];
            if (extract_json_name(value, name, sizeof(name))) {
                if (strncmp(name, "litefx", 6) == 0) {
                    const TrackedDev *hw = resolve_target_sink(ctx);
                    if (hw) {
                        char jbuf[192];
                        snprintf(jbuf, sizeof(jbuf), "{\"name\":\"%s\"}", hw->name);
                        pw_metadata_set_property(ctx->default_meta, 0, key, "Spa:String:JSON", jbuf);
                    }
                } else if (!is_virtual_or_own(name) && strcmp(ctx->system_default_sink, name) != 0) {
                    snprintf(ctx->system_default_sink, sizeof(ctx->system_default_sink), "%s", name);
                    reconcile_links(ctx);
                    sync_shm_snapshot(ctx);
                }
            }
        } else if (strcmp(key, "default.audio.source") == 0 ||
                   strcmp(key, "default.configured.audio.source") == 0) {
            char name[LITEFX_NAME_LEN];
            if (extract_json_name(value, name, sizeof(name))) {
                if (strncmp(name, "litefx", 6) == 0) {
                    const TrackedDev *hw = resolve_target_source(ctx);
                    if (hw) {
                        char jbuf[192];
                        snprintf(jbuf, sizeof(jbuf), "{\"name\":\"%s\"}", hw->name);
                        pw_metadata_set_property(ctx->default_meta, 0, key, "Spa:String:JSON", jbuf);
                    }
                } else if (!is_virtual_or_own(name) && strcmp(ctx->system_default_source, name) != 0) {
                    snprintf(ctx->system_default_source, sizeof(ctx->system_default_source), "%s", name);
                    reconcile_links(ctx);
                    sync_shm_snapshot(ctx);
                }
            }
        }
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

static int classify_port_channel(const char *port_name, const char *dsp_channel) {
    const char *s = (dsp_channel && dsp_channel[0]) ? dsp_channel : port_name;
    if (!s) return -1;
    if (strstr(s, "FL") || strstr(s, "_0") || strstr(s, "MONO") || strstr(s, "mono") ||
        strcmp(s, "in_l") == 0 || strcmp(s, "out_l") == 0 || strcmp(s, "input_FL") == 0 || strcmp(s, "output_FL") == 0)
        return 0;
    if (strstr(s, "FR") || strstr(s, "_1") ||
        strcmp(s, "in_r") == 0 || strcmp(s, "out_r") == 0 || strcmp(s, "input_FR") == 0 || strcmp(s, "output_FR") == 0)
        return 1;
    return -1;
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
    (void)permissions;
    (void)version;
    DaemonCtx *ctx = (DaemonCtx *)data;
    if (!props) return;

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *mname = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (mname && strcmp(mname, "default") == 0 && !ctx->default_meta) {
            ctx->default_meta_id = id;
            ctx->default_meta = (struct pw_metadata *)pw_registry_bind(
                ctx->registry, id, type, PW_VERSION_METADATA, 0);
            if (ctx->default_meta) {
                pw_metadata_add_listener(ctx->default_meta, &ctx->meta_listener,
                                         &metadata_events, ctx);
                apply_pipewire_metadata_defaults(ctx);
                reconcile_streams(ctx);
            }
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *mclass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char *nname = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *ndesc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        const char *ser_s = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
        uint32_t serial = ser_s ? (uint32_t)strtoul(ser_s, NULL, 10) : id;

        if (nname && strcmp(nname, LITEFX_SINK_NAME) == 0) {
            ctx->virtual_sink_id = id;
            ctx->virtual_sink_serial = serial;
            apply_pipewire_metadata_defaults(ctx);
            reconcile_links(ctx);
            reconcile_streams(ctx);
            return;
        }

        if (nname && strcmp(nname, LITEFX_SOURCE_NAME) == 0) {
            ctx->virtual_source_id = id;
            ctx->virtual_source_serial = serial;
            apply_pipewire_metadata_defaults(ctx);
            reconcile_links(ctx);
            return;
        }

        if (nname && strcmp(nname, LITEFX_DSP_SINK_NAME) == 0) {
            ctx->sink_dsp_node_id = id;
            reconcile_links(ctx);
            return;
        }

        if (nname && strcmp(nname, LITEFX_DSP_SOURCE_NAME) == 0) {
            ctx->source_dsp_node_id = id;
            reconcile_links(ctx);
            return;
        }

        /* Physical Sinks (Speakers/Headphones) */
        if (mclass && strcmp(mclass, "Audio/Sink") == 0) {
            if (!is_virtual_or_own(nname) && ctx->sink_count < LITEFX_MAX_SINKS) {
                TrackedDev *dev = &ctx->sinks[ctx->sink_count++];
                dev->id = id;
                dev->serial = serial;
                snprintf(dev->name, sizeof(dev->name), "%s", nname ? nname : "");
                snprintf(dev->desc, sizeof(dev->desc), "%s", ndesc ? ndesc : dev->name);
                reconcile_links(ctx);
                sync_shm_snapshot(ctx);
            }
            return;
        }

        /* Physical Sources (Microphones) */
        if (mclass && strcmp(mclass, "Audio/Source") == 0) {
            if (!is_virtual_or_own(nname) && ctx->source_count < LITEFX_MAX_SOURCES) {
                TrackedDev *dev = &ctx->sources[ctx->source_count++];
                dev->id = id;
                dev->serial = serial;
                snprintf(dev->name, sizeof(dev->name), "%s", nname ? nname : "");
                snprintf(dev->desc, sizeof(dev->desc), "%s", ndesc ? ndesc : dev->name);
                reconcile_links(ctx);
                sync_shm_snapshot(ctx);
            }
            return;
        }

        /* Active Audio Streams (Playback or Capture) */
        if (mclass && (strcmp(mclass, "Stream/Output/Audio") == 0 ||
                       strcmp(mclass, "Stream/Input/Audio") == 0)) {
            const char *pid_s = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
            uint32_t pid = pid_s ? (uint32_t)strtoul(pid_s, NULL, 10) : 0;
            if (pid == (uint32_t)getpid()) return;
            if (nname && (strncmp(nname, "litefx", 6) == 0 ||
                          strncmp(nname, "ee_", 3) == 0 ||
                          strncmp(nname, "easyeffects", 11) == 0)) {
                return;
            }
            if (ctx->stream_count < LITEFX_MAX_STREAMS) {
                const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
                const char *bin = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
                const char *med = spa_dict_lookup(props, PW_KEY_MEDIA_NAME);
                const char *cap_sink = spa_dict_lookup(props, "stream.capture.sink");
                const char *tgt_obj = spa_dict_lookup(props, PW_KEY_TARGET_OBJECT);
                const char *tgt_node = spa_dict_lookup(props, "target.node");

                bool is_sink_cap = false;
                if (cap_sink && (strcasecmp(cap_sink, "true") == 0 || strcmp(cap_sink, "1") == 0)) {
                    is_sink_cap = true;
                }
                if (tgt_obj && strstr(tgt_obj, ".monitor") != NULL) {
                    is_sink_cap = true;
                }
                if (tgt_node && strstr(tgt_node, ".monitor") != NULL) {
                    is_sink_cap = true;
                }
                if (nname && (strstr(nname, "default_output") || strstr(nname, ".monitor"))) {
                    is_sink_cap = true;
                }
                if (med && (strstr(med, "default_output") || strstr(med, "Monitor of") || strstr(med, ".monitor"))) {
                    is_sink_cap = true;
                }

                TrackedStream *st = &ctx->streams[ctx->stream_count++];
                memset(st, 0, sizeof(*st));
                st->id = id;
                st->pid = pid;
                st->is_sink_capture = is_sink_cap;
                st->is_input = (strcmp(mclass, "Stream/Input/Audio") == 0) && !is_sink_cap;
                snprintf(st->node_name, sizeof(st->node_name), "%s", nname ? nname : "");
                snprintf(st->app_name, sizeof(st->app_name), "%s", app ? app : (nname ? nname : "App"));
                snprintf(st->binary, sizeof(st->binary), "%s", bin ? bin : "");
                snprintf(st->media_name, sizeof(st->media_name), "%s", med ? med : "");
                reconcile_streams(ctx);
            }
            return;
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        if (ctx->port_count >= MAX_TRACKED_PORTS) return;
        const char *nid_s = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *pname = spa_dict_lookup(props, PW_KEY_PORT_NAME);
        const char *pdir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char *pmon = spa_dict_lookup(props, PW_KEY_PORT_MONITOR);
        const char *pch = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
        if (!nid_s || !pname || !pdir) return;

        int ch = classify_port_channel(pname, pch);
        if (ch < 0) return;

        TrackedPort *pt = &ctx->ports[ctx->port_count++];
        pt->global_id = id;
        pt->node_id = (uint32_t)strtoul(nid_s, NULL, 10);
        pt->channel = ch;
        pt->is_input = (strcmp(pdir, "in") == 0);
        pt->is_monitor = (pmon && strcmp(pmon, "true") == 0) || (strncmp(pname, "monitor", 7) == 0);

        reconcile_links(ctx);
    }
}

static void on_registry_global_remove(void *data, uint32_t id) {
    DaemonCtx *ctx = (DaemonCtx *)data;

    if (id == ctx->default_meta_id && ctx->default_meta) {
        spa_hook_remove(&ctx->meta_listener);
        pw_proxy_destroy((struct pw_proxy *)ctx->default_meta);
        ctx->default_meta = NULL;
        ctx->default_meta_id = 0;
        return;
    }

    for (uint32_t i = 0; i < ctx->port_count; ++i) {
        if (ctx->ports[i].global_id == id) {
            ctx->ports[i] = ctx->ports[--ctx->port_count];
            return;
        }
    }

    for (uint32_t i = 0; i < ctx->sink_count; ++i) {
        if (ctx->sinks[i].id == id) {
            if (ctx->linked_hw_sink_id == id) {
                destroy_proxy_safe(&ctx->link_sink_hw_l);
                destroy_proxy_safe(&ctx->link_sink_hw_r);
                ctx->linked_hw_sink_id = 0;
            }
            for (uint32_t j = i; j + 1 < ctx->sink_count; ++j) ctx->sinks[j] = ctx->sinks[j + 1];
            ctx->sink_count--;
            reconcile_links(ctx);
            sync_shm_snapshot(ctx);
            return;
        }
    }

    for (uint32_t i = 0; i < ctx->source_count; ++i) {
        if (ctx->sources[i].id == id) {
            if (ctx->linked_hw_source_id == id) {
                destroy_proxy_safe(&ctx->link_source_hw_l);
                destroy_proxy_safe(&ctx->link_source_hw_r);
                ctx->linked_hw_source_id = 0;
            }
            for (uint32_t j = i; j + 1 < ctx->source_count; ++j) ctx->sources[j] = ctx->sources[j + 1];
            ctx->source_count--;
            reconcile_links(ctx);
            sync_shm_snapshot(ctx);
            return;
        }
    }

    for (uint32_t i = 0; i < ctx->stream_count; ++i) {
        if (ctx->streams[i].id == id) {
            for (uint32_t j = i; j + 1 < ctx->stream_count; ++j) ctx->streams[j] = ctx->streams[j + 1];
            ctx->stream_count--;
            sync_shm_snapshot(ctx);
            return;
        }
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void apply_new_config(DaemonCtx *ctx, const LiteFxConfig *new_cfg, bool save_disk) {
    bool sink_dev_changed = (strcmp(ctx->cfg.target_sink, new_cfg->target_sink) != 0);
    bool source_dev_changed = (strcmp(ctx->cfg.target_source, new_cfg->target_source) != 0);

    ctx->cfg = *new_cfg;

    uint32_t sr = ctx->shm ? atomic_load_explicit(&ctx->shm->sample_rate, memory_order_relaxed) : 48000u;
    if (sr < 8000u) sr = 48000u;

    dsp_engine_update_sink(&ctx->dsp_sink, &ctx->cfg, sr);
    dsp_engine_update_source(&ctx->dsp_source, &ctx->cfg, sr);

    if (sink_dev_changed) {
        destroy_proxy_safe(&ctx->link_sink_hw_l);
        destroy_proxy_safe(&ctx->link_sink_hw_r);
        ctx->linked_hw_sink_id = 0;
    }
    if (source_dev_changed) {
        destroy_proxy_safe(&ctx->link_source_hw_l);
        destroy_proxy_safe(&ctx->link_source_hw_r);
        ctx->linked_hw_source_id = 0;
    }

    reconcile_links(ctx);
    reconcile_streams(ctx);
    apply_pipewire_metadata_defaults(ctx);
    sync_shm_snapshot(ctx);

    if (save_disk) {
        litefx_config_save(&ctx->cfg, ctx->cfg_path);
    }
}

static void on_ipc_socket_io(void *data, int fd, uint32_t mask) {
    (void)mask;
    DaemonCtx *ctx = (DaemonCtx *)data;
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) return;

    LiteFxCmdPacket pkt;
    uint8_t *buf = (uint8_t *)&pkt;
    size_t rem = sizeof(pkt);
    while (rem > 0) {
        ssize_t r = recv(cfd, buf, rem, 0);
        if (r <= 0) {
            close(cfd);
            return;
        }
        buf += (size_t)r;
        rem -= (size_t)r;
    }

    LiteFxReplyPacket rep;
    memset(&rep, 0, sizeof(rep));

    if (pkt.magic != LITEFX_MAGIC) {
        rep.ok = 0;
        snprintf(rep.message, sizeof(rep.message), "Invalid IPC magic");
    } else {
        rep.ok = 1;
        switch ((LiteFxCmdType)pkt.cmd) {
            case CMD_SET_CONFIG:
                apply_new_config(ctx, &pkt.config, pkt.arg_int != 0);
                snprintf(rep.message, sizeof(rep.message), "OK");
                break;

            case CMD_SAVE_CURRENT:
                rep.ok = litefx_config_save(&ctx->cfg, ctx->cfg_path) ? 1 : 0;
                snprintf(rep.message, sizeof(rep.message),
                         rep.ok ? "Saved configuration" : "Failed to save config");
                break;

            case CMD_RELOAD_CURRENT: {
                LiteFxConfig loaded;
                if (litefx_config_load(&loaded, ctx->cfg_path)) {
                    apply_new_config(ctx, &loaded, false);
                    snprintf(rep.message, sizeof(rep.message), "Reloaded config");
                } else {
                    rep.ok = 0;
                    snprintf(rep.message, sizeof(rep.message), "Config file not found");
                }
                break;
            }

            case CMD_IMPORT_EASYEFFECTS: {
                LiteFxConfig imported = ctx->cfg;
                if (litefx_config_import_easyeffects(&imported, rep.message, sizeof(rep.message))) {
                    apply_new_config(ctx, &imported, true);
                } else {
                    rep.ok = 0;
                }
                break;
            }

            case CMD_TOGGLE_SINK_BYPASS: {
                LiteFxConfig tmp = ctx->cfg;
                tmp.sink_bypass = !tmp.sink_bypass;
                apply_new_config(ctx, &tmp, true);
                snprintf(rep.message, sizeof(rep.message),
                         "Speakers DSP %s", tmp.sink_bypass ? "Bypassed" : "Active");
                break;
            }

            case CMD_TOGGLE_SOURCE_BYPASS: {
                LiteFxConfig tmp = ctx->cfg;
                tmp.source_bypass = !tmp.source_bypass;
                apply_new_config(ctx, &tmp, true);
                snprintf(rep.message, sizeof(rep.message),
                         "Mic DSP %s", tmp.source_bypass ? "Bypassed" : "Active");
                break;
            }

            case CMD_STOP_DAEMON:
                snprintf(rep.message, sizeof(rep.message), "Stopping litefxd");
                send(cfd, &rep, sizeof(rep), MSG_NOSIGNAL);
                close(cfd);
                pw_main_loop_quit(ctx->main_loop);
                return;

            default:
                rep.ok = 0;
                snprintf(rep.message, sizeof(rep.message), "Unknown command");
                break;
        }
    }

    send(cfd, &rep, sizeof(rep), MSG_NOSIGNAL);
    close(cfd);
}

static void on_quit_signal(void *data, int signal_number) {
    (void)signal_number;
    DaemonCtx *ctx = (DaemonCtx *)data;
    pw_main_loop_quit(ctx->main_loop);
}

static bool check_existing_daemon_alive(const char *sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    bool alive = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return alive;
}

static bool setup_shm_and_socket(DaemonCtx *ctx) {
    litefx_get_sock_path(ctx->sock_path, sizeof(ctx->sock_path));
    litefx_get_shm_path(ctx->shm_path, sizeof(ctx->shm_path));

    if (check_existing_daemon_alive(ctx->sock_path)) {
        fprintf(stderr, "[litefxd] Daemon is already running at %s\n", ctx->sock_path);
        return false;
    }

    unlink(ctx->sock_path);
    ctx->sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ctx->sock_fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ctx->sock_path);

    if (bind(ctx->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ctx->sock_fd);
        return false;
    }
    chmod(ctx->sock_path, 0600);
    if (listen(ctx->sock_fd, 8) < 0) {
        close(ctx->sock_fd);
        return false;
    }

    ctx->shm_fd = open(ctx->shm_path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
    if (ctx->shm_fd < 0) {
        close(ctx->sock_fd);
        return false;
    }
    if (ftruncate(ctx->shm_fd, sizeof(LiteFxShm)) < 0) {
        close(ctx->shm_fd);
        close(ctx->sock_fd);
        return false;
    }

    void *map = mmap(NULL, sizeof(LiteFxShm), PROT_READ | PROT_WRITE, MAP_SHARED, ctx->shm_fd, 0);
    if (map == MAP_FAILED) {
        close(ctx->shm_fd);
        close(ctx->sock_fd);
        return false;
    }

    ctx->shm = (LiteFxShm *)map;
    memset(ctx->shm, 0, sizeof(LiteFxShm));
    ctx->shm->magic = LITEFX_MAGIC;
    ctx->shm->version = LITEFX_VERSION;
    ctx->shm->daemon_pid = (int32_t)getpid();
    atomic_init(&ctx->shm->gui_heartbeat_ns, 0ULL);
    atomic_init(&ctx->shm->config_seq, 1u);
    atomic_init(&ctx->shm->sample_rate, 48000u);
    atomic_init(&ctx->shm->quantum_frames, 1024u);
    return true;
}

static bool setup_pipewire_nodes(DaemonCtx *ctx) {
    /* 1. Virtual Sink (Speakers/Headphones) */
    struct pw_properties *sink_props = pw_properties_new(
        PW_KEY_FACTORY_NAME, "support.null-audio-sink",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, LITEFX_SINK_NAME,
        PW_KEY_NODE_NICK, "litefx_sink",
        PW_KEY_NODE_DESCRIPTION, LITEFX_SINK_DESC,
        PW_KEY_NODE_GROUP, "litefx_sink_group",
        PW_KEY_NODE_LINK_GROUP, "litefx_sink_group",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        PW_KEY_NODE_VIRTUAL, "true",
        PW_KEY_NODE_DRIVER, "true",
        PW_KEY_OBJECT_LINGER, "false",
        "audio.position", "FL,FR",
        "monitor.channel-volumes", "false",
        "monitor.passthrough", "true",
        "priority.session", "0",
        NULL
    );

    ctx->virtual_sink_proxy = (struct pw_proxy *)pw_core_create_object(
        ctx->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &sink_props->dict, 0);
    pw_properties_free(sink_props);

    /* 2. Virtual Source (Microphone) */
    struct pw_properties *src_props = pw_properties_new(
        PW_KEY_FACTORY_NAME, "support.null-audio-sink",
        PW_KEY_MEDIA_CLASS, "Audio/Source/Virtual",
        PW_KEY_NODE_NAME, LITEFX_SOURCE_NAME,
        PW_KEY_NODE_NICK, "litefx_source",
        PW_KEY_NODE_DESCRIPTION, LITEFX_SOURCE_DESC,
        PW_KEY_NODE_GROUP, "litefx_source_group",
        PW_KEY_NODE_LINK_GROUP, "litefx_source_group",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        PW_KEY_NODE_VIRTUAL, "true",
        PW_KEY_NODE_DRIVER, "true",
        PW_KEY_OBJECT_LINGER, "false",
        "audio.position", "FL,FR",
        "monitor.channel-volumes", "false",
        "monitor.passthrough", "true",
        "priority.session", "0",
        NULL
    );

    ctx->virtual_source_proxy = (struct pw_proxy *)pw_core_create_object(
        ctx->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &src_props->dict, 0);
    pw_properties_free(src_props);

    /* 3. Output DSP Filter (litefx_sink_dsp) */
    struct pw_properties *flt_sink_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_NAME, LITEFX_DSP_SINK_NAME,
        PW_KEY_NODE_NAME, LITEFX_DSP_SINK_NAME,
        PW_KEY_NODE_NICK, "litefx_sink",
        PW_KEY_NODE_DESCRIPTION, "LiteFX Speakers DSP",
        PW_KEY_NODE_GROUP, "litefx_sink_group",
        PW_KEY_NODE_LINK_GROUP, "litefx_sink_group",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        PW_KEY_NODE_PASSIVE, "true",
        NULL
    );

    ctx->sink_filter = pw_filter_new(ctx->core, LITEFX_DSP_SINK_NAME, flt_sink_props);
    if (!ctx->sink_filter) return false;
    pw_filter_add_listener(ctx->sink_filter, &ctx->sink_filter_listener, &sink_filter_events, ctx);

    ctx->port_sink_in_l = pw_filter_add_port(ctx->sink_filter, PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "input_FL", PW_KEY_AUDIO_CHANNEL, "FL", NULL), NULL, 0);
    ctx->port_sink_in_r = pw_filter_add_port(ctx->sink_filter, PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "input_FR", PW_KEY_AUDIO_CHANNEL, "FR", NULL), NULL, 0);
    ctx->port_sink_out_l = pw_filter_add_port(ctx->sink_filter, PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output_FL", PW_KEY_AUDIO_CHANNEL, "FL", NULL), NULL, 0);
    ctx->port_sink_out_r = pw_filter_add_port(ctx->sink_filter, PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output_FR", PW_KEY_AUDIO_CHANNEL, "FR", NULL), NULL, 0);

    uint8_t pod_buf[256];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[1];
    params[0] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency,
        &SPA_PROCESS_LATENCY_INFO_INIT(.quantum = 0.0f, .rate = 1, .ns = 0));

    if (pw_filter_connect(ctx->sink_filter, PW_FILTER_FLAG_RT_PROCESS, params, 1) < 0) {
        return false;
    }

    /* 4. Input / Microphone DSP Filter (litefx_source_dsp) */
    struct pw_properties *flt_src_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_NAME, LITEFX_DSP_SOURCE_NAME,
        PW_KEY_NODE_NAME, LITEFX_DSP_SOURCE_NAME,
        PW_KEY_NODE_NICK, "litefx_source",
        PW_KEY_NODE_DESCRIPTION, "LiteFX Mic DSP",
        PW_KEY_NODE_GROUP, "litefx_source_group",
        PW_KEY_NODE_LINK_GROUP, "litefx_source_group",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        PW_KEY_NODE_PASSIVE, "false",
        NULL
    );

    ctx->source_filter = pw_filter_new(ctx->core, LITEFX_DSP_SOURCE_NAME, flt_src_props);
    if (!ctx->source_filter) return false;
    pw_filter_add_listener(ctx->source_filter, &ctx->source_filter_listener, &source_filter_events, ctx);

    ctx->port_source_in_l = pw_filter_add_port(ctx->source_filter, PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "input_FL", PW_KEY_AUDIO_CHANNEL, "FL", NULL), NULL, 0);
    ctx->port_source_in_r = pw_filter_add_port(ctx->source_filter, PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "input_FR", PW_KEY_AUDIO_CHANNEL, "FR", NULL), NULL, 0);
    ctx->port_source_out_l = pw_filter_add_port(ctx->source_filter, PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output_FL", PW_KEY_AUDIO_CHANNEL, "FL", NULL), NULL, 0);
    ctx->port_source_out_r = pw_filter_add_port(ctx->source_filter, PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output_FR", PW_KEY_AUDIO_CHANNEL, "FR", NULL), NULL, 0);

    b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    params[0] = spa_process_latency_build(&b, SPA_PARAM_ProcessLatency,
        &SPA_PROCESS_LATENCY_INFO_INIT(.quantum = 0.0f, .rate = 1, .ns = 0));

    if (pw_filter_connect(ctx->source_filter, PW_FILTER_FLAG_RT_PROCESS, params, 1) < 0) {
        return false;
    }

    return true;
}

int main(int argc, char **argv) {
    DaemonCtx *ctx = (DaemonCtx *)calloc(1, sizeof(DaemonCtx));
    if (!ctx) return 1;
    ctx->shm_fd = -1;
    ctx->sock_fd = -1;

    litefx_get_default_config_path(ctx->cfg_path, sizeof(ctx->cfg_path));

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            ctx->verbose = true;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            snprintf(ctx->cfg_path, sizeof(ctx->cfg_path), "%s", argv[++i]);
        }
    }

    if (!litefx_config_load(&ctx->cfg, ctx->cfg_path)) {
        litefx_config_set_defaults(&ctx->cfg);
        char msg[128];
        litefx_config_import_easyeffects(&ctx->cfg, msg, sizeof(msg));
        litefx_config_save(&ctx->cfg, ctx->cfg_path);
    }

    if (!setup_shm_and_socket(ctx)) {
        free(ctx);
        return 0;
    }

    dsp_engine_init(&ctx->dsp_sink);
    dsp_engine_init(&ctx->dsp_source);
    dsp_engine_update_sink(&ctx->dsp_sink, &ctx->cfg, 48000u);
    dsp_engine_update_source(&ctx->dsp_source, &ctx->cfg, 48000u);
    sync_shm_snapshot(ctx);

    pw_init(&argc, &argv);

    ctx->main_loop = pw_main_loop_new(NULL);
    ctx->loop = pw_main_loop_get_loop(ctx->main_loop);
    ctx->context = pw_context_new(ctx->loop,
        pw_properties_new(
            PW_KEY_APP_NAME, "LiteFX",
            PW_KEY_APP_ID, "org.litefx.daemon",
            NULL),
        0);

    if (!ctx->context) goto cleanup;

    ctx->core = pw_context_connect(ctx->context, NULL, 0);
    if (!ctx->core) goto cleanup;

    ctx->sigint_source = pw_loop_add_signal(ctx->loop, SIGINT, on_quit_signal, ctx);
    ctx->sigterm_source = pw_loop_add_signal(ctx->loop, SIGTERM, on_quit_signal, ctx);
    ctx->sock_source = pw_loop_add_io(ctx->loop, ctx->sock_fd, SPA_IO_IN,
                                      false, on_ipc_socket_io, ctx);

    ctx->registry = pw_core_get_registry(ctx->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(ctx->registry, &ctx->registry_listener, &registry_events, ctx);

    if (!setup_pipewire_nodes(ctx)) goto cleanup;

    if (ctx->verbose) {
        fprintf(stderr, "[litefxd] Running PID %d (Speakers: litefx_sink, Mic: litefx_source)\n", (int)getpid());
    }

    pw_main_loop_run(ctx->main_loop);

    /* Graceful Shutdown: Restore streams and disconnect */
    ctx->shutting_down = true;

    if (ctx->default_meta) {
        /* Unroute all tracked streams back to default hardware routing */
        for (uint32_t i = 0; i < ctx->stream_count; ++i) {
            if (ctx->streams[i].routed_by_us) {
                route_stream_to_sink(ctx, &ctx->streams[i], false);
            }
        }

        /* If default metadata was somehow pointing to litefx, restore hardware */
        const TrackedDev *hw_sink = resolve_target_sink(ctx);
        if (hw_sink && ctx->system_default_sink[0] &&
            strncmp(ctx->system_default_sink, "litefx", 6) == 0) {
            char jbuf[192];
            snprintf(jbuf, sizeof(jbuf), "{\"name\":\"%s\"}", hw_sink->name);
            pw_metadata_set_property(ctx->default_meta, 0, "default.audio.sink", "Spa:String:JSON", jbuf);
            pw_metadata_set_property(ctx->default_meta, 0, "default.configured.audio.sink", "Spa:String:JSON", jbuf);
        }

        const TrackedDev *hw_source = resolve_target_source(ctx);
        if (hw_source && ctx->system_default_source[0] &&
            strncmp(ctx->system_default_source, "litefx", 6) == 0) {
            char jbuf[192];
            snprintf(jbuf, sizeof(jbuf), "{\"name\":\"%s\"}", hw_source->name);
            pw_metadata_set_property(ctx->default_meta, 0, "default.audio.source", "Spa:String:JSON", jbuf);
            pw_metadata_set_property(ctx->default_meta, 0, "default.configured.audio.source", "Spa:String:JSON", jbuf);
        }

        if (ctx->core && ctx->loop) {
            pw_core_sync(ctx->core, PW_ID_CORE, 0);
            pw_loop_iterate(ctx->loop, 30);
        }
    }

cleanup:
    destroy_proxy_safe(&ctx->link_sink_mon_l);
    destroy_proxy_safe(&ctx->link_sink_mon_r);
    destroy_proxy_safe(&ctx->link_sink_hw_l);
    destroy_proxy_safe(&ctx->link_sink_hw_r);

    destroy_proxy_safe(&ctx->link_source_hw_l);
    destroy_proxy_safe(&ctx->link_source_hw_r);
    destroy_proxy_safe(&ctx->link_source_play_l);
    destroy_proxy_safe(&ctx->link_source_play_r);

    if (ctx->sink_filter) pw_filter_destroy(ctx->sink_filter);
    if (ctx->source_filter) pw_filter_destroy(ctx->source_filter);

    destroy_proxy_safe(&ctx->virtual_sink_proxy);
    destroy_proxy_safe(&ctx->virtual_source_proxy);

    if (ctx->default_meta) {
        spa_hook_remove(&ctx->meta_listener);
        pw_proxy_destroy((struct pw_proxy *)ctx->default_meta);
    }
    if (ctx->registry) {
        spa_hook_remove(&ctx->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)ctx->registry);
    }

    if (ctx->sock_source) pw_loop_destroy_source(ctx->loop, ctx->sock_source);
    if (ctx->sigint_source) pw_loop_destroy_source(ctx->loop, ctx->sigint_source);
    if (ctx->sigterm_source) pw_loop_destroy_source(ctx->loop, ctx->sigterm_source);
    if (ctx->core) pw_core_disconnect(ctx->core);
    if (ctx->context) pw_context_destroy(ctx->context);
    if (ctx->main_loop) pw_main_loop_destroy(ctx->main_loop);
    pw_deinit();

    if (ctx->shm) munmap(ctx->shm, sizeof(LiteFxShm));
    if (ctx->shm_fd >= 0) close(ctx->shm_fd);
    if (ctx->sock_fd >= 0) close(ctx->sock_fd);
    if (ctx->shm_path[0]) unlink(ctx->shm_path);
    if (ctx->sock_path[0]) unlink(ctx->sock_path);

    free(ctx);
    return 0;
}
