/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/src/fx/fx_graph.c — Customizable-Order 6+1 Effect Graph + Stereo/Tail/Limiter */

#include "../../include/pa_hw.h"
#include "../../include/pa_evlog.h"
#include "pa_config.h"
#include "pa_fx.h"
#include "pa_cpu_meter.h"
#include "pa_dsp_cost.h"
#include "../dsp/fixedpoint.h"
#include "pa_biquad.h"
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#include "pa_plat.h"                    /* PA_ISR_CP: bench breadcrumbs for the audio interrupt */
#endif
#ifndef PA_ISR_CP
#define PA_ISR_CP(n) ((void)0)
#endif
#include <math.h>
#include "pa_wild.h"

extern void *pa_alloc(uint32_t n);
extern void  pa_free(void *p);

void fx_graph_run(fx_node_t *head, int32_t *buf, int n) {
    for (fx_node_t *nd = head; nd; nd = nd->next)
        if ((nd->active || nd->tail_draining) && nd->run)
            nd->run(nd, buf, n);
}

static void fx_link(fx_node_t **head, fx_node_t *node) {
    if (!node) return;
    if (!*head) {
        node->next = 0;
        *head = node;
        return;
    }
    /* Check if already in list to avoid cyclical loop */
    if (*head == node) {
        node->next = 0;
        return;
    }
    fx_node_t *p = *head;
    while (p->next) {
        if (p == node) return;
        p = p->next;
    }
    if (p == node) return;
    node->next = 0;
    p->next = node;
}

static void fx_unlink(fx_node_t **head, fx_node_t *node) {
    if (*head == node) { *head = node->next; node->next = 0; return; }
    for (fx_node_t *p = *head; p && p->next; p = p->next)
        if (p->next == node) { p->next = node->next; node->next = 0; return; }
}

/* Rebuild linked list according to the user's customizable `chain->slot_order[0..7]` (Manual Page 4 & 6) */
void pa_fx_chain_relink_ordered(pa_fx_chain_t *chain) {
    if (!chain) return;
    chain->head = 0;
    for (int i = 0; i < PA_SLOT_COUNT; i++) {
        uint8_t s = chain->slot_order[i];
        if (s < PA_SLOT_COUNT) {
            fx_node_t *nd = chain->slots[s];
            if (nd && (nd->active || nd->tail_draining)) {
                fx_link(&chain->head, nd);
            }
        }
    }
}

void pa_fx_chain_reset_states(pa_fx_chain_t *chain) {
    if (!chain) return;
    chain->lmt_env_q21 = PA_Q21_ONE;
    chain->spk_lp_L    = 0;
    chain->spk_lp_R    = 0;
    /* chain->in_dc_x1 / in_dc_y1 are NOT reset here: the input DC blocker follows the real input (the codec's offset), which a preset does not change. Reset to zero it passed that offset as a
     * step into the amp's gain at every swap (the thump measured on the unit, pack 186). It is cleared at init only. */
    chain->fizz_s1_L[0] = chain->fizz_s1_L[1] = 0;
    chain->fizz_s2_L[0] = chain->fizz_s2_L[1] = 0;
    chain->fizz_s1_R[0] = chain->fizz_s1_R[1] = 0;
    chain->fizz_s2_R[0] = chain->fizz_s2_R[1] = 0;
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        fx_node_t *nd = chain->slots[s];
        if (nd && nd->reset) {
            nd->reset(nd);
        }
    }
    pa_fx_chain_relink_ordered(chain);
}

void fx_set_enabled(fx_node_t **head, fx_node_t *node, int on) {
    if (!node) return;
#if PA_FEAT_FREE_ON_BYPASS
    if (on && !node->active) {
        if (!node->state && node->create) node->create(node);
        node->tail_draining = 0;
        node->active = 1;
        fx_link(head, node);
    } else if (!on && node->active) {
        node->active = 0;
        fx_unlink(head, node);
        if (node->destroy) node->destroy(node);
    }
#else
    node->active = on ? 1 : 0;
#endif
}

void pa_fx_chain_init(pa_fx_chain_t *chain, fx_node_t *amp_node, fx_node_t *cab_node) {
    if (!chain) return;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    for (uint32_t i = 0; i < 32u; i++) ((volatile uint32_t *)(uintptr_t)0x01C7D300u)[i] = 0u;   /* the level meters (see pa_lvl) */
#endif
    chain->head              = 0;
    chain->input_gain_q21    = PA_Q21_ONE;
    chain->preset_vol_q21    = PA_Q21_ONE;
    chain->speaker_mode      = PA_SPK_MODE_FRFR;
    chain->output_limit_mode = PA_LMT_MODE_BRICKWALL;
    chain->lmt_env_q21       = PA_Q21_ONE;
    chain->spk_lp_L          = 0;
    chain->spk_lp_R          = 0;
    chain->in_dc_x1          = 0;
    chain->in_dc_y1          = 0;
    chain->in_dc_primed      = 0;
    chain->fizz_filter_enabled = 1; /* LOW (6.5 kHz) */
    chain->meter             = 0;   /* optional per-stage meter; the host sets it */
    chain->global_in_gain_q21  = PA_Q21_ONE;
    chain->gate_thresh_q21     = 0;
    chain->gate_env_q21        = 0;
    chain->gate_gain_q21       = PA_Q21_ONE;
    chain->gate_hold           = 0;
    chain->force_cab_off       = 0;
    /* The parameter shadow starts EMPTY: a chain that was never given a
     * parameter must not replay one. (The struct is a stack local in the
     * host tests, so this cannot rely on the memory being zeroed.) */
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        chain->slot_param_mask[s] = 0;
        for (int id = 0; id < PA_FX_PARAM_IDS; id++) chain->slot_param_q21[s][id] = 0;
    }
    chain->preset_vol_cur_q21  = PA_Q21_ONE;
    chain->fizz_s1_L[0] = chain->fizz_s1_L[1] = 0;
    chain->fizz_s2_L[0] = chain->fizz_s2_L[1] = 0;
    chain->fizz_s1_R[0] = chain->fizz_s1_R[1] = 0;
    chain->fizz_s2_R[0] = chain->fizz_s2_R[1] = 0;

    chain->slots[PA_SLOT_DYNAMICS] = fx_dynamics_new();
    chain->slots[PA_SLOT_DRIVE]    = fx_drive_new();
    chain->slots[PA_SLOT_AMP]      = amp_node;
    chain->slots[PA_SLOT_CAB]      = cab_node;
    chain->slots[PA_SLOT_EQ]       = fx_eq_new();
    chain->slots[PA_SLOT_MOD]      = fx_mod_new();
    chain->slots[PA_SLOT_DELAY]    = fx_delay_new();
    chain->slots[PA_SLOT_REVERB]   = fx_reverb_new();

    /* Default order matching User Manual Page 4 diagram:
     * IN -> FX (Dynamics + Drive) -> AMP -> EQ -> MOD -> DLY -> REV -> CAB -> OUT */
    chain->slot_order[0] = PA_SLOT_DYNAMICS;      /* IMP-13: keep mono CAB/EQ before stereo FX */
    chain->slot_order[1] = PA_SLOT_DRIVE;
    chain->slot_order[2] = PA_SLOT_AMP;
    chain->slot_order[3] = PA_SLOT_CAB;
    chain->slot_order[4] = PA_SLOT_EQ;
    chain->slot_order[5] = PA_SLOT_MOD;
    chain->slot_order[6] = PA_SLOT_DELAY;
    chain->slot_order[7] = PA_SLOT_REVERB;

    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        if (chain->slots[s]) {
            chain->slots[s]->active        = 0;
            chain->slots[s]->tail_draining = 0;
            chain->slots[s]->next          = 0;
            if (chain->slots[s]->create) {
                chain->slots[s]->state     = 0;
            }
        }
    }
    /* IMP-13: only AMP + CAB at boot. Creating all 8 blocks needed ~87 KB of what
     * was then an 80 KB pool, so the last one (REVERB) got a state without its
     * tank and stayed silently dead. The pool is PA_DSP_POOL_BYTES (147,456 B) now
     * and the measured high-water with everything on is 136,336 B, but the boot
     * policy stays: the boot preset (IMP-10) enables exactly what it needs. */
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        fx_node_t *nd = chain->slots[s];
        if (!nd) continue;
        nd->tail_draining = 0; nd->next = 0; nd->fade_dir = 0; nd->pending_destroy = 0;
        if (s == PA_SLOT_AMP || s == PA_SLOT_CAB) {
#if PA_FEAT_FREE_ON_BYPASS
            if (!nd->state && nd->create) nd->create(nd);
#endif
            nd->active = 1; nd->wet_q21 = PA_Q21_ONE;
        } else {
            nd->active = 0; nd->wet_q21 = 0;
        }
    }
    pa_fx_chain_relink_ordered(chain);
}

