/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * custom-fw/include/pa_debug_log.h — Pocket Amp Real-Time Firmware & Emulator Logger
 */
#ifndef PA_DEBUG_LOG_H
#define PA_DEBUG_LOG_H

#include <stdint.h>

#define PA_LOG_MAX_ENTRIES 128
#define PA_LOG_MSG_LEN     80

typedef struct {
    uint32_t timestamp_ms;
    char     level;       /* 'I' (info), 'W' (warn), 'E' (error) */
    char     module[8];   /* "AUDIO", "PRESET", "NAM", "CAB", "UI", "FLASH" */
    char     msg[PA_LOG_MSG_LEN];
} pa_debug_log_entry_t;

typedef struct {
    float    in_peak_db;
    float    out_peak_db;
    float    in_dc_mv;
    float    out_dc_mv;
    uint32_t clip_count;
    uint32_t xrun_count;
    uint32_t sample_rate;
    uint8_t  active_amp;
    uint8_t  active_cab;
    uint8_t  play_mode;
    uint8_t  active_slots_mask;
} pa_audio_telemetry_t;

void pa_debug_log_init(void);
void pa_debug_log(char level, const char *module, const char *fmt, ...);
uint32_t pa_debug_log_get_entries(pa_debug_log_entry_t *out_entries, uint32_t max_count);
void pa_debug_log_clear(void);

#define PA_LOGI(mod, ...) pa_debug_log('I', mod, __VA_ARGS__)
#define PA_LOGW(mod, ...) pa_debug_log('W', mod, __VA_ARGS__)
#define PA_LOGE(mod, ...) pa_debug_log('E', mod, __VA_ARGS__)

#endif /* PA_DEBUG_LOG_H */
