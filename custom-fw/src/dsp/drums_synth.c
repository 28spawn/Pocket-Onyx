/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/dsp/drums_synth.c — Groove Station drum machine
 *
 * Plays the project's own rhythm library (src/dsp/drum_patterns.inc, written in
 * tools/gen_drum_patterns.py) with the project's own synthesised kit
 * (src/dsp/drum_kit.inc, made by tools/gen_drum_kit.py): kick, snare, side stick, closed
 * hat, pedal hat, open hat, crash, ride, ride bell and three toms - one voice per lane -
 * plus the metronome click.  The drum voices are stored at 22.05 kHz and played back at
 * half speed into the 44.1 kHz output.  No recordings of any product are used.
 *
 * The sequencer clock is exact: the step length is 220500 / BPM samples in 16.16, so a
 * four-bar loop is four bars of wall-clock time at any tempo and the groove never drifts.
 */
#include "pa_drums.h"
#include "pa_config.h"
#include "pa_evlog.h"
#include "fixedpoint.h"

#include "drum_codec.h"
#include "drum_patterns.inc"
#include "drum_kit.inc"

/* The whole drum engine runs at the kit's own rate, 22.05 kHz: every voice advances one source sample a tick (two output samples), the
 * voices are summed, and the bus (low-pass, limiter, level) works on that sum.  Only the finished bus is brought up to the 44.1 kHz
 * output, by a linear interpolation one bus sample late (45 us).  Interpolating the sum is the same arithmetic as interpolating each
 * voice and adding them, so the voices no longer pay for it one by one, and the bus does half the work. */

/* Voice definition: which recording, for how long, at what level.
 *
 *   data     the samples (PCM16 / ADPCM4 / ULAW8) or the synthesiser's constants (KICK / TOM): see drum_codec.h
 *   kind     PA_DK_*
 *   frames   the recording's own length in source samples (22.05 kHz)
 *   trim_q15 per-voice level.  gen_drum_kit.py normalises every voice to the same peak,
 *            so the trims ARE the mix: kick and snare on top, toms 2-3 dB under, the side
 *            stick -5 dB, the cymbals -6..-9 dB and the hats -9..-10 dB.
 *   shift    sample -> Q21 conversion */
typedef struct {
    const void *data;
    uint8_t  kind;
    uint32_t frames;
    int32_t  trim_q15;
    uint8_t  shift;
} pa_drum_voice_def_t;

#define KIT_V(kind, data, n, trim) { &(data), (kind), (n), (trim), 6 }
static const pa_drum_voice_def_t k_voice[PA_DRUM_VOICE_COUNT] = {
    /* KICK    */ KIT_V(PA_DK_KICK,   k_kit_kick,    KIT_KICK_N,    32768),   /*   0 dB */
    /* SNARE   */ KIT_V(PA_DK_ADPCM4, k_kit_snare,   KIT_SNARE_N,   29200),   /*  -1 dB */
    /* RIM     */ KIT_V(PA_DK_ADPCM4, k_kit_rim,     KIT_RIM_N,     18400),   /*  -5 dB */
    /* HAT     */ KIT_V(PA_DK_ADPCM4, k_kit_hat,     KIT_HAT_N,     11600),   /*  -9 dB */
    /* PEDAL   */ KIT_V(PA_DK_ADPCM4, k_kit_pedal,   KIT_PEDAL_N,   10400),   /* -10 dB */
    /* OPEN    */ KIT_V(PA_DK_ULAW8,  k_kit_open,    KIT_OPEN_N,    13000),   /*  -8 dB */
    /* CRASH   */ KIT_V(PA_DK_ULAW8,  k_kit_crash,   KIT_CRASH_N,   16400),   /*  -6 dB */
    /* RIDE    */ KIT_V(PA_DK_ADPCM4, k_kit_ride,    KIT_RIDE_N,    11600),   /*  -9 dB */
    /* BELL    */ KIT_V(PA_DK_ADPCM4, k_kit_bell,    KIT_BELL_N,    11600),   /*  -9 dB */
    /* TOM_LO  */ KIT_V(PA_DK_TOM,    k_kit_tom_lo,  KIT_TOM_LO_N,  26000),   /*  -2 dB */
    /* TOM_MID */ KIT_V(PA_DK_TOM,    k_kit_tom_mid, KIT_TOM_MID_N, 24500),   /* -2.5 dB */
    /* TOM_HI  */ KIT_V(PA_DK_TOM,    k_kit_tom_hi,  KIT_TOM_HI_N,  23200),   /*  -3 dB */
    /* CLICK   */ KIT_V(PA_DK_PCM16,  k_kit_click,   KIT_CLICK_N,   29491),
};