/* Reorder modules live (Manual Page 6: "adjust the connection sequence of the 6 modules") */
void pa_fx_chain_swap_order(pa_fx_chain_t *chain, uint8_t pos_a, uint8_t pos_b) {
    if (!chain || pos_a >= PA_SLOT_COUNT || pos_b >= PA_SLOT_COUNT) return;
    uint8_t tmp = chain->slot_order[pos_a];
    chain->slot_order[pos_a] = chain->slot_order[pos_b];
    chain->slot_order[pos_b] = tmp;
    pa_fx_chain_relink_ordered(chain);
}

/* Immediate enable/disable. Only safe while the ISR is not running the chain
 * (pa_audio_is_muted()) - used by pa_preset_apply_to_chain(). */
void pa_fx_chain_set_slot_enabled_now(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, int on) {
    if (!chain || slot >= PA_SLOT_COUNT) return;
    fx_node_t *nd = chain->slots[slot];
    if (!nd) return;
    nd->fade_dir = 0; nd->pending_destroy = 0;
#if PA_FEAT_FREE_ON_BYPASS
    if (on && !nd->active) {
        if (!nd->state && nd->create) {
            nd->create(nd);
            if (!nd->state) return;
            pa_fx_chain_restore_slot_params(chain, slot);
        }
        nd->tail_draining = 0;
        nd->active = (nd->state || !nd->create) ? 1 : 0;   /* out of pool -> stays off, UI shows it */
    } else if (!on && (nd->active || nd->tail_draining)) {
        nd->active = 0;
        nd->tail_draining = 0;
        if (nd->destroy) nd->destroy(nd);
    }
#else
    nd->active = on ? 1 : 0;
#endif
    nd->wet_q21 = nd->active ? PA_Q21_ONE : 0;
    pa_fx_chain_relink_ordered(chain);
}

/* Live toggle from UI / stomp / MIDI (IMP-07, Blk-02): never frees memory under the ISR.
 * Enable: allocate in the UI thread, then let the ISR fade the wet signal in over one block.
 * Disable: ISR fades to dry, then flags pending_destroy; pa_fx_chain_collect_garbage() frees it.
 * DLY/REV with tail_enabled keep ringing (tail_draining) instead of fading. */
void pa_fx_chain_set_slot_enabled(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, int on) {
    if (!chain || slot >= PA_SLOT_COUNT) return;
    fx_node_t *nd = chain->slots[slot];
    if (!nd) return;
    if (on) {
        if (nd->active && nd->fade_dir >= 0) return;
        if (!nd->state && nd->create) {
            nd->create(nd);
            if (!nd->state) return;               /* pool exhausted: refuse, stay off */
            if (nd->reset) nd->reset(nd);
            /* The fresh state holds the FACTORY defaults. Put back what the
             * user had set - the knobs and the model - or the block comes
             * back with the wrong sound after a bypass. */
            pa_fx_chain_restore_slot_params(chain, slot);
        }
        nd->pending_destroy = 0;
        nd->tail_draining   = 0;
        if (!nd->active) nd->wet_q21 = 0;
        nd->fade_dir = +1;
        nd->active   = 1;
    } else {
        if (!nd->active) return;
        if (nd->tail_enabled && nd->state) {    /* V105 tail: stop feeding input, let it ring */
            nd->active = 0; nd->tail_draining = 1; nd->fade_dir = 0;
        } else {
            nd->fade_dir = -1;                  /* ISR completes the fade and clears 'active' */
        }
    }
    pa_fx_chain_relink_ordered(chain);
}

void pa_fx_chain_collect_garbage(pa_fx_chain_t *chain) {
#if PA_FEAT_FREE_ON_BYPASS
    if (!chain) return;
    for (int s = 0; s < PA_SLOT_COUNT; s++) {
        fx_node_t *nd = chain->slots[s];
        if (nd && nd->pending_destroy && !nd->active && !nd->tail_draining) {
            nd->pending_destroy = 0;
            if (nd->destroy) nd->destroy(nd);
        }
    }
#else
    (void)chain;
#endif
}

/* Push a value into a block AND remember it, so it can be replayed if the
 * block's state is freed and created again. */
static void fx_set_param_recorded(pa_fx_chain_t *chain, pa_fx_slot_id_t slot,
                                   int param_id, int32_t val) {
    if (!chain || slot >= PA_SLOT_COUNT) return;
    if (param_id >= 0 && param_id < PA_FX_PARAM_IDS) {
        chain->slot_param_q21[slot][param_id] = val;
        chain->slot_param_mask[slot] |= (uint16_t)(1u << param_id);
    }
    fx_node_t *nd = chain->slots[slot];
    if (nd && nd->set_param) {
        nd->set_param(nd, param_id, val);
    }
}

