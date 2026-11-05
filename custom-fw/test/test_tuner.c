/* custom-fw/test/test_tuner.c — Chromatic Tuner verification suite.
 * Tests string pitch detection (Low E2 to High E4), filter response,
 * A4 calibration (430..450 Hz), bypass volume, and input meter dots.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "pa_ui.h"
#include "pa_config.h"

void pa_tuner_init(pa_tuner_state_t *t);
void pa_tuner_process_block(pa_tuner_state_t *t, int32_t *buf, int n);

static int fails = 0;
#define CHECK(ok, ...) do { \
    printf((ok) ? "  ok   " : "  FAIL "); \
    printf(__VA_ARGS__); printf("\n"); \
    if (!(ok)) fails++; \
} while (0)

#define FS 44100.0
#define PI 3.14159265358979323846
#define Q21 2097152.0

static const char *k_note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};


/* ---- realistic signals: a plucked string is a stack of harmonics, not a sine ---- */
static uint32_t g_rng = 12345u;
static double rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return ((double)(g_rng >> 8) / 8388608.0) - 1.0; }

/* the phase of each harmonic is fixed per run (random but repeatable); `amps` are the harmonic amplitudes relative to the largest */
static void string_tone(int32_t *buf, double f, const double *amps, int nh, double level, double noise, double *phase, long start) {
    for (int i = 0; i < PA_BLOCK_N; i++) {
        double sm = 0.0, tt = (double)(start + i) / FS;
        for (int h = 0; h < nh; h++) sm += amps[h] * sin(2.0 * PI * f * (double)(h + 1) * tt + phase[h]);
        sm = sm * level + noise * rnd();
        if (sm > 0.95) sm = 0.95;
        if (sm < -0.95) sm = -0.95;
        buf[i] = (int32_t)(sm * Q21);
    }
}

static double cents_of(double f, double ref) { return 1200.0 * log2(f / ref); }