/* MIDI velocity (the step's 4-bit field, i.e. MIDI velocity >> 3) to linear amplitude,
 * Q15.  A 1.5-power curve: ghost notes (~40 of 127) come out around 18 % and accents
 * (~120) around 95 %. */
static const int32_t k_vel_tab[16] = {
      183,   952,  2048,  3392,  4945,  6682,  8585, 10641,
    12838, 15169, 17626, 20203, 22895, 25697, 28604, 31614,
};

#define LPF_ALPHA_MIN 16384        /* 0.5: the warmest setting, used for ghost notes */
#define LPF_ALPHA_STEP  1092       /* + 1/30 per velocity step -> ~1.0 at full velocity */

/* The drum bus limiter (2026-10-23, measured by custom-fw/test/test_drum_clip.c).
 *
 * It replaces a compressor whose 4-sample peak follower and 2-sample gain slew rode the
 * waveform of every loud low voice: a full-velocity kick came out with its distortion only
 * 22 dB under the signal (~8 %), the toms 28..32 dB, the click 28 dB - the "clipping" in the
 * Groove Station.  The kit is balanced by the per-voice trims, so a single hit of any voice
 * sits under the ceiling and passes untouched; the limiter only catches stacked hits (kick +
 * crash + snare on one step) that would go over:
 *   - a 72-sample (1.6 ms) look-ahead delay on the bus
 *   - per 8-sample chunk, the gain that chunk needs (ceiling / peak, <= 1)
 *   - held for 9 chunks (the look-ahead + one), released over ~80 ms
 *   - averaged over the last 8 chunks, then interpolated per sample: the gain is at its
 *     floor before the loud chunk leaves the delay, and it moves without steps
 * Below the ceiling the gain is exactly 1 and the bus is a pure delay. */
#define DRUM_LIM_CEIL   (0.89125f * 2097152.0f)       /* -1 dBFS at the bus (before LEVEL) */
#define DRUM_LIM_REL    0.99102f                       /* per chunk: ~80 ms release */

const char *pa_drums_get_style_name(uint8_t style) {
    if (style >= PA_DRUM_STYLE_COUNT) style = 0;
    return k_groove_names[style];
}

uint16_t pa_drums_style_length(uint8_t style) {
    return k_groove_len[style % PA_DRUM_STYLE_COUNT];
}

uint16_t pa_drums_sounding_step(const pa_drums_t *d) {
    if (!d) return 0;
    if (d->armed) return 0;     /* just restarted: current_step 0 is the NEXT step, not "the step after the last one" - the old maths lit the LAST strip block for a moment after every pattern change */
    uint16_t len = k_groove_len[d->style % PA_DRUM_STYLE_COUNT];
    uint16_t cur = (d->current_step < len) ? d->current_step : 0;
    return (uint16_t)((cur + len - 1u) % len);
}

void pa_drums_set_bpm(pa_drums_t *d, uint16_t bpm) {
    if (!d) return;
    if (bpm < 40u)  bpm = 40u;
    if (bpm > 240u) bpm = 240u;
    d->bpm = bpm;
    /* One step is 1/48 of a 4/4 bar = 1/12 of a beat, so at 44.1 kHz:
     * (44100 * 60) / (12 * bpm) = 220500 / bpm samples.  The length is kept in 16.16, so
     * a step is exact to 1/65536 of a sample: a four-bar loop is four bars of wall-clock
     * time at any tempo, and the groove cannot drift. */
    d->step_len_q16 = (uint32_t)(((uint64_t)220500u << 16) / (uint64_t)bpm);
    d->samples_per_step = 220500u / (uint32_t)bpm;
}