/* IMP-13: model/type selection. Each node historically used a different param id. */
void pa_fx_chain_set_slot_type(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, uint8_t type) {
    if (!chain || slot >= PA_SLOT_COUNT) return;
    fx_node_t *nd = chain->slots[slot];
    if (!nd || !nd->set_param) return;
    switch (slot) {
        case PA_SLOT_DRIVE:    fx_set_param_recorded(chain, slot, 3,  (int32_t)type); break;  /* DRV_PARAM_TYPE */
        case PA_SLOT_DYNAMICS:
        case PA_SLOT_EQ:
        case PA_SLOT_MOD:
        case PA_SLOT_DELAY:
        case PA_SLOT_REVERB:   fx_set_param_recorded(chain, slot, 10, (int32_t)type); break;  /* "explicit type" id */
        default: break;                                                     /* AMP/CAB: loaders */
    }
}

uint8_t pa_fx_slot_type_count(pa_fx_slot_id_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return 5;     /* + COMP (no gate) */
        case PA_SLOT_DRIVE:    return 6;
        case PA_SLOT_MOD:      return 7;
        case PA_SLOT_EQ:       return 3;     /* 5-BAND, LOW/HIGH CUT, PARAMETRIC-3 */
        case PA_SLOT_DELAY:    return 9;     /* + DIGITAL, PING PONG, REVERSE, SLAPBACK */
        case PA_SLOT_REVERB:   return 7;     /* + PLATE (AMBIENCE was removed in 1.13) */
        default:               return 1;
    }
}

void pa_fx_chain_set_param(pa_fx_chain_t *chain, pa_fx_slot_id_t slot, int param_id, int32_t val) {
    fx_set_param_recorded(chain, slot, param_id, val);
}

void pa_fx_chain_restore_slot_params(pa_fx_chain_t *chain, pa_fx_slot_id_t slot) {
    if (!chain || slot >= PA_SLOT_COUNT) return;
    fx_node_t *nd = chain->slots[slot];
    if (!nd || !nd->set_param) return;
    uint16_t mask = chain->slot_param_mask[slot];

    /* 1. Restore TYPE FIRST so the effect model is selected before knobs are applied */
    if (slot == PA_SLOT_DRIVE && (mask & (1u << 3))) {
        nd->set_param(nd, 3, chain->slot_param_q21[slot][3]);
    } else if (mask & (1u << 10)) {
        nd->set_param(nd, 10, chain->slot_param_q21[slot][10]);
    }

    /* 2. Restore all other knob parameters */
    for (int id = 0; id < PA_FX_PARAM_IDS; id++) {
        if (id == 10 || (slot == PA_SLOT_DRIVE && id == 3)) continue;
        if (mask & (1u << id)) {
            nd->set_param(nd, id, chain->slot_param_q21[slot][id]);
        }
    }
}

void pa_fx_chain_set_input_gain(pa_fx_chain_t *chain, int32_t gain_q21) {
    if (!chain) return;
    chain->input_gain_q21 = gain_q21;
}

void pa_fx_chain_set_preset_vol(pa_fx_chain_t *chain, uint8_t vol_0_to_100) {
    if (!chain) return;
    if (vol_0_to_100 > 100u) vol_0_to_100 = 100u;
    chain->preset_vol_q21 = (int32_t)(((uint64_t)vol_0_to_100 * PA_Q21_ONE) / 100u);
}

void pa_fx_chain_set_speaker_mode(pa_fx_chain_t *chain, pa_speaker_mode_t mode) {
    if (!chain) return;
    chain->speaker_mode  = mode;
    chain->force_cab_off = (mode == PA_SPK_MODE_AMP_RETURN) ? 1 : 0;   /* don't fight the preset's CAB state */
}

void pa_fx_chain_set_output_limit_mode(pa_fx_chain_t *chain, pa_lmt_mode_t mode) {
    if (!chain) return;
    if (chain->output_limit_mode != mode) {
        chain->output_limit_mode = mode;
        if (chain->lmt_env_q21 < (PA_Q21_ONE >> 2) || chain->lmt_env_q21 > PA_Q21_ONE) {
            chain->lmt_env_q21 = PA_Q21_ONE;
        }
    }
}

static inline void apply_output_limiter_stereo(pa_fx_chain_t *chain, int32_t *pL, int32_t *pR, int n) {
    /* A wild sample (more than 64 full scales out: the footprint of a saturated float-to-integer conversion, never of music) is dropped to silence before the limiter sees it: its gain
     * reduction would otherwise swallow the next stretch of real signal. A loud but real overshoot is NOT touched: the limiter handles it. */
    for (int i = 0; i < n; i++) {
        if (pa_q21_wild(pL[i])) pL[i] = 0;
        if (pR && pR != pL && pa_q21_wild(pR[i])) pR[i] = 0;
    }
    if (chain->output_limit_mode == PA_LMT_MODE_OFF) {
        *pL = sat32(*pL);
        if (pR) *pR = sat32(*pR);
        return;
    }
    const int32_t ceiling = 2055208;
    if (chain->output_limit_mode == PA_LMT_MODE_SOFT) {
        /* The soft knee. It used to look at *pL and *pR - the FIRST sample of
         * the buffer - and leave the other 63 samples completely untouched,
         * so the soft mode was not a limiter at all: it clipped one sample
         * per block and passed the rest through at full level. */
        const int32_t knee = 1782579;
        for (int i = 0; i < n; i++) {
            int32_t x = pL[i];
            if (x >  knee) x =  knee + ((x - knee) >> 2);
            if (x < -knee) x = -knee - ((-knee - x) >> 2);
            if (x >  ceiling) x =  ceiling;
            if (x < -ceiling) x = -ceiling;
            pL[i] = x;
            if (pR) {
                x = pR[i];
                if (x >  knee) x =  knee + ((x - knee) >> 2);
                if (x < -knee) x = -knee - ((-knee - x) >> 2);
                if (x >  ceiling) x =  ceiling;
                if (x < -ceiling) x = -ceiling;
                pR[i] = x;
            }
        }
        return;
    }
    /* Per-sample gain reduction.
     *
     * The old code took the peak of the WHOLE 64-sample block, computed ONE
     * gain, and applied it to every sample in the block, with an INSTANT
     * attack. The gain therefore stepped between adjacent blocks - a waveform
     * discontinuity, i.e. a click - and the ~30 ms release ramped back up
     * before the next transient knocked it down again, which is textbook
     * pumping. Because the limiter is the last stage of the chain, the CAB is
     * what drives it, so "cab + level" is exactly where the artefact was heard.
     *
     * The target gain is now computed once per 16-sample SUB-BLOCK, from the
     * sub-block peak, with a 32-bit division. pi32v2 has no fast divider and
     * a 64-bit divide is a compiler-rt call, so a per-sample 64-bit division
     * cost ~1 % of the 522,449-cycle block budget; this costs ~0.03 %.
     *
     * The per-sample envelope is kept: it shapes the ATTACK (fast, >>5) and
     * the RELEASE (slow, >>11), so the gain is continuous, a transient is
     * caught within a few samples, and nothing steps at a block boundary. */
    for (int base = 0; base < n; base += 16) {
        int end = base + 16;
        if (end > n) end = n;

        int32_t pk = 0;
        for (int i = base; i < end; i++) {
            int32_t a = (pL[i] < 0) ? -pL[i] : pL[i];
            if (pR) { int32_t b = (pR[i] < 0) ? -pR[i] : pR[i]; if (b > a) a = b; }
            if (a > pk) pk = a;
        }

        int32_t g_target = PA_Q21_ONE;
        if (pk > ceiling) {
            /* The gain we want is g_q21 = (ceiling_q21 << 21) / pk_q21, which
             * does not fit in 32 bits. So both are taken down to Q15 first:
             *
             *     g_q15 = (ceiling_q15 << 15) / pk_q15      (32-bit divide)
             *     g_q21 = g_q15 << 6
             *
             * The numerator is 32112 << 15 = 1.05e9, which fits in 32 bits,
             * and because the divide only happens when pk > ceiling the
             * quotient is at most 32768.
             *
             * THIS DIVIDED BY THE Q21 PEAK, not the Q15 peak - which made the
             * gain 64x (=-36 dB) too small. The limiter therefore did not
             * limit, it CRUSHED: every peak above the ceiling dropped the
             * whole mix by 36 dB and then the 30 ms release brought it back,
             * which is the "the audio cuts out on a real recording" report.
             * A quiet test tone never reaches the ceiling, so it never showed
             * the fault.
             *
             * The fix is the divisor: the peak has to be brought down to Q15
             * as well. */
            uint32_t num = ((uint32_t)(ceiling >> 6)) << 15;
            uint32_t pk15 = (uint32_t)pk >> 6;
            if (pk15 == 0u) pk15 = 1u;
            uint32_t g15 = num / pk15;
            g_target = (int32_t)(g15 << 6);
            if (g_target > PA_Q21_ONE) g_target = PA_Q21_ONE;
        }

        for (int i = base; i < end; i++) {
            if (g_target < chain->lmt_env_q21) chain->lmt_env_q21 += (g_target - chain->lmt_env_q21) >> 5;  /* ~1.4 ms attack */
            else                                  chain->lmt_env_q21 += (g_target - chain->lmt_env_q21) >> 11; /* ~30 ms release */
            pL[i] = q21_mul(pL[i], chain->lmt_env_q21);
            if (pR) pR[i] = q21_mul(pR[i], chain->lmt_env_q21);
        }
    }
}

