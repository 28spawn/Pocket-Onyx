/* custom-fw/include/pa_drums.h — Groove Station drum machine
 *
 * The 49 styles (40 grooves + 9 metronome patterns) are the project's own rhythm library,
 * written in tools/gen_drum_patterns.py and generated into src/dsp/drum_patterns.inc.
 *
 * Timing grid: 48 steps to a 4/4 bar, i.e. 40 ticks at PPQ 480 (a 32nd-note triplet).
 * That is the coarsest grid on which both a 16th note (3 steps) and an 8th-note triplet
 * (4 steps) are exact, which the shuffle / swing / 12/8 styles need.  The clock is a 12th
 * of a beat (one 48th of a bar): samples_per_step = 220500 / BPM at 44.1 kHz.
 *
 * Voices: 12 lanes, one voice each, from the project's own synthesised kit
 * (src/dsp/drum_kit.inc, made by tools/gen_drum_kit.py): kick, snare, side stick, closed
 * hat, pedal hat, open hat, crash, ride, ride bell and three toms, plus the metronome
 * click.  The drum voices run at 22.05 kHz and are played back at half speed into the
 * 44.1 kHz output.  The kick and the toms are synthesised as they play, the others are
 * stored compressed (src/dsp/drum_codec.h).
 *
 * Velocity: bits 12..15 of each step word hold the loudest velocity of that step
 * (velocity >> 3).  All voices on the step share it and each voice has its own trim, so
 * backbeat accents and ghost notes survive into the mix.
 */
#ifndef PA_DRUMS_H
#define PA_DRUMS_H

#include <stdint.h>

#define PA_DRUM_STYLE_COUNT 49
#define PA_DRUM_METRO_4_4   43u   /* "METRO 3": 4 clicks per bar */

/* The Groove Station shows the 40 groove styles; the 9 METRO styles are the metronome
 * patterns and the redesign has no click mode.  They are still in the table (and still
 * play as click patterns) but the screen cannot select them. */
#define PA_DRUM_GROOVE_STYLE_COUNT 40u

/* --- timing grid --------------------------------------------------------- */
#define PA_DRUM_STEPS_PER_BAR  48u    /* 40-tick grid: 16ths = 3 steps, 8th triplets = 4 */
#define PA_DRUM_STEP_TICKS     40u
#define PA_DRUM_MAX_STEPS      384u   /* up to eight bars */
#define PA_DRUM_BAR_BLOCKS     16u    /* the step strip: 16 blocks per 4/4 bar */

/* Which of the strip's 16 blocks the playhead is in.  The engine counts 48 steps to the
 * bar, so the strip is `step % 48 / 3`; without the modulo the playhead lit up for the
 * first bar only and then went out for the rest of the loop ("the bar goes away after
 * the 4th, after some time it comes back"). */
static inline uint8_t pa_drums_bar_block(uint16_t step) {
    return (uint8_t)((step % PA_DRUM_STEPS_PER_BAR) / 3u);
}

/* --- voices and lanes ---------------------------------------------------- */
/* Voice indices.  Lane bit N drives voice N, so a step word can be tested against the
 * whole voice set.  CLICK (12) has no lane: it is the metronome voice the METRO styles
 * use. */
enum {
    PA_DRUM_V_KICK = 0,
    PA_DRUM_V_SNARE,
    PA_DRUM_V_RIM,       /* side stick              - pitched metronome click */
    PA_DRUM_V_HAT,       /* closed hi-hat */
    PA_DRUM_V_PEDAL,     /* pedal hi-hat            - truncated hi-hat */
    PA_DRUM_V_OPEN,      /* open hi-hat */
    PA_DRUM_V_CRASH,     /* crash cymbal            - open hat, pitched down */
    PA_DRUM_V_RIDE,      /* ride cymbal             - open hat, pitched down */
    PA_DRUM_V_BELL,      /* ride bell               - hi-hat, pitched down */
    PA_DRUM_V_TOM_LO,    /* low tom                 - tom, pitched down */
    PA_DRUM_V_TOM_MID,
    PA_DRUM_V_TOM_HI,
    PA_DRUM_V_CLICK,     /* metronome click (METRO styles) */
    PA_DRUM_VOICE_COUNT
};

