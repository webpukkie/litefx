#ifndef LITEFX_CONFIG_H
#define LITEFX_CONFIG_H

#include "common.h"
#include <stddef.h>

void litefx_config_set_defaults(LiteFxConfig *cfg);
void litefx_config_apply_preset_only(LiteFxConfig *dst, const LiteFxConfig *src);
void litefx_config_resize_sink_eq_bands(LiteFxConfig *cfg, uint32_t new_count);
void litefx_config_resize_source_eq_bands(LiteFxConfig *cfg, uint32_t new_count);

void litefx_get_sock_path(char *buf, size_t len);
void litefx_get_shm_path(char *buf, size_t len);
void litefx_get_config_dir(char *buf, size_t len);
void litefx_get_default_config_path(char *buf, size_t len);
void litefx_get_preset_path(const char *preset_name, char *buf, size_t len);

bool litefx_config_save(const LiteFxConfig *cfg, const char *path);
bool litefx_config_load(LiteFxConfig *cfg, const char *path);
bool litefx_config_import_easyeffects(LiteFxConfig *cfg, char *msg_out, size_t msg_len);
bool litefx_config_import_autoeq(LiteFxConfig *cfg, const char *path, bool is_mic,
                                 char *msg_out, size_t msg_len);

int  litefx_list_presets(char names[][64], int max_presets);
bool litefx_send_cmd(const LiteFxCmdPacket *pkt, LiteFxReplyPacket *reply_out);

uint64_t litefx_now_ns(void);

#endif /* LITEFX_CONFIG_H */
