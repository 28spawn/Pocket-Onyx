/* pa_settings.h — persistent system settings (IMP-10)
 * Stored in the System Settings sector (include/pa_flash_map.h: one 4 KB sector, updated in place), sequence + CRC-protected,
 * written 2 s after the last change so encoder turns don't wear the flash. */
#ifndef PA_SETTINGS_H
#define PA_SETTINGS_H
#include <stdint.h>

#define PA_SETTINGS_MAGIC    0x54455350u /* "PSET" */
#define PA_SETTINGS_VERSION  1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;              /* sizeof(pa_settings_t): lets newer firmware extend the struct */
    uint32_t seq;
    /* Home / navigation */
    uint8_t  last_preset;       /* restored and APPLIED at boot */
    uint8_t  home_view_mode;    /* 0 GRID, 1 LIST */
    uint8_t  play_mode;         /* 0 PRESET, 1 STOMP */
    uint8_t  fizz_level;        /* 0..3 */
    /* Display */
    uint8_t  display_brightness;
    uint8_t  display_auto_dim;
    uint8_t  display_battery_pct;
    uint8_t  theme;             /* 0 ONYX, 1 HIGH CONTRAST */
    /* Input / Output */
    int8_t   input_gain_db;     /* -12..+12 */
    int8_t   gate_db;           /* -80 (off) .. -30 */
    uint8_t  out_level_pct;
    uint8_t  io_output_type;    /* 0 PHONES, 1 LINE */
    uint8_t  io_cab_on_line;
    uint8_t  usb_sample_rate;   /* 0 48 kHz, 1 44.1 kHz */
    uint8_t  bt_enabled;
    uint8_t  tuner_mute;
    uint16_t tuner_ref_hz;
    /* Groove Station */
    uint16_t drum_bpm;
    uint8_t  drum_pattern;
    uint8_t  drum_volume;
    uint8_t  drum_drums_on;
    /* The output limiter's mode. The limiter is the LAST stage of the chain,
     * so its mode decides whether the cab's peaks are limited, softly
     * compressed or left alone - and it used to be hard-wired to the
     * brickwall with no way to A/B it. */
    uint8_t  output_limit_mode;  /* 0 BRICKWALL, 1 SOFT, 2 OFF */
    /* The USB audio loopback: the guitar goes to the host and the host's
     * audio comes back. The stock firmware ships with it ON. */
    uint8_t  usb_loopback;       /* 0 OFF, 1 ON */
    /* Global 5-band EQ (Settings > Global EQ) */
    uint8_t  eq_enabled;
    int8_t   eq_bands_db[5];     /* -12..+12 dB per band */
    /* Stereo output: the width, 0 (mono) .. 100 (the two channels fully decorrelated), STORED AS WIDTH + 1: the record's former reserved byte reads 0 on a unit
     * that was saved before the stereo existed, and 0 means "not set" = PA_STEREO_WIDTH_DEFAULT (the headphone jack is stereo unless the owner turns it off). */
    uint8_t  stereo_width;
    /* Output routing and balance, applied last, just before the DAC (not to the USB capture). 0 STEREO, 1 MONO (L+R to both), 2 LEFT to both,
     * 3 RIGHT to both, 4 SWAP. Balance -50 (left) .. +50 (right): the far channel is turned down linearly, 0 = centre = untouched. */
    uint8_t  out_route;
    int8_t   out_balance;
    /* The USB audio mode (what an audio interface calls direct monitoring): 0 AMP = the unit as an amp (the recording is the amp, the output is the amp plus the host's audio, the stock's way);
     * 1 INTERFACE = the output is ONLY the host's audio, the recording is the dry guitar (no loopback possible); 2 INTERFACE + DIRECT MONITOR = the same, with the dry guitar also heard on the
     * output. Takes the first byte of the former reserved area, so older records read 0 = AMP. */
    uint8_t  usb_mode;
    uint8_t  reserved[7];
    uint32_t crc32;
} pa_settings_t;

void pa_settings_defaults(pa_settings_t *s);
int  pa_settings_load(pa_settings_t *s);        /* 0 = loaded, <0 = defaults returned */
int  pa_settings_save(const pa_settings_t *s);  /* writes the older A/B sector */

#endif