#define PA_DRUM_L_KICK     (1u << PA_DRUM_V_KICK)
#define PA_DRUM_L_SNARE    (1u << PA_DRUM_V_SNARE)
#define PA_DRUM_L_RIM      (1u << PA_DRUM_V_RIM)
#define PA_DRUM_L_HAT      (1u << PA_DRUM_V_HAT)
#define PA_DRUM_L_PEDAL    (1u << PA_DRUM_V_PEDAL)
#define PA_DRUM_L_OPEN     (1u << PA_DRUM_V_OPEN)
#define PA_DRUM_L_CRASH    (1u << PA_DRUM_V_CRASH)
#define PA_DRUM_L_RIDE     (1u << PA_DRUM_V_RIDE)
#define PA_DRUM_L_BELL     (1u << PA_DRUM_V_BELL)
#define PA_DRUM_L_TOM_LO   (1u << PA_DRUM_V_TOM_LO)
#define PA_DRUM_L_TOM_MID  (1u << PA_DRUM_V_TOM_MID)
#define PA_DRUM_L_TOM_HI   (1u << PA_DRUM_V_TOM_HI)
#define PA_DRUM_L_MASK     0x0FFFu

/* Step word: lanes in bits 0..11, the step's loudest MIDI velocity >> 3 in bits 12..15 */
#define PA_DRUM_VEL_SHIFT  12
#define PA_DRUM_VEL_MASK   0x0Fu

/* The drum bus runs at the kit's own rate, 22.05 kHz (see drums_synth.c): a chunk of 4 samples is the same 0.18 ms the 8-sample chunk of
 * the 44.1 kHz bus was, and the look-ahead the same 1.6 ms. */
#define PA_DRUM_LIM_CHUNK      4u
#define PA_DRUM_LIM_CHUNKS     8u                                    /* averaged over 8 chunks */
/* one chunk more than the averaging: the gain has reached its floor one chunk BEFORE the
 * loud chunk leaves the delay, so the per-sample ramp never lets an edge through */
#define PA_DRUM_LIM_LOOKAHEAD  (PA_DRUM_LIM_CHUNK * (PA_DRUM_LIM_CHUNKS + 1u))  /* 36 samples at 22.05 kHz: 1.6 ms */

/* A voice's read position (src/dsp/drum_codec.h): what the decoder or the synthesiser needs to give the next source sample. */
typedef struct {
    uint32_t n;                    /* source samples produced so far */
    uint32_t rng;
    int32_t  pred, sidx;           /* ADPCM predictor and step index */
    float    ph, g, e1, e2, e3;    /* synthesised voices: phase, pitch glide, envelopes */
    float    l1, l2, l3;           /* synthesised voices: filter states */
} pa_dsrc_t;

/* The drum kit: twelve lanes, one synthesised voice each (src/dsp/drum_kit.inc), plus a
 * metronome voice. */