void pa_fx_chain_set_fizz_filter(pa_fx_chain_t *chain, uint8_t enabled) {
    if (!chain) return;
    chain->fizz_filter_enabled = (enabled > 3) ? 3 : enabled;
}

/* 4th-Order Butterworth Low-Pass "Fizz Filter" (fc = 7.0 kHz @ 44.1 kHz):
 * 2 cascaded 2nd-order Direct Form II Transposed biquad sections.
 * Stage 1: b=[0.1493, 0.2986, 0.1493], a1=-0.6109, a2=0.1261
 * Stage 2: b=[0.1493, 0.2986, 0.1493], a1=-0.8211, a2=0.5135
 * Rolls off ultrasonic harmonics and alias foldover from high-gain profiles without dulling core tone. */
/* IMP-09 "Fizz" filter: 4th-order Butterworth LPF (2 biquads, exact per-stage coefficients).
 * Placed right after the AMP block (before the CAB), where the fizz/alias energy is created.
 * The old 7 kHz filter after the CAB was inaudible: stock cab IRs are already -14..-39 dB at
 * 8 kHz and the GRU engine has its own 7.2 kHz de-fizz. Levels: 1 = 6.5 kHz, 2 = 5 kHz, 3 = 4 kHz. */
static const int32_t k_fizz_coef[3][2][5] = {   /* {b0, b1, b2, a1, a2} in Q21 */
    { { 240689, 481378, 240689, -1449932,  315535 }, { 320416, 640833, 320416, -1930218, 1114732 } },
    { { 158992, 317984, 158992, -1979128,  517944 }, { 203981, 407963, 203981, -2539153, 1257927 } },
    { { 110595, 221190, 110595, -2356660,  701887 }, { 137362, 274724, 137362, -2927040, 1379337 } },
};

static void apply_fizz_filter_block(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n) {
    uint8_t lvl = chain->fizz_filter_enabled;
    if (lvl == 0) return;
    if (lvl > 3) lvl = 3;
    for (int stg = 0; stg < 2; stg++) {
        const int32_t *c = k_fizz_coef[lvl - 1][stg];
        pa_biquad_q21(buf_L, n, c, &chain->fizz_s1_L[stg], &chain->fizz_s2_L[stg]);
        if (buf_R) pa_biquad_q21(buf_R, n, c, &chain->fizz_s1_R[stg], &chain->fizz_s2_R[stg]);
    }
}

/* IMP-05 global input gain + noise gate (Settings > Input / Output) */
void pa_fx_chain_set_global_input(pa_fx_chain_t *chain, int8_t gain_db, int8_t gate_db) {
    if (!chain) return;
    static const int32_t k_db_q21_m12_p12[25] = {   /* 10^(dB/20) in Q21, -12..+12 */
        526793, 591322, 663749, 745059, 836329, 938783, 1053787, 1182877, 1327781, 1490442,
        1673024, 1877977, 2097152, 2352002, 2637767, 2958253, 3317676, 3720786, 4172863,
        4679796, 5248386, 5885994, 6601097, 7403131, 8302581 };
    if (gain_db < -12) gain_db = -12;
    if (gain_db >  12) gain_db =  12;
    chain->global_in_gain_q21 = k_db_q21_m12_p12[gain_db + 12];
    if (gate_db <= -80) { chain->gate_thresh_q21 = 0; return; }       /* -80 dB = OFF */
    if (gate_db > -30) gate_db = -30;
    chain->gate_thresh_q21 = (int32_t)(powf(10.0f, (float)gate_db / 20.0f) * (float)PA_Q21_ONE);
}

void pa_fx_chain_set_output_routing(pa_fx_chain_t *chain, uint8_t line_out, uint8_t cab_on_line) {
    if (!chain) return;
    chain->speaker_mode  = line_out ? PA_SPK_MODE_FRFR : PA_SPK_MODE_HEADPHONE_AIR;
    chain->force_cab_off = (line_out && !cab_on_line) ? 1 : 0;
}