static inline void trigger_voice(pa_drums_t *d, uint32_t v, int32_t vel_q15) {
    d->voice_pos[v] = 0;
    if (k_voice[v].kind != PA_DK_PCM16) pa_dsrc_start(&d->src[v], k_voice[v].kind, k_voice[v].data);
    d->voice_vel[v] = (int32_t)(((int64_t)vel_q15 * k_voice[v].trim_q15) >> 15);
}

/* Stop a voice where it stands (voice_pos is the next source sample's index, so this parks it at the end of its recording). */
static inline void choke_voice(pa_drums_t *d, uint32_t v) {
    d->voice_pos[v] = k_voice[v].frames;
}

/* The kit's voices decoded from their start: for the tests, which read the kit through the same decoder the engine plays it with. */
uint32_t pa_drums_voice_decode(uint32_t voice, float *out, uint32_t max) {
    if (voice >= PA_DRUM_VOICE_COUNT || !out) return 0;
    const pa_drum_voice_def_t *vd = &k_voice[voice];
    pa_dsrc_t st;
    pa_dsrc_start(&st, vd->kind, vd->data);
    const uint32_t n = (vd->frames < max) ? vd->frames : max;
    for (uint32_t i = 0; i < n; i++) out[i] = pa_dsrc_next(&st, vd->kind, vd->data, vd->frames);
    return n;
}

void pa_drums_init(pa_drums_t *d) {
    if (!d) return;
    d->playing         = 0;
    d->style           = 0;
    d->level_q21       = PA_Q21_ONE; /* Full studio mix level */
    d->step_pos_q16    = 0;
    d->current_step    = 0;
    d->armed           = 1;

    for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++) {
        choke_voice(d, v);              /* every voice starts finished */
        d->voice_vel[v] = 32768;
    }

    d->b_prev           = 0;
    d->odd              = 0;
    d->dbg_active       = 0;
    d->lpf_state        = 0;
    d->lpf_alpha_q15    = 32768; /* Start wide open (1.0) */
    d->target_alpha_q15 = 32768;

    for (uint32_t k = 0; k < PA_DRUM_LIM_LOOKAHEAD; k++) d->lim_dly[k] = 0;
    for (uint32_t k = 0; k <= PA_DRUM_LIM_CHUNKS; k++) d->lim_ring[k] = 1.0f;
    for (uint32_t k = 0; k < PA_DRUM_LIM_CHUNKS; k++) d->lim_box[k] = 1.0f;
    d->lim_rel  = 1.0f;
    d->lim_gain = 1.0f;
    d->lim_gain_from = 1.0f;
    d->lim_bi   = 0;
    d->lim_peak = 0.0f;
    d->lim_pos  = 0;
    d->lim_quiet = 0;
    d->lim_ri   = 0;

    pa_drums_set_bpm(d, 120);
}

void pa_drums_set_style(pa_drums_t *d, uint8_t style) {
    if (!d) return;
    style %= PA_DRUM_STYLE_COUNT;
    if (style != d->style) {
        d->style        = style;
        d->current_step = 0;    /* patterns have different lengths; restart the loop */
        d->armed        = 1;
    }
}

void pa_drums_set_playing(pa_drums_t *d, uint8_t playing) {
    if (!d) return;
    if (playing && !d->playing) {
        d->current_step = 0;
        d->armed        = 1;
        d->step_pos_q16 = d->step_len_q16 - 0x20000u; /* trigger step 0 on the first tick */
    }
    d->playing = playing ? 1u : 0u;
}

void pa_drums_set_level(pa_drums_t *d, uint8_t pct) {
    if (!d) return;
    if (pct > 100u) pct = 100u;
    d->level_q21 = (int32_t)(((int64_t)pct * (PA_Q21_ONE * 7 / 10)) / 100);   /* 100 % = -3 dB */
}

/* One step of the pattern.  The velocity field is the loudest hit on that step, so a
 * backbeat snare, or a hi-hat doubling a kick, keeps its accent. */