typedef struct {
    uint8_t  playing;              /* 1 = sequencer active */
    uint8_t  style;                /* 0..48 (k_groove_steps / k_groove_off) */
    uint16_t bpm;                  /* Tempo: 40 .. 240 BPM */
    int32_t  level_q21;            /* Master drum mix level in Q21 */

    /* Sequencer clock.  One step = 1/48 of a 4/4 bar = 1/12 of a beat.  The position
     * counts samples in 16.16 and the step advances when it reaches the step length, so
     * a loop is exactly as long as it should be. */
    uint32_t step_pos_q16;
    uint32_t step_len_q16;         /* 220500 / BPM samples, in 16.16 */
    uint32_t samples_per_step;     /* the same, rounded down (for the UI and the tests) */
    uint16_t current_step;         /* 0..k_groove_len[style]-1 */
    uint8_t  armed;                /* 1 from a restart (style change, play) until step 0 is triggered: no step is sounding yet */

    /* Voice playback: the index of the voice's next source sample (the engine runs at the kit's 22.05 kHz, one sample a tick).
     * voice_pos >= the voice's length means the voice is finished. */
    uint32_t voice_pos[PA_DRUM_VOICE_COUNT];
    int32_t  voice_vel[PA_DRUM_VOICE_COUNT];   /* Q15 level of the current hit */
    pa_dsrc_t src[PA_DRUM_VOICE_COUNT];        /* the decoders (the click is plain PCM and does not use its entry) */

    /* Bus-level velocity-driven dynamic 1-pole low-pass filter (alphas are per 44.1 kHz sample; the bus uses the two-sample equivalent) */
    int32_t  lpf_state;        /* Filter output state in Q21 */
    int32_t  lpf_alpha_q15;    /* Current slew-smoothed alpha in Q15 */
    int32_t  target_alpha_q15; /* Step target alpha in Q15 (32768 = wide open) */

    /* The 22.05 kHz bus is brought up to 44.1 kHz by a linear interpolation one bus sample late: the output at an even position is the
     * previous bus sample, at an odd position the mean of the previous and the new one. */
    int32_t  b_prev;           /* the last bus sample, Q21 */
    uint8_t  odd;              /* the next output sample is the second of a pair */

    /* The drum bus limiter (drums_synth.c): a 72-sample look-ahead, so the gain is
     * already down when a stacked hit arrives and never has to chase the waveform */
    int32_t  lim_dly[PA_DRUM_LIM_LOOKAHEAD];   /* the bus, delayed */
    float    lim_ring[PA_DRUM_LIM_CHUNKS + 1]; /* the gain each 8-sample chunk needs */
    float    lim_box[PA_DRUM_LIM_CHUNKS];      /* the released gain, last 8 chunks */
    float    lim_rel;          /* the gain after the hold and the release */
    float    lim_gain;         /* the gain at the end of this chunk */
    float    lim_gain_from;    /* the gain at the end of the last chunk */
    float    lim_peak;         /* the peak of the chunk being filled */
    uint16_t lim_pos;          /* sample index in lim_dly */
    uint16_t lim_quiet;        /* samples since the last non-zero bus sample (the limiter's delay is empty after LOOKAHEAD) */
    uint8_t  lim_ri;           /* chunk index in lim_ring */
    uint8_t  lim_bi;           /* chunk index in lim_box */
    uint8_t  dbg_active;       /* voices sounding in the last tick (for the readout) */
    uint8_t  bus_idle;         /* the last pa_drums_mix_block() added nothing (no voice sounding, bus empty): the caller can skip the sum limiter's input */
} pa_drums_t;

void pa_drums_init(pa_drums_t *d);
void pa_drums_set_bpm(pa_drums_t *d, uint16_t bpm);
void pa_drums_set_style(pa_drums_t *d, uint8_t style);
void pa_drums_set_playing(pa_drums_t *d, uint8_t playing);
void pa_drums_set_level(pa_drums_t *d, uint8_t pct);    /* Groove Station volume */
const char *pa_drums_get_style_name(uint8_t style);

/* Loop length of a style, in steps (48 per 4/4 bar) */
uint16_t pa_drums_style_length(uint8_t style);

/* The step that is SOUNDING now: the last one the clock triggered. current_step is the
 * NEXT step to play, so a playhead drawn from it ran one step (~42 ms at 120 BPM) ahead
 * of the audio and the downbeat block lit before the beat was heard. The screen draws
 * from this one. */
uint16_t pa_drums_sounding_step(const pa_drums_t *d);

/* Decode one voice of the kit from its start into out[] (16-bit units, unrounded), at most `max` source samples; returns how many.  The
 * tests and the generator's cross-checks read the kit through the same decoder the engine plays it with. */
uint32_t pa_drums_voice_decode(uint32_t voice, float *out, uint32_t max);

/* Synthesize and mix drum audio directly into the Q21 output buffer */
void pa_drums_mix_block(pa_drums_t *d, int32_t *q21_buf, int n);

#endif /* PA_DRUMS_H */