static void run_global_gate(pa_fx_chain_t *chain, int32_t *buf, int n) {
    const int32_t th_open = chain->gate_thresh_q21, th_close = th_open >> 1;   /* 6 dB hysteresis */
    for (int i = 0; i < n; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        chain->gate_env_q21 += (a > chain->gate_env_q21) ? ((a - chain->gate_env_q21) >> 4)    /* ~0.4 ms */
                                                         : ((a - chain->gate_env_q21) >> 11);  /* ~46 ms  */
        int32_t target;
        if (chain->gate_env_q21 >= th_open)      { target = PA_Q21_ONE; chain->gate_hold = 2205; }  /* 50 ms hold */
        else if (chain->gate_hold)               { target = PA_Q21_ONE; chain->gate_hold--; }
        else if (chain->gate_env_q21 < th_close) { target = 0; }
        else                                     { target = chain->gate_gain_q21; }
        chain->gate_gain_q21 += (target > chain->gate_gain_q21) ? ((target - chain->gate_gain_q21) >> 5)
                                                                : ((target - chain->gate_gain_q21) >> 12);
        buf[i] = q21_mul(buf[i], chain->gate_gain_q21);
    }
}

/* Map an effect slot onto a CPU-meter stage. The CAB shares the MISC bucket
 * with everything that is not a named effect. */
static inline enum pa_stage fx_stage_of_slot(uint8_t slot) {
    switch (slot) {
        case PA_SLOT_DYNAMICS: return PA_ST_DYN;
        case PA_SLOT_DRIVE:    return PA_ST_DRIVE;
        case PA_SLOT_AMP:      return PA_ST_NAM;
        case PA_SLOT_EQ:       return PA_ST_EQ;
        case PA_SLOT_MOD:      return PA_ST_MOD;
        case PA_SLOT_DELAY:    return PA_ST_DELAY;
        case PA_SLOT_REVERB:   return PA_ST_REVERB;
        default:               return PA_ST_MISC;
    }
}

/* Per-node wet/dry crossfade for click-free bypass (runs in ISR) */
static int32_t s_dry_L[PA_BLOCK_N], s_dry_R[PA_BLOCK_N];
static void node_xfade_post(fx_node_t *nd, int32_t *L, int32_t *R, int n, const int32_t *dry_L, const int32_t *dry_R) {
    const int32_t step = PA_Q21_ONE / PA_BLOCK_N;
    for (int i = 0; i < n; i++) {
        nd->wet_q21 += nd->fade_dir * step;
        if (nd->wet_q21 > PA_Q21_ONE) nd->wet_q21 = PA_Q21_ONE;
        if (nd->wet_q21 < 0)          nd->wet_q21 = 0;
        L[i] = dry_L[i] + q21_mul(L[i] - dry_L[i], nd->wet_q21);
        if (R) R[i] = dry_R[i] + q21_mul(R[i] - dry_R[i], nd->wet_q21);
    }
    if (nd->fade_dir > 0 && nd->wet_q21 >= PA_Q21_ONE) nd->fade_dir = 0;
    if (nd->fade_dir < 0 && nd->wet_q21 <= 0) { nd->fade_dir = 0; nd->active = 0; nd->pending_destroy = 1; }
}

/* BENCH: measured time per slot, in TIMER4 ticks (24 MHz; the block budget is 34,800 ticks = 1.451 ms).
 * SRAM 0x01C7C500, 4 words per slot (0 DYN 1 DRV 2 AMP 3 CAB 4 EQ 5 MOD 6 DLY 7 REV): max, sum, calls, last.
 * Read in download mode with tools/jl_ram_read.py slots. Device builds only. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
extern uint32_t plat_ticks24(void);
#define PA_SLOT_T0() uint32_t pa_slot_t0 = plat_ticks24()
#define PA_SLOT_T1(s) do { \
        volatile uint32_t *w_ = (volatile uint32_t *)(uintptr_t)(0x01C7C500u + 16u * (uint32_t)(s)); \
        uint32_t d_ = plat_ticks24() - pa_slot_t0; \
        if (d_ > w_[0]) w_[0] = d_; \
        w_[1] += d_ >> 4; w_[2]++; w_[3] = d_;         if (w_[1] >= 0xC0000000u) { w_[1] >>= 1; w_[2] >>= 1; } } while (0)   /* the sum is in 16-tick units: the plain sum wrapped after 3 minutes of the amp, and this one after about an hour of it         (7 hours for the 12 % slots: an overnight readout showed DRV 2 us, REV 1 us). Before it can wrap, the sum and the call count are halved: the average stays right for any length of run, the "calls" word is then a weight, not an exact count. */
#else
#define PA_SLOT_T0() (void)0
#define PA_SLOT_T1(s) (void)(s)
#endif

/* Hooks from the amp (pa_audio_core.c): core 0 asks the amp to leave its tone stack and trim to core 0 for the blocks it hands to core 1, and applies them
 * to the block it gets back. The weak defaults keep a build without pa_audio_core.c (the host suites) as it was. */
__attribute__((weak)) void pa_amp_defer_post(int on) { (void)on; }
__attribute__((weak)) void pa_amp_apply_post(int32_t *buf, int n) { (void)buf; (void)n; }
__attribute__((weak)) void pa_amp_pre_core0(const int32_t *buf, int n) { (void)buf; (void)n; }

static pa_fx_post_handoff_fn s_post_handoff;
void pa_fx_chain_set_post_handoff(pa_fx_post_handoff_fn fn) { s_post_handoff = fn; }

/* Run slot_order[from..to) on the buffers. `stereo_split` carries the stereo state between calls; `dry_L/R` is the caller's own
 * crossfade scratch (core 0 and core 1 each pass theirs). */
/* Per-slot output peaks (device only): SRAM 0x01C7D400, [0..7] largest |sample| since boot per slot, [8..15] in the last block.
 * Shows WHICH slot makes a runaway signal. Core 1 writes the slots it runs, core 0 the others. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
static void pa_slot_peak(unsigned slot, const int32_t *b, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) { const int32_t v = b[i]; const uint32_t a = (uint32_t)(v < 0 ? -v : v); if (a > m) m = a; }
    volatile uint32_t *D = (volatile uint32_t *)(uintptr_t)0x01C7D400u;
    D[8 + slot] = m;
    if (m > D[slot]) D[slot] = m;
}
#define PA_SLOT_PEAK(s, b, n) pa_slot_peak((s), (b), (n))
#elif defined(PA_EMU_DLL)
/* the emulator keeps the same peaks for the tests: g_emu_slot_pk[slot] is the largest |sample| a slot produced since emu_clear_slot_peaks() */
volatile uint32_t g_emu_slot_pk[8];
static void pa_slot_peak(unsigned slot, const int32_t *b, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) { const int32_t v = b[i]; const uint32_t a = (uint32_t)(v < 0 ? -v : v); if (a > m) m = a; }
    if (slot < 8u && m > g_emu_slot_pk[slot]) g_emu_slot_pk[slot] = m;
}
#define PA_SLOT_PEAK(s, b, n) pa_slot_peak((s), (b), (n))
#else
#define PA_SLOT_PEAK(s, b, n) ((void)0)
#endif