static void trigger_step(pa_drums_t *d, uint32_t word) {
    uint32_t lanes = word & PA_DRUM_L_MASK;
    if (!lanes) return;

    uint32_t nib = (word >> PA_DRUM_VEL_SHIFT) & PA_DRUM_VEL_MASK;
    int32_t  vel = k_vel_tab[nib];

    /* Dynamic low-pass: accents play wide open, ghost notes are damped for warmth */
    d->target_alpha_q15 = LPF_ALPHA_MIN + (int32_t)nib * LPF_ALPHA_STEP;

    /* The METRO styles are click patterns: they play the metronome voice with the step's
     * own accent (METRO 3 accents beat 1) */
    if (d->style >= PA_DRUM_GROOVE_STYLE_COUNT) {
        trigger_voice(d, PA_DRUM_V_CLICK, vel);
        return;
    }

    for (uint32_t v = 0; v <= PA_DRUM_V_TOM_HI; v++) {
        if (lanes & (1u << v)) trigger_voice(d, v, vel);
    }

    /* A closed or pedal hi-hat chokes the open hat, as a real one does */
    if ((lanes & (PA_DRUM_L_HAT | PA_DRUM_L_PEDAL)) && !(lanes & PA_DRUM_L_OPEN)) {
        choke_voice(d, PA_DRUM_V_OPEN);
    }
}

/* Add the voices' next ticks [a, b) to mixf[] (the bus rate, 22.05 kHz: one source sample a tick).  Voice by voice, not tick by tick: a voice's
 * recording, decoder and velocity stay in registers for its whole run, and a finished voice costs one compare. */
static void render_voices(pa_drums_t *d, float *mixf, int a, int b) {
    if (b <= a) return;
    for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++) {
        const pa_drum_voice_def_t *vd = &k_voice[v];
        uint32_t idx = d->voice_pos[v];
        const uint32_t frames = vd->frames;
        if (idx >= frames) continue;                                   /* finished */
        /* velocity, the Q15 -> 1.0 scale and the sample's shift folded into one float */
        const float vel = (float)d->voice_vel[v] * (1.0f / 32768.0f) * (float)(1u << vd->shift);
        uint32_t end = idx + (uint32_t)(b - a);
        if (end > frames) end = frames;
        int j = a;
        if (vd->kind == PA_DK_PCM16) {
            const int16_t *pcm = (const int16_t *)vd->data;
            for (; idx < end; idx++, j++) mixf[j] += (float)pcm[idx] * vel;
        } else {
            pa_dsrc_t *st = &d->src[v];
            const uint8_t kind = vd->kind;
            const void *data = vd->data;
            for (; idx < end; idx++, j++) mixf[j] += pa_dsrc_next(st, kind, data, frames) * vel;
        }
        d->voice_pos[v] = idx;
    }
}

static int drums_idle(const pa_drums_t *d) {
    if (d->lpf_state != 0 || d->lim_quiet < PA_DRUM_LIM_LOOKAHEAD) return 0;
    if (d->lim_gain < 1.0f || d->lim_gain_from < 1.0f || d->lim_rel < 1.0f) return 0;
    for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++)
        if (d->voice_pos[v] < k_voice[v].frames) return 0;
    return 1;
}

#if PA_DRUMDBG_ON
/* the readout's count of voices sounding, and of each kind of decoder (jl_ram_read.py shows the most at once) */
static void drums_dbg_voices(const pa_drums_t *d) {
    uint32_t act = 0, ad = 0, ul = 0, sy = 0;
    for (uint32_t v = 0; v < PA_DRUM_VOICE_COUNT; v++) {
        if (d->voice_pos[v] >= k_voice[v].frames) continue;
        act++;
        const uint8_t k = k_voice[v].kind;
        if (k == PA_DK_ADPCM4) ad++; else if (k == PA_DK_ULAW8) ul++; else if (k == PA_DK_KICK || k == PA_DK_TOM) sy++;
    }
    pa_drumdbg_voices(act, ad, ul, sy);
}
#define PA_DRUMDBG_VOICES(d) drums_dbg_voices(d)
#else
#define PA_DRUMDBG_VOICES(d) ((void)0)
#endif

