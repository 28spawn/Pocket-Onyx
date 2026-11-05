/* pa_input.h - the portable front-panel input layer.
 *
 * The firmware used to read the RAW button level every 60 Hz tick, which
 * meant:
 *   - a 100 ms tap moved the cursor SIX steps (six ticks of contact);
 *   - a contact shorter than one tick was missed entirely;
 *   - a bouncing contact could fire the same action twice;
 *   - the HOLD bits (PARA 1/2/3 s, HOME 1.5 s) were never produced, so
 *     Save-to, Moving mode, Live Stomp Lock and Factory Reset were
 *     unreachable on real hardware.
 *
 * This module turns a raw pressed/released mask into the mask the UI sees.
 * EVERY bit it produces is a ONE-TICK PULSE:
 *   - a tap is exactly one action;
 *   - a held button repeats after PA_IN_REPEAT_DELAY_MS, so scrolling is
 *     still fast when you want it;
 *   - a combo fires once when the combination becomes active;
 *   - a HOLD fires once, when the duration threshold is crossed.
 *
 * It is portable and SHARED: src/board/gpio_scan.c feeds it on the device
 * and wasm/pa_wasm.c feeds it in the browser, so both behave identically.
 * The board layer only has to say "this button is down".
 */
#ifndef PA_INPUT_H
#define PA_INPUT_H

#include <stdint.h>

/* ---- the mask the UI sees ---------------------------------------------- */
#define PA_BTN_HOME        (1u << 0)   /* HOME / BT switch                    */
#define PA_BTN_VOL_MINUS   (1u << 1)   /* VOL-                                */
#define PA_BTN_VOL_PLUS    (1u << 2)   /* VOL+                                */
#define PA_BTN_LEFT        (1u << 3)   /* <                                   */
#define PA_BTN_RIGHT       (1u << 4)   /* >                                   */
#define PA_BTN_PARA_CLICK  (1u << 5)   /* PARA click                          */
#define PA_BTN_COMBO_TUNER (1u << 6)   /* VOL- & VOL+  -> tuner                */
#define PA_BTN_COMBO_BANK  (1u << 7)   /* < & >      -> bank mode             */
#define PA_BTN_COMBO_DRUMS (1u << 8)   /* HOME & VOL- -> groove station       */
#define PA_BTN_COMBO_CHAIN (1u << 9)   /* HOME & VOL+ -> signal chain (unused) */
#define PA_BTN_PARA_HOLD1  (1u << 10)  /* PARA held 1-3 s, fires on release -> "Save to"          */
#define PA_BTN_PARA_HOLD2  (1u << 11)  /* PARA held past 3 s, fires the moment 3 s is reached (however long it stays down) -> pick the block up */
#define PA_BTN_PARA_HOLD3  (1u << 12)  /* unused (the factory reset is a click in Settings > System) */
#define PA_BTN_HOME_HOLD   (1u << 13)  /* HOME held 1.5 s, fires when 1.5 s is reached -> toggle PRESET / STOMP mode */
/* In STOMP mode on the home screen VOL-, VOL+, < and > act on RELEASE (a short press = the footswitch), and a long press (0.8 s, fires when reached) is one of these: */
#define PA_BTN_VOLM_HOLD   (1u << 14)  /* VOL- held   */
#define PA_BTN_VOLP_HOLD   (1u << 15)  /* VOL+ held   */
#define PA_BTN_LEFT_HOLD   (1u << 16)  /* <   held    */
#define PA_BTN_RIGHT_HOLD  (1u << 17)  /* >   held    */

/* the combos are now their own bits, not a mask of two buttons */
#define PA_COMBO_TUNER     PA_BTN_COMBO_TUNER
#define PA_COMBO_BANK      PA_BTN_COMBO_BANK
#define PA_COMBO_DRUMS     PA_BTN_COMBO_DRUMS
#define PA_COMBO_CHAIN     PA_BTN_COMBO_CHAIN

/* ---- timing ------------------------------------------------------------ */
#define PA_IN_DEBOUNCE_MS      15u   /* a contact must be stable this long   */
#define PA_IN_COMBO_WINDOW_MS  15u   /* chord window: hold single presses to catch chords */
#define PA_IN_REPEAT_DELAY_MS 350u  /* a held button starts repeating after  */
#define PA_IN_REPEAT_MS        70u  /* ... and then every this often         */
#define PA_IN_HOLD1_MS       1000u
#define PA_IN_HOLD2_MS       3000u   /* PARA: past this = the move hold (was 2 s, and a hold past 3 s did nothing) */
#define PA_IN_HOME_HOLD_MS   1500u
#define PA_IN_KEYHOLD_MS      800u   /* STOMP mode: VOL-, VOL+, <, > long press */

typedef struct {
    uint32_t raw;            /* the raw pressed mask, bits 0..5            */
    uint32_t cand;           /* the debounce candidate                     */
    uint32_t cand_ms;        /* when the candidate started                 */
    uint32_t stable;         /* the debounced level                        */
    uint32_t prev;           /* the previous debounced level (edge detect) */
    uint32_t press_ms[6];    /* when each button went down                 */
    uint32_t next_repeat_ms[6];
    uint8_t  hold_fired[6];  /* per button: its HOLD pulse has fired for this press */
    uint8_t  defer[6];       /* per button, set at the press: act on release (STOMP mode keys) */
    uint8_t  keys_hold_mode; /* the UI sets it each tick: STOMP mode on the home screen */
    uint8_t  combo_fired[4]; /* which combo pulses have already fired      */
    uint8_t  combo_used[6];  /* a button that was part of a chord          */
    uint8_t  press_fired[6]; /* initial press pulse emitted                */
    uint8_t  started;
} pa_input_state_t;

void     pa_input_init(pa_input_state_t *s);
/* The board layer calls this whenever a raw button state changes (or every
 * scan, which is fine). */
void     pa_input_set_raw(pa_input_state_t *s, uint32_t raw_mask);
/* The 60 Hz tick calls this and passes the result to the UI. */
uint32_t pa_input_update(pa_input_state_t *s, uint32_t now_ms);

#endif /* PA_INPUT_H */