/* While the WaveNet block engine runs the amp, core 1 has no microsecond to spare: the debug peak scans of the amp slot run on one block in 8. */
__attribute__((weak)) volatile uint8_t g_pa_wn_lean;
static inline int dbg_scan_now(int s) {
    static uint8_t cnt;
    if (s != PA_SLOT_AMP || !g_pa_wn_lean) return 1;
    return ((++cnt) & 7u) == 0u;
}

/* Set by core 0 for the blocks it hands to core 1: the fizz filter after the amp (about 25 us of software 64-bit biquads) then runs on core 0, on the block that comes
 * back, so core 1's job is the amp alone. Core 1 reads it as its job starts; core 0 only changes it for the NEXT block. */
static volatile uint8_t s_fizz_core0;

volatile uint8_t g_pa_stereo_allow = 1;
volatile uint8_t g_pa_cab_stereo;

void pa_fx_chain_set_stereo_width(pa_fx_chain_t *chain, uint8_t pct) {
    if (chain && chain->slots[PA_SLOT_CAB]) fx_cab_ir_set_stereo_width(chain->slots[PA_SLOT_CAB], pct);
}

/* Does this node run its stereo version now? A node without the option always does (when it has one). */
static inline int node_stereo_now(const fx_node_t *nd, int stereo_split) {
    if (!nd->run_stereo) return 0;
    if (nd->stereo_opt == 0) return 1;
    if (!g_pa_stereo_allow) return 0;
    if (nd->stereo_opt == 1) return nd->stereo_on != 0;
    return stereo_split && g_pa_cab_stereo;
}

PA_HOT static void run_slot_range(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n, int from, int to,
                           int *stereo_split, int32_t *dry_L, int32_t *dry_R, int skip_fizz) {
    for (int idx = from; idx < to; idx++) {
        uint8_t s = chain->slot_order[idx];
        if (s >= PA_SLOT_COUNT) continue;
        fx_node_t *nd = chain->slots[s];
        if (!nd || (!nd->active && !nd->tail_draining)) continue;
        if (s == PA_SLOT_CAB && chain->force_cab_off) continue;          /* IMP-05 line out */

        const int fading = (nd->fade_dir != 0);
        if (fading) {                                                    /* keep dry copy */
            for (int i = 0; i < n; i++) { dry_L[i] = buf_L[i]; dry_R[i] = buf_R ? buf_R[i] : buf_L[i]; }
        }

        const int scan_now = dbg_scan_now(s);
        PA_ISR_CP(0x40u + s);                   /* about to run slot s (0 DYN 1 DRV 2 AMP 3 CAB 4 EQ 5 MOD 6 DLY 7 REV) */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        uint32_t amp_in_pk = 0u;
        if (s == PA_SLOT_AMP && scan_now) for (int i = 0; i < n; i++) { const int32_t v = buf_L[i]; const uint32_t av = (uint32_t)(v < 0 ? -v : v); if (av > amp_in_pk) amp_in_pk = av; }
#endif
        PA_SLOT_T0();
        if (buf_R && node_stereo_now(nd, *stereo_split)) {
            nd->run_stereo(nd, buf_L, buf_R, n);
            (*stereo_split) = 1;
        } else if (nd->run) {
            if ((*stereo_split) && buf_R) {
                /* Sum L+R to mono if a mono block (e.g. CAB at end of chain) follows stereo MOD/DLY/REV */
                for (int i = 0; i < n; i++) buf_L[i] = (buf_L[i] >> 1) + (buf_R[i] >> 1);
                if (fading) for (int i = 0; i < n; i++) dry_L[i] = buf_L[i];
            }
            nd->run(nd, buf_L, n);
            if (buf_R) {
                for (int i = 0; i < n; i++) buf_R[i] = buf_L[i];
            }
            (*stereo_split) = 0;
        }
        PA_SLOT_T1(s);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        if (s == PA_SLOT_AMP) {
            const uint32_t amp_us = (plat_ticks24() - pa_slot_t0) / 24u;
            if (amp_us > 1600u) {
                uint32_t out_pk = 0u;
                for (int i = 0; i < n; i++) { const int32_t v = buf_L[i]; const uint32_t av = (uint32_t)(v < 0 ? -v : v); if (av > out_pk) out_pk = av; }
                pa_evlog(PA_EV_AMP_SLOW, amp_us, (((amp_in_pk >> 15) * 100u) >> 6) << 16 | ((((out_pk >> 15) * 100u) >> 6) & 0x7FFFu) | (((PA_READ32(JL_SPI1_CON) & 0x8000u) == 0u) ? 0x8000u : 0u));   /* bit 15: the LCD's SPI DMA was running at that moment */   /* peaks in percent of full scale (2^21 = 2^15 * 64) */
            }
        }
#endif
        if (scan_now) PA_SLOT_PEAK(s, buf_L, n);
        PA_ISR_CP(0x50u + s);                   /* slot s finished */
        /* Per-stage load: a calibrated model (include/pa_dsp_cost.h) so the
         * device, the host emulator and the WASM build agree. */
        if (chain->meter) cpu_meter_add(chain->meter, fx_stage_of_slot(s), pa_dsp_slot_cycles[s]);
        if (fading) node_xfade_post(nd, buf_L, buf_R, n, dry_L, dry_R);

        /* IMP-09: Fizz filter directly after the AMP block */
        if (s == PA_SLOT_AMP && chain->fizz_filter_enabled && !skip_fizz) {
            apply_fizz_filter_block(chain, buf_L, (*stereo_split) ? buf_R : 0, n);
            if (!(*stereo_split) && buf_R) {
                for (int i = 0; i < n; i++) buf_R[i] = buf_L[i];
            }
        }
    }
}

/* LEVEL METERS (device only): where does the signal vanish? Per stage, the largest |sample| since boot [0..7] and in the last block
 * [8..15], plus the chain's gain scalars [16..]. SRAM 0x01C7D300; read with tools/jl_ram_read.py slots. Two cores write different words.
 *   0 raw input  1 after input conditioning  2 after the front (amp) on core 1  3 as received by core 0  4 after the post slots  5 after the tail */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PA_LVL_BASE ((volatile uint32_t *)(uintptr_t)0x01C7D300u)