#define DRUMS_SUB 128                                                  /* the longest block (output samples) the mixer takes in one go */
#define DRUMS_TICKS (DRUMS_SUB / 2 + 1)                                /* bus samples (22.05 kHz) in one go: the odd/even phase can add one */

static void drums_mix_sub(pa_drums_t *d, int32_t *q21_buf, int n) {
    const uint16_t len = k_groove_len[d->style];
    float   mixf[DRUMS_TICKS];
    int32_t busq[DRUMS_TICKS];
    /* a bus sample is made at every second output sample (the second of each pair); `odd` says the next output is one of those */
    const int T = ((int)d->odd + n) >> 1;
    d->bus_idle = 0u;
    for (int i = 0; i < T; i++) mixf[i] = 0.0f;
    PA_DRUMDBG_T(t0);

    /* 1. The step clock and the voices. The clock counts output samples in 16.16 (two to a tick) and the step advances when it reaches
     * the step length, so the loop is exact; a hit starts on the tick it falls in (at most one output sample, 23 us, late). The voices
     * up to a trigger are rendered before it, the new ones after it. */
    int seg = 0;
    for (int t = 0; t < T; t++) {
        d->step_pos_q16 += 0x20000u;
        if (d->step_pos_q16 >= d->step_len_q16) {
            render_voices(d, mixf, seg, t);
            seg = t;
            d->step_pos_q16 -= d->step_len_q16;
            uint16_t step = d->current_step;
            if (step >= len) step = 0;                /* style changed to a shorter one */
            trigger_step(d, k_groove_steps[k_groove_off[d->style] + step]);
            d->current_step = (uint16_t)((step + 1u) % len);
            d->armed = 0;
        }
    }
    render_voices(d, mixf, seg, T);
    PA_DRUMDBG_T(t1);
    PA_DRUMDBG_VOICES(d);
    PA_DRUMDBG_TIME(0, t1 - t0);
    if (drums_idle(d)) {                          /* nothing sounding and the bus empty: a silent bus adds nothing, and between hits there is no work */
        d->bus_idle = 1u;
        d->b_prev = 0;
        d->odd = (uint8_t)((d->odd + n) & 1);
        return;
    }

    /* 2. The bus, at 22.05 kHz: the dynamic low-pass, the look-ahead limiter, the level. The state lives in locals for the whole block
     * (written back at the end): every d->field in this loop was a memory access of 7-10 cycles on this core, several per sample, and the
     * compiler cannot keep them in registers because the stores to lim_dly[] and busq[] might alias the struct. */
    const float level_f = (float)d->level_q21 * (1.0f / 2097152.0f);
    int32_t  lpf_alpha = d->lpf_alpha_q15, lpf_state = d->lpf_state;
    const int32_t lpf_target = d->target_alpha_q15;
    float    lim_peak = d->lim_peak, lim_gain = d->lim_gain, lim_gain_from = d->lim_gain_from, lim_rel = d->lim_rel;
    uint32_t lim_quiet = d->lim_quiet, lim_pos = d->lim_pos, lim_ri = d->lim_ri, lim_bi = d->lim_bi;
    for (int i = 0; i < T; i++) {
        /* Slew alpha smoothly towards the step target (a ~32-sample slew constant at this rate, the 64 of 44.1 kHz: no zipper noise) */
        lpf_alpha += (lpf_target - lpf_alpha) >> 5;

        float mf = mixf[i];
        if (mf > 2.0e9f) mf = 2.0e9f; else if (mf < -2.0e9f) mf = -2.0e9f;
        int32_t mix = (int32_t)mf;

        /* Bus-level dynamic 1-pole low-pass filter: y[n] = y[n-1] + alpha * (x[n] - y[n-1]), with the alpha of two 44.1 kHz samples:
         * a2 = 1 - (1 - a)^2 = a (2 - a) */
        if (mix != 0 || lpf_state != 0) {
            const float af = (float)lpf_alpha * (1.0f / 32768.0f);
            int32_t diff = mix - lpf_state;
            lpf_state += (int32_t)((float)diff * (af * (2.0f - af)));
            mix = lpf_state;
        }

        /* The drum bus limiter (see the top of the file) */
        {
            float a = (float)mix;
            if (a < 0.0f) a = -a;
            if (a > lim_peak) lim_peak = a;
            if (mix != 0) lim_quiet = 0u; else if (lim_quiet < 0xFFFFu) lim_quiet++;

            const uint32_t pos = lim_pos;
            const int32_t delayed = d->lim_dly[pos];
            d->lim_dly[pos] = mix;
            lim_pos = (pos + 1u >= PA_DRUM_LIM_LOOKAHEAD) ? 0u : pos + 1u;                 /* a compare, not a modulo */

            /* the gain for this sample: from the last chunk's end towards the next one's */
            const uint32_t k = pos % PA_DRUM_LIM_CHUNK;
            if (k == 0u) {
                /* a new chunk starts: work out where its gain has to be by its end */
                lim_gain_from = lim_gain;
                float need = (lim_peak > DRUM_LIM_CEIL) ? DRUM_LIM_CEIL / lim_peak : 1.0f;
                lim_peak = 0.0f;
                lim_ri = (lim_ri + 1u > PA_DRUM_LIM_CHUNKS) ? 0u : lim_ri + 1u;
                d->lim_ring[lim_ri] = need;
                float hold = 1.0f;
                for (uint32_t r = 0; r <= PA_DRUM_LIM_CHUNKS; r++)
                    if (d->lim_ring[r] < hold) hold = d->lim_ring[r];
                lim_rel = (hold < lim_rel) ? hold : hold + (lim_rel - hold) * DRUM_LIM_REL;
                lim_bi = (lim_bi + 1u) & (PA_DRUM_LIM_CHUNKS - 1u);
                d->lim_box[lim_bi] = lim_rel;
                float sum = 0.0f;
                for (uint32_t r = 0; r < PA_DRUM_LIM_CHUNKS; r++) sum += d->lim_box[r];
                lim_gain = sum * (1.0f / (float)PA_DRUM_LIM_CHUNKS);
            }
            const float g = lim_gain_from + (lim_gain - lim_gain_from)
                          * ((float)(k + 1u) * (1.0f / (float)PA_DRUM_LIM_CHUNK));
            mix = (g >= 1.0f) ? delayed : (int32_t)((float)delayed * g);
        }

        busq[i] = (int32_t)((float)mix * level_f);                      /* the level as a float multiply, not a 64-bit one */
    }
    d->lpf_alpha_q15 = lpf_alpha; d->lpf_state = lpf_state;
    d->lim_peak = lim_peak; d->lim_gain = lim_gain; d->lim_gain_from = lim_gain_from; d->lim_rel = lim_rel;
    d->lim_quiet = (uint16_t)lim_quiet; d->lim_pos = (uint16_t)lim_pos; d->lim_ri = (uint8_t)lim_ri; d->lim_bi = (uint8_t)lim_bi;
    PA_DRUMDBG_T(t2);
    PA_DRUMDBG_TIME(1, t2 - t1);

    /* 3. Up to 44.1 kHz: linear interpolation, one bus sample late. An even output is the previous bus sample, an odd one the mean of the
     * previous and the new. */
    uint32_t odd = d->odd;
    int32_t  bprev = d->b_prev;
    int t = 0;
    for (int j = 0; j < n; j++) {
        int32_t y;
        if (!odd) {
            y = bprev; odd = 1u;
        } else {
            const int32_t bn = busq[t++];
            y = (int32_t)(((int64_t)bprev + bn) >> 1);
            bprev = bn; odd = 0u;
        }
        q21_buf[j] = sat32((int64_t)q21_buf[j] + y);
    }
    d->odd = (uint8_t)odd; d->b_prev = bprev;
}

void pa_drums_mix_block(pa_drums_t *d, int32_t *q21_buf, int n) {
    if (!d || !d->playing || !q21_buf) return;
    while (n > 0) {
        const int m = (n > DRUMS_SUB) ? DRUMS_SUB : n;
        drums_mix_sub(d, q21_buf, m);
        q21_buf += m; n -= m;
    }
}