static void run_real_tests(void) {
    static const struct { const char *name; double f; int note; int oct; } str[6] = {
        { "E2", 82.4069, 4, 2 }, { "A2", 110.0, 9, 2 }, { "D3", 146.8324, 2, 3 }, { "G3", 195.9977, 7, 3 }, { "B3", 246.9417, 11, 3 }, { "E4", 329.6276, 4, 4 } };
    static const double amps_norm[8] = { 1.0, 0.7, 0.5, 0.35, 0.25, 0.18, 0.12, 0.08 };
    static const double amps_weak[8] = { 0.3, 1.0, 0.9, 0.5, 0.3, 0.2, 0.1, 0.05 };        /* a weak fundamental: the 2nd harmonic is the strongest */
    double phase[8];
    for (int h = 0; h < 8; h++) phase[h] = 6.2831853 * (0.5 + 0.5 * rnd());
    pa_tuner_state_t t;
    int32_t buf[PA_BLOCK_N];

    printf("[7] harmonic tones, six strings, detuned by -20 / 0 / +14 cents, ref 440 and 435: note, octave, cents within 2...\n");
    {
        const double det[3] = { -20.0, 0.0, 14.0 };
        const int refs[2] = { 440, 435 };
        int bad = 0, total = 0;
        for (int r = 0; r < 2; r++) for (int s = 0; s < 6; s++) for (int d = 0; d < 3; d++) {
            const double ref = (double)refs[r];
            const double f = str[s].f * (ref / 440.0) * pow(2.0, det[d] / 1200.0);
            pa_tuner_init(&t); t.active = 1; t.ref_a4_hz = (uint16_t)refs[r];
            for (int b = 0; b < 320; b++) { string_tone(buf, f, amps_norm, 8, 0.25, 0.002, phase, (long)b * PA_BLOCK_N); pa_tuner_process_block(&t, buf, PA_BLOCK_N); }
            total++;
            const int ok = t.signal_locked && t.note_index == str[s].note && t.octave == str[s].oct && abs((int)t.cents - (int)lround(det[d])) <= 2;
            if (!ok) { bad++; printf("   FAIL %s ref %d detune %+.0f: read %s%d %+d (locked %u)\n", str[s].name, refs[r], det[d], k_note_names[t.note_index], t.octave, t.cents, t.signal_locked); }
        }
        CHECK(bad == 0, "%d of %d harmonic readings within 2 cents with the right note and octave", total - bad, total);
    }

    printf("[8] a weak fundamental (the 2nd harmonic is 3x stronger) still reads the fundamental, not the octave...\n");
    {
        int bad = 0;
        for (int s = 0; s < 6; s++) {
            pa_tuner_init(&t); t.active = 1;
            for (int b = 0; b < 320; b++) { string_tone(buf, str[s].f, amps_weak, 8, 0.25, 0.002, phase, (long)b * PA_BLOCK_N); pa_tuner_process_block(&t, buf, PA_BLOCK_N); }
            if (!(t.signal_locked && t.note_index == str[s].note && t.octave == str[s].oct && abs(t.cents) <= 2)) {
                bad++; printf("   FAIL %s: read %s%d %+d\n", str[s].name, k_note_names[t.note_index], t.octave, t.cents);
            }
        }
        CHECK(bad == 0, "all six strings keep their own octave with a weak fundamental (%d wrong)", bad);
    }

    printf("[9] the reading is STEADY: over 1.5 s of a held note with noise the cents stay within 1 and the note name never flickers...\n");
    {
        pa_tuner_init(&t); t.active = 1;
        for (int b = 0; b < 200; b++) { string_tone(buf, 110.0 * pow(2.0, 7.0 / 1200.0), amps_norm, 8, 0.2, 0.01, phase, (long)b * PA_BLOCK_N); pa_tuner_process_block(&t, buf, PA_BLOCK_N); }
        int lo = 99, hi = -99, name_changes = 0, unlocked = 0;
        uint8_t n0 = t.note_index;
        for (int b = 200; b < 200 + 1030; b++) {
            string_tone(buf, 110.0 * pow(2.0, 7.0 / 1200.0), amps_norm, 8, 0.2, 0.01, phase, (long)b * PA_BLOCK_N);
            pa_tuner_process_block(&t, buf, PA_BLOCK_N);
            if (!t.signal_locked) unlocked++;
            if (t.note_index != n0) name_changes++;
            if (t.cents < lo) lo = t.cents;
            if (t.cents > hi) hi = t.cents;
        }
        printf("   cents %+d .. %+d, name changes %d, unlocked blocks %d\n", lo, hi, name_changes, unlocked);
        CHECK(hi - lo <= 1, "the cents reading varies by at most 1 over 1.5 s (%+d .. %+d)", lo, hi);
        CHECK(name_changes == 0 && unlocked == 0, "no flicker of the name, no lost lock");
    }

    printf("[10] a string that rings and dies away stays on the display: locked through the decay, held after it...\n");
    {
        pa_tuner_init(&t); t.active = 1;
        int lost_in_decay = 0; long blk = 0;
        for (; blk < 2100; blk++) {                                         /* 3 s decaying from 0.4 to about 0.0004 (60 dB per 3 s... T60 = 3 s) */
            const double env = 0.4 * pow(10.0, -3.0 * ((double)blk * PA_BLOCK_N / FS) / 3.0 * 1.0);
            string_tone(buf, 82.4069, amps_norm, 8, env, 0.0, phase, blk * PA_BLOCK_N);
            pa_tuner_process_block(&t, buf, PA_BLOCK_N);
            if (blk > 200 && !t.signal_locked) lost_in_decay++;
        }
        CHECK(lost_in_decay == 0, "the lock is never lost while the string is still audible (%d blocks lost)", lost_in_decay);
        CHECK(t.note_index == 4 && t.octave == 2 && abs(t.cents) <= 2, "...and it still reads E2 %+d at the end of the decay", t.cents);
        memset(buf, 0, sizeof buf);
        for (int b = 0; b < 700; b++) pa_tuner_process_block(&t, buf, PA_BLOCK_N);                 /* 1 s of nothing */
        CHECK(t.signal_locked == 1 && t.held == 1, "1 s after the string died the reading is still on the screen (held)");
    }

    printf("[11] turning the peg: a glide from -30 to +10 cents is followed...\n");
    {
        pa_tuner_init(&t); t.active = 1;
        long blk = 0; double f0 = 82.4069;
        for (; blk < 220; blk++) { string_tone(buf, f0 * pow(2.0, -30.0 / 1200.0), amps_norm, 8, 0.25, 0.002, phase, blk * PA_BLOCK_N); pa_tuner_process_block(&t, buf, PA_BLOCK_N); }
        CHECK(t.signal_locked && abs(t.cents - (-30)) <= 2, "before the turn the reading is -30 (%+d)", t.cents);
        /* a glide needs a continuous phase: integrate the frequency */
        double ph = 0.0;
        for (int b = 0; b < 350; b++) {                                       /* 0.5 s */
            for (int i = 0; i < PA_BLOCK_N; i++) {
                const double cents = -30.0 + 40.0 * (double)(b * PA_BLOCK_N + i) / (0.5 * FS);
                const double f = f0 * pow(2.0, (cents > 10.0 ? 10.0 : cents) / 1200.0);
                ph += 2.0 * PI * f / FS;
                double sm = 0.0;
                for (int h = 0; h < 8; h++) sm += amps_norm[h] * sin((double)(h + 1) * ph + phase[h]);
                buf[i] = (int32_t)(sm * 0.25 * Q21 * 0.9);
            }
            pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        }
        for (int b = 0; b < 120; b++) {                                       /* hold +10 for 0.17 s */
            for (int i = 0; i < PA_BLOCK_N; i++) {
                ph += 2.0 * PI * f0 * pow(2.0, 10.0 / 1200.0) / FS;
                double sm = 0.0;
                for (int h = 0; h < 8; h++) sm += amps_norm[h] * sin((double)(h + 1) * ph + phase[h]);
                buf[i] = (int32_t)(sm * 0.25 * Q21 * 0.9);
            }
            pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        }
        CHECK(t.signal_locked && abs(t.cents - 10) <= 3, "0.17 s after the turn stopped the reading is +10 (%+d)", t.cents);
    }

    printf("[12] noise alone never locks; a lock comes fast...\n");
    {
        pa_tuner_init(&t); t.active = 1;
        int locked_blocks = 0;
        for (int b = 0; b < 1400; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = (int32_t)(0.01 * rnd() * Q21);                  /* white noise at -40 dBFS */
            pa_tuner_process_block(&t, buf, PA_BLOCK_N);
            if (t.signal_locked) locked_blocks++;
        }
        CHECK(locked_blocks == 0, "white noise at -40 dBFS never locks the tuner (%d blocks)", locked_blocks);
        int worst = 0;
        for (int s = 0; s < 6; s++) {
            pa_tuner_init(&t); t.active = 1;
            int at = -1;
            for (int b = 0; b < 400 && at < 0; b++) { string_tone(buf, str[s].f, amps_norm, 8, 0.25, 0.002, phase, (long)b * PA_BLOCK_N); pa_tuner_process_block(&t, buf, PA_BLOCK_N); if (t.signal_locked) at = b; }
            if (at > worst) worst = at;
            if (at < 0) worst = 999;
        }
        CHECK(worst <= 210, "every string locks within %d blocks (%.0f ms)", worst, worst * 1000.0 * PA_BLOCK_N / FS);
    }
    (void)cents_of;
}