static void pa_lvl(unsigned idx, const int32_t *b, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) { const int32_t v = b[i]; const uint32_t a = (uint32_t)(v < 0 ? -v : v); if (a > m) m = a; }
    volatile uint32_t *D = PA_LVL_BASE;
    D[8 + idx] = m;
    if (m > D[idx]) D[idx] = m;
}
static void pa_lvl_scalars(const pa_fx_chain_t *c) {
    volatile uint32_t *D = PA_LVL_BASE;
    D[16] = (uint32_t)c->input_gain_q21;      D[17] = (uint32_t)c->global_in_gain_q21;
    D[18] = (uint32_t)c->preset_vol_q21;      D[19] = (uint32_t)c->preset_vol_cur_q21;
    D[20] = (uint32_t)c->gate_thresh_q21;     D[21] = (uint32_t)c->gate_gain_q21;
    uint32_t act = 0;
    for (int i = 0; i < PA_SLOT_COUNT; i++) if (c->slots[i] && c->slots[i]->active) act |= 1u << i;
    D[22] = act;                              D[23] = (uint32_t)c->force_cab_off;
    D[24] = (uint32_t)c->output_limit_mode;
    D[31] = 0x4C564C53u;                      /* "LVLS" */
}
/* [7]/[15]: the largest |L - R| since boot / in the last block: 0 means the output is dual mono. */
static void pa_lvl_width(const int32_t *l, const int32_t *r, int n) {
    uint32_t m = 0;
    for (int i = 0; i < n; i++) { const int32_t d = l[i] - r[i]; const uint32_t a = (uint32_t)(d < 0 ? -d : d); if (a > m) m = a; }
    volatile uint32_t *D = PA_LVL_BASE;
    D[15] = m;
    if (m > D[7]) D[7] = m;
}
#define PA_LVL(i, b, n) pa_lvl((i), (b), (n))
#else
#define PA_LVL(i, b, n) ((void)0)
#define pa_lvl_scalars(c) ((void)0)
#define pa_lvl_width(l, r, n) ((void)0)
#endif

/* The input conditioning (runs first, on whichever core runs the front of the chain). */
static void chain_condition(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n) {
    PA_LVL(0, buf_L, n);
    PA_ISR_CP(0x30);

    /* Pre-model DC-blocking high-pass filter (~15 Hz, R = 0.9978 in Q21 = 2092558)
     * Strips static ADC/converter DC offsets so high-gain models and tanh activations
     * remain centered at zero without generating idle saturation hiss */
    const int32_t dc_r = 2092558; /* 0.9978 in Q21 */
    for (int i = 0; i < n; i++) {
        int32_t x = buf_L[i];
        if (!chain->in_dc_primed) { chain->in_dc_x1 = x; chain->in_dc_y1 = 0; chain->in_dc_primed = 1; }     /* the first sample after init: no step from the input's DC offset */
        int32_t y = sat32((int64_t)x - chain->in_dc_x1 + q21_mul(dc_r, chain->in_dc_y1));
        chain->in_dc_x1 = x;
        chain->in_dc_y1 = y;
        y = sat32(q21_mul(y, chain->input_gain_q21));                     /* preset trim   */
        if (chain->global_in_gain_q21) y = sat32(q21_mul(y, chain->global_in_gain_q21)); /* global */
        buf_L[i] = y;
    }
    PA_ISR_CP(0x31);
    if (chain->gate_thresh_q21) run_global_gate(chain, buf_L, n);
    PA_ISR_CP(0x32);
    if (buf_R) {
        for (int i = 0; i < n; i++) buf_R[i] = buf_L[i];
    }
    PA_LVL(1, buf_L, n);
}

/* The tail after the slots: preset volume, speaker mode, output limiter (always on core 0, the one that owns the output). */
static void chain_tail(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n) {
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t tail_t0 = plat_ticks24();
#endif
    PA_ISR_CP(0x33);
    PA_ISR_CP(0x34);

    /* Zipper-free preset volume: ~3 ms one-pole glide toward the target */
    for (int i = 0; i < n; i++) {
        chain->preset_vol_cur_q21 += (chain->preset_vol_q21 - chain->preset_vol_cur_q21) >> 7;
        buf_L[i] = q21_mul(buf_L[i], chain->preset_vol_cur_q21);
        if (buf_R) buf_R[i] = q21_mul(buf_R[i], chain->preset_vol_cur_q21);
    }

    if (chain->speaker_mode == PA_SPK_MODE_HEADPHONE_AIR && buf_R) {
        for (int i = 0; i < n; i++) {
            int32_t L = buf_L[i], R = buf_R[i];
            chain->spk_lp_L += (L - chain->spk_lp_L) >> 3;
            chain->spk_lp_R += (R - chain->spk_lp_R) >> 3;
            buf_L[i] = sat32((int64_t)L + (chain->spk_lp_R >> 3));
            buf_R[i] = sat32((int64_t)R + (chain->spk_lp_L >> 3));
        }
    }

    /* The limiter needs the whole block, so it runs after the per-sample loops. */
    apply_output_limiter_stereo(chain, buf_L, buf_R, n);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    pa_timing(0, plat_ticks24() - tail_t0);
#endif
}

/* Core 1's share of the chain: slot_order[lo..to), and, when lo is 0, the input conditioning first. Core 1 runs this for the amp (see
 * pa_smp.h), or core 0 when core 1 is not available. */
/* Set by core 0 when it has conditioned the block it is about to hand over (the conditioning is 20-30 us a block that the WaveNet engine's core 1 can not spare).
 * Core 1 reads it as its job starts, and core 0 only changes it for the NEXT block, after that job has started. */
static volatile uint8_t s_pre_cond;

void pa_fx_chain_run_front(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n, int lo, int to, int32_t *dry_L, int32_t *dry_R) {
    int ss = 0;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t ft0 = plat_ticks24();
#endif
    if (lo <= 0 && !s_pre_cond) chain_condition(chain, buf_L, buf_R, n);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t ft1 = plat_ticks24();
#endif
    run_slot_range(chain, buf_L, buf_R, n, lo < 0 ? 0 : lo, to, &ss, dry_L, dry_R, s_fizz_core0);
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    {   /* where core 1's job goes (SRAM 0x01C7E7C0, TIMER4 ticks): [1] this function in all, last, [2] longest, [3] the slot loop, last, [4] the part before it, last */
        volatile uint32_t *F = (volatile uint32_t *)(uintptr_t)0x01C7E7C0u;
        const uint32_t ft2 = plat_ticks24();
        F[3] = ft2 - ft1; F[4] = ft1 - ft0; F[1] = ft2 - ft0; if (F[1] > F[2]) F[2] = F[1];
        F[0] = 0x544E5246u;                                           /* "FRNT" */
    }
#endif
    if (dbg_scan_now(PA_SLOT_AMP)) PA_LVL(2, buf_L, n);
}

/* Measured cost of each slot on the device, microseconds per 64-sample block (the AMP is the GRU model; see the slot timings readout). */
static const uint16_t k_slot_cost_us[PA_SLOT_COUNT] = { 45u, 330u, 1210u, 340u, 190u, 75u, 85u, 270u };   /* DYN DRV AMP CAB EQ MOD DLY REV */
static uint32_t s_core1_budget_us = 1300u;
void pa_fx_chain_set_core1_budget_us(uint32_t us) { s_core1_budget_us = us ? us : 1u; }

