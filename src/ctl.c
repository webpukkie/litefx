#define _GNU_SOURCE
#include "common.h"
#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static bool read_proc_mem_kb(int pid, long *rss_kb, long *pss_kb) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps_rollup", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    *rss_kb = 0;
    *pss_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Rss:", 4) == 0) *rss_kb = strtol(line + 4, NULL, 10);
        else if (strncmp(line, "Pss:", 4) == 0) *pss_kb = strtol(line + 4, NULL, 10);
    }
    fclose(fp);
    return (*rss_kb > 0);
}

static LiteFxShm *open_shm_readonly(int *fd_out) {
    char shm_path[256];
    litefx_get_shm_path(shm_path, sizeof(shm_path));
    int fd = open(shm_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    void *map = mmap(NULL, sizeof(LiteFxShm), PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    LiteFxShm *shm = (LiteFxShm *)map;
    if (shm->magic != LITEFX_MAGIC) {
        munmap(map, sizeof(LiteFxShm));
        close(fd);
        return NULL;
    }
    *fd_out = fd;
    return shm;
}

static void print_usage(void) {
    printf("Usage: litefxctl <command> [args]\n\n"
           "Commands:\n"
           "  status                 Show memory, audio stream states, devices and routing\n"
           "  toggle                 Toggle Speakers DSP bypass\n"
           "  toggle-mic             Toggle Microphone DSP bypass\n"
           "  import-easyeffects     Import active EQ & Crystalizer from EasyEffects\n"
           "  import-autoeq <file>   Import AutoEQ / EqualizerAPO ParametricEQ or GraphicEQ (.txt)\n"
           "  preset list            List saved presets\n"
           "  preset save <name>     Save current settings as preset <name>\n"
           "  preset load <name>     Load preset <name>\n"
           "  reset                  Reset all DSP settings to factory defaults\n"
           "  stop                   Restore default audio devices and stop litefxd\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "status") == 0) {
        int fd = -1;
        LiteFxShm *shm = open_shm_readonly(&fd);
        if (!shm) {
            printf("litefxd: stopped\n");
            return 1;
        }

        long rss_kb = 0, pss_kb = 0;
        read_proc_mem_kb(shm->daemon_pid, &rss_kb, &pss_kb);

        uint32_t sr = atomic_load_explicit(&shm->sample_rate, memory_order_relaxed);
        uint32_t qf = atomic_load_explicit(&shm->quantum_frames, memory_order_relaxed);
        bool sink_stream = atomic_load_explicit(&shm->sink_dsp_streaming, memory_order_relaxed);
        bool src_stream = atomic_load_explicit(&shm->source_dsp_streaming, memory_order_relaxed);

        printf("litefxd: running (PID %d)\n", shm->daemon_pid);
        printf("  Memory           : PSS %.2f MB (RSS %.2f MB)\n",
               (double)pss_kb / 1024.0, (double)rss_kb / 1024.0);
        printf("  Audio Clock      : %u Hz | quantum %u frames\n", sr, qf);
        printf("  Speakers / Out   : %s (%s) | %s\n",
               shm->resolved_sink_desc[0] ? shm->resolved_sink_desc : "Auto",
               shm->config.target_sink,
               sink_stream ? "STREAMING" : "IDLE");
        printf("    - DSP          : %s | EQ (%u bands) | Crystalizer (13 bands) | Limiter: %s\n",
               shm->config.sink_bypass ? "BYPASS" : "ACTIVE",
               shm->config.sink_eq_num_bands,
               shm->config.sink_soft_limiter ? "ON" : "OFF");
        printf("  Microphone / In  : %s (%s) | %s\n",
               shm->resolved_source_desc[0] ? shm->resolved_source_desc : "Auto",
               shm->config.target_source,
               src_stream ? "STREAMING" : "IDLE");
        printf("    - DSP          : %s | EQ (%u bands) | Crystalizer (13 bands) | Gate: %s | Denoise: %s | Comp: %s\n",
               shm->config.source_bypass ? "BYPASS" : "ACTIVE",
               shm->config.source_eq_num_bands,
               shm->config.source_noise_gate ? "ON" : "OFF",
               shm->config.source_denoiser ? "ON" : "OFF",
               shm->config.source_compressor ? "ON" : "OFF");
        printf("  Auto-Route Apps  : Output: %s | Input: %s (%u active streams)\n",
               shm->config.auto_route_sink_apps ? "ON" : "OFF",
               shm->config.auto_route_source_apps ? "ON" : "OFF",
               shm->stream_count);

        for (uint32_t i = 0; i < shm->stream_count; ++i) {
            const LiteFxStreamInfo *st = &shm->streams[i];
            printf("    - [%u] %s (%s) -> %s\n",
                   st->id, st->app_name,
                   st->media_name[0] ? st->media_name : st->binary,
                   st->blocklisted ? "BLOCKLISTED" :
                   (st->routed_to_litefx ? "LiteFX Sink" : "System Sink"));
        }

        munmap(shm, sizeof(LiteFxShm));
        close(fd);
        return 0;
    }

    if (strcmp(cmd, "import-autoeq") == 0 && argc >= 3) {
        int fd = -1;
        LiteFxShm *shm = open_shm_readonly(&fd);
        LiteFxConfig cfg;
        if (shm) {
            cfg = shm->config;
            munmap(shm, sizeof(LiteFxShm));
            close(fd);
        } else {
            char cpath[512];
            litefx_get_default_config_path(cpath, sizeof(cpath));
            if (!litefx_config_load(&cfg, cpath)) litefx_config_set_defaults(&cfg);
        }
        bool is_mic = (argc >= 4 && strcmp(argv[3], "--mic") == 0);
        char msg[256] = {0};
        if (!litefx_config_import_autoeq(&cfg, argv[2], is_mic, msg, sizeof(msg))) {
            fprintf(stderr, "Error: %s\n", msg);
            return 1;
        }
        LiteFxCmdPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = LITEFX_MAGIC;
        pkt.cmd = CMD_SET_CONFIG;
        pkt.arg_int = 1;
        pkt.config = cfg;
        LiteFxReplyPacket rep;
        if (litefx_send_cmd(&pkt, &rep)) {
            printf("%s\n", msg);
            return 0;
        }
        return 1;
    }

    if (strcmp(cmd, "preset") == 0 && argc >= 3) {
        if (strcmp(argv[2], "list") == 0) {
            char names[64][64];
            int n = litefx_list_presets(names, 64);
            if (n == 0) printf("No saved presets.\n");
            else for (int i = 0; i < n; ++i) printf("%s\n", names[i]);
            return 0;
        }
        if (strcmp(argv[2], "save") == 0 && argc >= 4) {
            int fd = -1;
            LiteFxShm *shm = open_shm_readonly(&fd);
            LiteFxConfig cfg;
            if (shm) {
                cfg = shm->config;
                munmap(shm, sizeof(LiteFxShm));
                close(fd);
            } else {
                char cpath[512];
                litefx_get_default_config_path(cpath, sizeof(cpath));
                if (!litefx_config_load(&cfg, cpath)) litefx_config_set_defaults(&cfg);
            }
            char ppath[512];
            litefx_get_preset_path(argv[3], ppath, sizeof(ppath));
            if (litefx_config_save(&cfg, ppath)) {
                printf("Saved preset '%s' -> %s\n", argv[3], ppath);
                return 0;
            }
            return 1;
        }
        if (strcmp(argv[2], "load") == 0 && argc >= 4) {
            char ppath[512];
            litefx_get_preset_path(argv[3], ppath, sizeof(ppath));
            LiteFxConfig preset_cfg;
            if (!litefx_config_load(&preset_cfg, ppath)) {
                fprintf(stderr, "Preset '%s' not found (%s)\n", argv[3], ppath);
                return 1;
            }
            int fd = -1;
            LiteFxShm *shm = open_shm_readonly(&fd);
            LiteFxConfig cfg;
            if (shm) {
                cfg = shm->config;
                munmap(shm, sizeof(LiteFxShm));
                close(fd);
                litefx_config_apply_preset_only(&cfg, &preset_cfg);
            } else {
                cfg = preset_cfg;
            }
            LiteFxCmdPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.magic = LITEFX_MAGIC;
            pkt.cmd = CMD_SET_CONFIG;
            pkt.arg_int = 1;
            pkt.config = cfg;
            LiteFxReplyPacket rep;
            if (litefx_send_cmd(&pkt, &rep)) {
                printf("Loaded preset '%s'\n", argv[3]);
                return 0;
            }
            return 1;
        }
    }

    if (strcmp(cmd, "reset") == 0) {
        LiteFxConfig def;
        litefx_config_set_defaults(&def);
        LiteFxCmdPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = LITEFX_MAGIC;
        pkt.cmd = CMD_SET_CONFIG;
        pkt.arg_int = 1;
        pkt.config = def;
        LiteFxReplyPacket rep;
        if (litefx_send_cmd(&pkt, &rep)) {
            printf("Reset all settings to factory defaults.\n");
            return 0;
        }
        char cpath[512];
        litefx_get_default_config_path(cpath, sizeof(cpath));
        if (litefx_config_save(&def, cpath)) {
            printf("Reset configuration file to factory defaults (%s).\n", cpath);
            return 0;
        }
        return 1;
    }

    LiteFxCmdPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = LITEFX_MAGIC;

    if (strcmp(cmd, "toggle") == 0) pkt.cmd = CMD_TOGGLE_SINK_BYPASS;
    else if (strcmp(cmd, "toggle-mic") == 0) pkt.cmd = CMD_TOGGLE_SOURCE_BYPASS;
    else if (strcmp(cmd, "import-easyeffects") == 0) pkt.cmd = CMD_IMPORT_EASYEFFECTS;
    else if (strcmp(cmd, "stop") == 0) pkt.cmd = CMD_STOP_DAEMON;
    else {
        print_usage();
        return 1;
    }

    LiteFxReplyPacket rep;
    if (!litefx_send_cmd(&pkt, &rep)) {
        fprintf(stderr, "Failed to contact litefxd (is it running?)\n");
        return 1;
    }
    printf("%s\n", rep.message);
    return 0;
}