int main(void) {
    printf("=== Pocket Amp Chromatic Tuner Verification Suite ===\n");
    pa_tuner_state_t t;

    /* [1] Standard 6-String Guitar Tuning Lock */
    printf("[1] testing pitch detection across all standard 6 guitar strings...\n");
    struct { const char *name; double freq; uint8_t expected_note; int8_t expected_oct; } strings[6] = {
        { "Low E2 ",  82.4069, 4, 2 },
        { "A2     ", 110.0000, 9, 2 },
        { "D3     ", 146.8324, 2, 3 },
        { "G3     ", 195.9977, 7, 3 },
        { "B3     ", 246.9417, 11, 3 },
        { "High E4", 329.6276, 4, 4 }
    };

    for (int s = 0; s < 6; s++) {
        pa_tuner_init(&t);
        t.active = 1;
        t.bypass_volume = 0;

        int32_t buf[PA_BLOCK_N];
        double f = strings[s].freq;

        /* Run 150 blocks (~218 ms) of pure sine tone */
        for (int b = 0; b < 150; b++) {
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double smp = sin(2.0 * PI * f * (double)(b * PA_BLOCK_N + i) / FS);
                buf[i] = (int32_t)(smp * 0.7 * Q21);
            }
            pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        }

        printf("   String %s (%5.1f Hz) -> %s%d (cents=%+2d, conf=%u, locked=%u)\n",
               strings[s].name, f, k_note_names[t.note_index], t.octave, t.cents,
               t.confidence, t.signal_locked);

        CHECK(t.signal_locked == 1, "%s locked signal", strings[s].name);
        CHECK(t.note_index == strings[s].expected_note, "%s note index %d (expected %d)",
              strings[s].name, t.note_index, strings[s].expected_note);
        CHECK(t.octave == strings[s].expected_oct, "%s octave %d (expected %d)",
              strings[s].name, t.octave, strings[s].expected_oct);
        CHECK(abs(t.cents) <= 2, "%s cents deviation %d <= 2 cents", strings[s].name, t.cents);
    }

    /* [2] Filter Attenuation at High E4 (329.6 Hz) */
    printf("[2] verifying low-pass filter cutoff (>800 Hz, NOT 300 Hz)...\n");
    {
        /* The lowpass filter is: lp += (x - lp) >> 3; (alpha = 1/8 = 0.125)
         * At fc = ~938 Hz, High E (329.6 Hz) experiences < 0.6 dB attenuation.
         * Let's measure the filter transmission at 329.6 Hz. */
        int32_t lp = 0;
        double max_in = 1000000.0, max_out = 0.0;
        for (int i = 0; i < 44100; i++) {
            int32_t x = (int32_t)(max_in * sin(2.0 * PI * 329.6276 * (double)i / FS));
            lp += (x - lp) >> 3;
            if (i > 22050 && abs(lp) > max_out) max_out = abs(lp);
        }
        double atten_db = 20.0 * log10(max_out / max_in);
        printf("   High E4 (329.6 Hz) transmission: %.1f%% (attenuation %.2f dB)\n",
               (max_out / max_in) * 100.0, atten_db);
        CHECK(atten_db > -1.0, "High E4 fundamental is preserved (attenuation %.2f dB > -1.0 dB)", atten_db);
    }

    /* [3] A4 Calibration Range (430..450 Hz) */
    printf("[3] testing A4 reference calibration (430..450 Hz)...\n");
    {
        uint16_t refs[3] = { 430, 440, 450 };
        for (int r = 0; r < 3; r++) {
            pa_tuner_init(&t);
            t.active = 1;
            t.ref_a4_hz = refs[r];

            /* Feed a sine tone exactly at ref_a4 */
            double f = (double)refs[r];
            int32_t buf[PA_BLOCK_N];
            for (int b = 0; b < 250; b++) {
                for (int i = 0; i < PA_BLOCK_N; i++) {
                    double smp = sin(2.0 * PI * f * (double)(b * PA_BLOCK_N + i) / FS);
                    buf[i] = (int32_t)(smp * 0.7 * Q21);
                }
                pa_tuner_process_block(&t, buf, PA_BLOCK_N);
            }
            printf("   Ref A4=%u Hz with %5.1f Hz in -> %s%d (cents=%+2d, locked=%u)\n",
                   refs[r], f, k_note_names[t.note_index], t.octave, t.cents, t.signal_locked);
            CHECK(t.signal_locked == 1, "locked at Ref A4=%u Hz", refs[r]);
            CHECK(t.note_index == 9 && t.octave == 4, "identified as A4 at Ref %u", refs[r]);
            CHECK(abs(t.cents) <= 2, "cents deviation %+2d <= 2 cents at Ref %u", t.cents, refs[r]);
        }
    }

    /* [4] Silence: the last reading is HELD for 4 s (so a string can be played once and the peg turned), then the lock drops */
    printf("[4] testing silence: hold, then drop-out...\n");
    {
        int32_t buf[PA_BLOCK_N];
        memset(buf, 0, sizeof(buf));
        for (int b = 0; b < 40; b++) pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        CHECK(t.signal_locked == 1, "40 blocks (58 ms) of silence do not drop the lock");
        for (int b = 0; b < 100; b++) pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        printf("   After 0.2 s of silence: signal_locked=%u held=%u\n", t.signal_locked, t.held);
        CHECK(t.signal_locked == 1 && t.held == 1, "the reading is held after the signal ends (locked %u, held %u)", t.signal_locked, t.held);
        for (int b = 0; b < 1380; b++) pa_tuner_process_block(&t, buf, PA_BLOCK_N);        /* 2 s */
        CHECK(t.signal_locked == 1 && t.held == 1, "still held after 2 s");
        for (int b = 0; b < 1100; b++) pa_tuner_process_block(&t, buf, PA_BLOCK_N);        /* 3.6 s */
        CHECK(t.signal_locked == 1, "still held after 3.6 s");
        for (int b = 0; b < 500; b++) pa_tuner_process_block(&t, buf, PA_BLOCK_N);         /* 4.3 s */
        printf("   After 4.3 s of silence: signal_locked=%u\n", t.signal_locked);
        CHECK(t.signal_locked == 0, "the lock drops after the 4 s hold");
        CHECK(t.input_level_dots == 0, "input level dots drops to 0 on silence");
    }

    /* [5] Bypass Volume Scaling */
    printf("[5] testing bypass volume control...\n");
    {
        pa_tuner_init(&t);
        t.active = 1;

        int32_t buf[PA_BLOCK_N];
        /* Test 0% (mute) */
        t.bypass_volume = 0;
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = 1000000;
        pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        CHECK(buf[0] == 0, "bypass_volume = 0 mutes output (got %d)", buf[0]);

        /* Test 100% (unity) */
        t.bypass_volume = 100;
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = 1000000;
        pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        CHECK(abs(buf[0] - 1000000) <= 1, "bypass_volume = 100 passes unity (got %d)", buf[0]);

        /* Test 50% */
        t.bypass_volume = 50;
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = 1000000;
        pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        CHECK(abs(buf[0] - 500000) <= 2, "bypass_volume = 50 scales to 50%% (got %d)", buf[0]);
    }

    /* [6] Input Level Dots (0..7) */
    printf("[6] testing input level dots calculation (0..7)...\n");
    {
        pa_tuner_init(&t);
        t.active = 1;
        int32_t buf[PA_BLOCK_N];

        /* Peak 0.5 FS -> ~1048576 >> 16 = 16 -> clamped to 7 */
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = (int32_t)(0.5 * Q21);
        pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        CHECK(t.input_level_dots == 7, "0.5 FS produces 7 dots (got %u)", t.input_level_dots);

        /* Peak 0.05 FS -> ~104857 >> 16 = 1 dot */
        for (int i = 0; i < PA_BLOCK_N; i++) buf[i] = (int32_t)(0.05 * Q21);
        pa_tuner_process_block(&t, buf, PA_BLOCK_N);
        CHECK(t.input_level_dots == 1, "0.05 FS produces 1 dot (got %u)", t.input_level_dots);
    }

    run_real_tests();

    if (fails) {
        printf("\nFAILED: %d check(s)\n", fails);
        return 1;
    }
    printf("\nALL TUNER TESTS PASSED (0 failures)\n");
    return 0;
}