/* The first slot_order index core 1 runs: the amp at cut-1, plus as many of the active slots in front of it as fit its budget. The rest
 * (a drive pedal, the dynamics) runs on core 0 just before the hand-over, so neither core is asked for more than a block. */
static int choose_core1_lo(const pa_fx_chain_t *chain, int cut) {
    uint32_t cost = 30u + k_slot_cost_us[PA_SLOT_AMP];                    /* input conditioning + the amp */
    int lo = cut - 1;
    for (int idx = cut - 2; idx >= 0; idx--) {
        const uint8_t s = chain->slot_order[idx];
        if (s >= PA_SLOT_COUNT) { lo = idx; continue; }
        const fx_node_t *nd = chain->slots[s];
        const uint32_t c = (nd && (nd->active || nd->tail_draining) && !(s == PA_SLOT_CAB && chain->force_cab_off)) ? k_slot_cost_us[s] : 0u;
        if (cost + c > s_core1_budget_us) break;
        cost += c;
        lo = idx;
    }
    return lo;
}

/* Execute chain in customizable `slot_order[0..7]` with true stereo propagation.
 * With the second-core hook installed: the hook takes this block's input and runs the front of the chain (conditioning and every
 * slot up to and including the AMP) on core 1, and returns the PREVIOUS block's amp output in the buffers; this core then runs the
 * slots after the amp and the tail on it. Without the hook, everything runs here, in one call, exactly as before. */
void pa_fx_chain_run_stereo(pa_fx_chain_t *chain, int32_t *buf_L, int32_t *buf_R, int n) {
    if (!chain || !buf_L) return;
    g_pa_cab_stereo = 0;
    int stereo_split = 0;
    int cut = -1;                                                         /* slot_order index just after the amp, if we hand the front over */
    if (s_post_handoff) {
        for (int idx = 0; idx < PA_SLOT_COUNT; idx++) if (chain->slot_order[idx] == PA_SLOT_AMP) { cut = idx + 1; break; }
    }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    const uint32_t rs_t0 = plat_ticks24();
    uint32_t rs_t1 = rs_t0, rs_t2 = rs_t0;
#endif
    if (cut > 0) {
        const int lo = choose_core1_lo(chain, cut);
        if (lo > 0) {                                                     /* what does not fit core 1 runs here first */
            int ss0 = 0;
            chain_condition(chain, buf_L, buf_R, n);
            run_slot_range(chain, buf_L, buf_R, n, 0, lo, &ss0, s_dry_L, s_dry_R, 0);
            s_pre_cond = 0;
        } else {                                                          /* the conditioning is done here too, so core 1 starts on the amp */
            chain_condition(chain, buf_L, buf_R, n);
            s_pre_cond = 1;
        }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        rs_t1 = plat_ticks24();
#endif
        pa_amp_defer_post(1);
        s_fizz_core0 = 1u;
        pa_amp_pre_core0(buf_L, n);                                       /* the WaveNet's input conditioning, on this core */
        s_post_handoff(chain, buf_L, buf_R, n, cut, lo);                  /* the amp is a mono block, so the stereo state after it is 0 */
        pa_amp_apply_post(buf_L, n);                                      /* the tone stack and the trim of the block that came back */
        {   /* the fizz filter, left to this core (see s_fizz_core0): only where the slot loop would have applied it, i.e. when the amp slot runs */
            const fx_node_t *amp = chain->slots[PA_SLOT_AMP];
            if (chain->fizz_filter_enabled && amp && (amp->active || amp->tail_draining)) apply_fizz_filter_block(chain, buf_L, 0, n);
        }
        if (buf_R) for (int i = 0; i < n; i++) buf_R[i] = buf_L[i];       /* the amp is mono: the right channel follows the left after the post stages */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
        rs_t2 = plat_ticks24();
#endif
        PA_LVL(3, buf_L, n);
        run_slot_range(chain, buf_L, buf_R, n, cut, PA_SLOT_COUNT, &stereo_split, s_dry_L, s_dry_R, 0);
    } else {
        pa_amp_defer_post(0);
        s_fizz_core0 = 0u;
        chain_condition(chain, buf_L, buf_R, n);
        run_slot_range(chain, buf_L, buf_R, n, 0, PA_SLOT_COUNT, &stereo_split, s_dry_L, s_dry_R, 0);
    }
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    {   const uint32_t rs_t3 = plat_ticks24();
        pa_timing(2, rs_t1 - rs_t0);       /* what runs on this core before the hand-over (conditioning + the slots in front of the amp) */
        pa_timing(3, rs_t2 - rs_t1);       /* the hand-over: waiting for core 1's previous block, the swap, the submit */
        pa_timing(4, rs_t3 - rs_t2);       /* the slots after the amp */
    }
#endif
    {   /* runaway guard: a loop in the delay/reverb/cabinet that has saturated stays that way (the "stuck high pitch"). More than 48 times
         * full scale after the slots is never music: clear the effects after the amp and let this block be silent. */
        uint32_t pk = 0;
        for (int i = 0; i < n; i++) {
            int32_t v = buf_L[i]; uint32_t a = (uint32_t)(v < 0 ? -v : v); if (a > pk) pk = a;
            if (buf_R) { v = buf_R[i]; a = (uint32_t)(v < 0 ? -v : v); if (a > pk) pk = a; }
        }
        if (pk > (48u << 21)) {
            const int from = cut > 0 ? cut : 0;
            for (int idx = from; idx < PA_SLOT_COUNT; idx++) {
                fx_node_t *nd = chain->slots[chain->slot_order[idx]];
                if (nd && nd->reset) nd->reset(nd);
            }
            for (int i = 0; i < n; i++) { buf_L[i] = 0; if (buf_R) buf_R[i] = 0; }
            uint32_t which = 0xFFu;
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
            for (int idx = from; idx < PA_SLOT_COUNT && which == 0xFFu; idx++) {
                const uint32_t sl = chain->slot_order[idx];
                if (sl < PA_SLOT_COUNT && ((volatile uint32_t *)(uintptr_t)0x01C7D400u)[8 + sl] > (48u << 21)) which = sl;
            }
#endif
            pa_evlog(PA_EV_RUNAWAY, pk / ((1u << 21) / 100u), which);
        }
    }
    PA_LVL(4, buf_L, n);
    chain_tail(chain, buf_L, buf_R, n);
    PA_LVL(5, buf_L, n);
    if (buf_R) { PA_LVL(6, buf_R, n); pa_lvl_width(buf_L, buf_R, n); }
    pa_lvl_scalars(chain);
}

void pa_fx_chain_run(pa_fx_chain_t *chain, int32_t *buf, int n) {
    pa_fx_chain_run_stereo(chain, buf, 0, n);
}
