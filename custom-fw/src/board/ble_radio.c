/* Bluetooth LE radio: the unity build of the imported Optimist stack (third_party/optimist_ble, see its README and
 * NOTICE.md for the authors and licences) with this firmware's platform glue (ble_platform/), the start-up policy and
 * the debug log. BLE-PORT-PLAN.md has the plan; include/pa_ble_radio.h the contract.
 *
 * PA_BLE=0: this file is empty. PA_BLE=1: the stack is compiled in, and the radio is started ONLY when asked at power-on
 * (VOL+ and VOL- held, or PA_BLE_AUTOSTART=1). Without the unit's radio calibration (ble_platform/ble_trims_unit.h,
 * made by tools/gen_ble_trims.py from the verified backup) it refuses to start and says so in the log.
 *
 * What it logs: a status block (include/pa_ble_radio.h) reached through the pointer block at 0x01C7D5D0, the stack's own
 * counters (ble_dg, magic "BLEL") and breadcrumb, and PA_EV_BLE events in the event log. `tools/jl_ram_read.py ble` prints them.
 *
 * The stack's functions are all `static` here (BLE_API), as in Optimist's own build, so what is never called is not linked. */
#include "pa_config.h"
#include "pa_ble_radio.h"

#if PA_BLE && !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)

#include "pa_evlog.h"
#include "pa_ble_midi_bridge.h"
#include "pa_bond.h"

/* the two radio IRQ trampolines (same shape as isr_alnk0 in entry.S: save, call the C body, restore, csync, rti) */
__asm__(
    ".section .text.pa_ble_tramp, \"ax\"\n"
    ".global pa_ble_tramp_event\n"
    ".type pa_ble_tramp_event, @function\n"
    "pa_ble_tramp_event:\n"
    "    [--sp] = {psr, rets, reti}\n"
    "    [--sp] = {r3-r0}\n"
    "    call pa_ble_isr_event\n"
    "    {r3-r0} = [sp++]\n"
    "    {psr, rets, reti} = [sp++]\n"
    "    csync\n"
    "    rti\n"
    ".size pa_ble_tramp_event, . - pa_ble_tramp_event\n"
    ".global pa_ble_tramp_rx\n"
    ".type pa_ble_tramp_rx, @function\n"
    "pa_ble_tramp_rx:\n"
    "    [--sp] = {psr, rets, reti}\n"
    "    [--sp] = {r3-r0}\n"
    "    call pa_ble_isr_rx\n"
    "    {r3-r0} = [sp++]\n"
    "    {psr, rets, reti} = [sp++]\n"
    "    csync\n"
    "    rti\n"
    ".size pa_ble_tramp_rx, . - pa_ble_tramp_rx\n"
);

/* ---- configuration of the stack for this device (ble_cfg.h lists every knob) ---- */
#define BLE_DEVICE_NAME "Pocket AMP_BLE"    /* advertised BLE device name */
#define BLE_NAME_HEX 0                       /* the name as it is, no " XXXX" suffix */
static uint16_t pa_ble_adv_units = 400u;     /* the advertising interval in 0.625 ms units: 400 = 250 ms (stock and the stack's default are 100 ms; slower means fewer scan-request interrupts competing with the audio) */
#define BLE_ADV_INTERVAL pa_ble_adv_units
#define BLE_CENTRAL 0
#define FELUCCA_BLE_BOND 1                   /* pairing as the responder: Just Works + LE legacy + link encryption (Windows insists on pairing; without it the stack answers Pairing Not Supported and Windows drops the link after 400 ms) */
#define BLE_DIAG 1                           /* the stack's counters and ring (ble_dg): the debug log reads them */

#include "ble_platform/fm1_cc.h"
extern uint8_t plat_p33_read(uint16_t reg);
extern void plat_p33_write(uint16_t reg, uint8_t v);
extern void plat_delay_us(uint32_t us);
extern uint8_t plat_wdt_read(void);
extern void plat_wdt_feed(void);
/* The stack's own isr_ble_event / isr_ble_rx are COMPLETE interrupt entry points (they save the registers, call the C body, restore and rti: hal/fm1_ble.h). Packs 108-119 called
 * them from the trampoline below as if they were C functions: each BLE interrupt then ended with THEIR rti, back in the interrupted code with this trampoline's frame still on the
 * stack and r0-r3 not restored: a corrupted stack after the first BLE interrupt, hence every freeze. The trampoline now calls the C bodies they call. */
#define isr_event_body ble_wl82_event_irq
#define isr_rx_body ble_wl82_rx_irq

/* the driver publishes its TX/RX record to the baseband engine with RING_PUBLISH(); Optimist's own host tests define it as a
 * compiler barrier, and the next line of the driver does the real csync before the hand-over (ble_hw_wl82.c hw_tx_service) */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

/* the stack's functions are static and mostly unused until the application starts it: no warning for that */
/* This object's zero-initialised state (about 5 KB: the baseband block, the link layer, the PLL table) goes to its own region, carved
 * from the low end of the user stack (app.ld BLERAM, picked by object name): the 96 KB RAM region has no room for it. pa_ble_radio_boot() zeroes it. */
extern uint32_t _bleb_start[], _bleb_end[];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
/* The VCO search's two waits (hal/fm1_ble_rf.h, overridable by PA-MOD): wait 5 * (system clock / reference) loop passes between the pulse edges and 100 * that
   before the measurement, the reference being 8 MHz when bit 0 of 0x40200 is set, else 4 MHz. Set at boot from that rule (pa_ble_radio_boot). */
static volatile uint32_t pa_vco_edge_us = 2u, pa_vco_settle_us = 50u;
#define FM1_VCO_EDGE_US pa_vco_edge_us
#define FM1_VCO_SETTLE_US pa_vco_settle_us
/* RF PLL calibration: 47-point feedback-divider calibration table for VCO banks, which sets the VCO
   bank in word 1 of the RF-die LUT entries 0..127. Optimist writes the emulator's version of those bits. Prototypes and hook macros first; the code follows the include. */
static void pa_vco_stock_scan(void);
static uint32_t pa_lut_w1(uint32_t entry, uint32_t cmd, uint32_t w);
#define FM1_VCO_STOCK_SCAN pa_vco_stock_scan
#define FM1_LUT_W1(e, c, w) pa_lut_w1((e), (c), (w))
static uint8_t pa_tbl[48];
static uint8_t pa_tbl_ok, pa_scan_disabled;
static uint32_t pa_lut_patched, pa_probes;
#define HW_WIDEN_X 1u                       /* the connection receive window multiplier: 1 = the spec's window. Pack 143 set it to 4 and the iPhone then never completed a connection (0 protocol PDUs in 8 connections); back to 1 until it is tested on its own */
#define PA_BLE_GLUE 1                        /* the stack reads the live experiment profile (third_party/optimist_ble/src/pa_prof.h) */
#include "pa_ble_diag.h"
struct pa_ble_diag pa_ble_dgb;               /* the link instrumentation block (zeroed at boot with the rest of this object's .bss, set up in pa_ble_radio_boot) */
#include "../../third_party/optimist_ble/src/ble_stack.c"
#pragma clang diagnostic pop

/* ---- this unit's radio calibration: read from the unit's own record log at boot (pa_ble_trims.h); the copy compiled in from a backup is only the fallback ---- */
#include "../../include/pa_ble_trims.h"
static pa_ble_trims_t s_trims;
static uint8_t s_trims_src;                   /* 0 none, 1 read from this unit's flash, 2 compiled in */
#if defined(__has_include)
#if __has_include("ble_platform/ble_trims_unit.h")
#include "ble_platform/ble_trims_unit.h"
#endif
#endif
#ifndef PA_BLE_TRIMS_PRESENT
#define PA_BLE_TRIMS_PRESENT 0
#endif
static void pa_trims_init(void)
{
    if (s_trims_src) return;
    if (pa_ble_trims_load_from_flash(&s_trims)) { s_trims_src = 1u; return; }
#if PA_BLE_TRIMS_PRESENT
    {
        uint32_t i;
        for (i = 0; i < sizeof s_trims.x106; i++) s_trims.x106[i] = pa_ble_trim_x106[i];
        for (i = 0; i < sizeof s_trims.x107; i++) s_trims.x107[i] = pa_ble_trim_x107[i];
        for (i = 0; i < sizeof s_trims.x108; i++) s_trims.x108[i] = pa_ble_trim_x108[i];
        for (i = 0; i < sizeof s_trims.x187; i++) s_trims.x187[i] = pa_ble_trim_x187[i];
        s_trims_src = 2u;
    }
#endif
}

/* A fixed random static address for this unit (ble_hw_addr in the stack calls it): FNV-1a over the unit's own radio calibration records, so it is the same on every boot
 * (Windows and iOS keep their bonds against it) and differs between units with different calibration. Core Vol 6 Part B 1.3.2.1: the top two bits 11, the rest neither all
 * zeros nor all ones. Without the compiled-in calibration it returns 0 and the stack falls back to a random address. The unit's own Bluetooth MAC in the BTIF sector (flash
 * 0xBD000) would be better still, but it is outside the XIP window and the flash driver cannot read it yet. */
int pa_ble_static_addr(uint8_t addr[6])
{
    uint32_t h = 2166136261u, i, k;
    static const char salt[] = "PocketAMP-BLE-addr-v1";
    for (i = 0; i < sizeof salt - 1u; i++) { h ^= (uint8_t)salt[i]; h *= 16777619u; }
    pa_trims_init();
    if (s_trims_src) {
        for (i = 0; i < sizeof s_trims.x106; i++) { h ^= s_trims.x106[i]; h *= 16777619u; }
        for (i = 0; i < sizeof s_trims.x107; i++) { h ^= s_trims.x107[i]; h *= 16777619u; }
        for (i = 0; i < sizeof s_trims.x108; i++) { h ^= s_trims.x108[i]; h *= 16777619u; }
        for (i = 0; i < sizeof s_trims.x187; i++) { h ^= s_trims.x187[i]; h *= 16777619u; }
    }
#if 0
#warning "ble_platform/ble_trims_unit.h is missing: the radio has no calibration, and the Bluetooth address is the SAME on every unit built this way (derived from the salt only). Copy the unit's header in before building for a unit."
#endif
    for (k = 0; k < 6u; k++) { h ^= k + 0x5Au; h *= 16777619u; h ^= h >> 15; addr[k] = (uint8_t)h; }
    addr[5] |= 0xC0u;
    if ((addr[0] | addr[1] | addr[2] | addr[3] | addr[4]) == 0u || (addr[5] & 0x3Fu) == 0u) addr[0] = 1u;
    return 1;
}

/* ---- the debug log ---- */
static volatile uint32_t s_st[PA_BLE_ST_WORDS];
static uint32_t s_boot_decision;     /* PA_EV_BLE_DECISION's detail, kept until the event log exists (pa_evlog_init runs after the boot) */
static uint32_t s_boot_flags;        /* bit 0 began, bit 1 up, bit 2 enabled: events not yet written */
static uint32_t s_en_ms;             /* TIMER4 ms when advertising was enabled */
static uint32_t s_midi_in_n;
static uint8_t s_quiet;

/* what the status block said when the PREVIOUS run ended (a reset keeps the SRAM; a power cycle does not, and the magic word tells which). Lives in the NOINIT region, outside the
 * state that pa_ble_radio_boot() zeroes. */
static volatile uint32_t s_prev[PA_BLE_ST_WORDS + 1u] __attribute__((section(".noinit.ble_prev")));
#define PREV_MAGIC 0x56455250u               /* PREV: words 0..PA_BLE_ST_WORDS-1 are the old status block, the last word this mark */

/* the interrupt-storm guard: BLE event / RX interrupts that never clear would starve the main loop and the 8 s watchdog would reset the unit
 * with nothing logged. Normal rates are 10 to 300 a second; 2,000 interrupts inside 100 ms (20,000 a second) masks both, keeps the engine's registers
 * for the log and says so. The counters are taken at the vector, before the stack's handler runs. */
static volatile uint32_t s_n_ev, s_n_rx, s_win_n, s_win_t0, s_storm, s_storm_regs[5];
static void isr_guard(void)
{
    if (++s_win_n < 2000u)
        return;
    s_win_n = 0u;
    const uint32_t t = fm1_ticks();
    if ((uint32_t)(t - s_win_t0) < 100u * 1000u * FM1_TICKS_PER_US && !s_storm) {
        s_storm_regs[0] = FM1_BLE_IEN;
        s_storm_regs[1] = FM1_BLE_IPND;
        s_storm_regs[2] = FM1_BLE_G2PND;
        s_storm_regs[3] = FM1_BLE_STAT;
        s_storm_regs[4] = t / (1000u * FM1_TICKS_PER_US);
        fm1_ble_irqs_hold(1);
        s_storm = 1u;
    }
    s_win_t0 = t;
}
/* event period measured against our own 24 MHz timer: the first and the last connection event of the current link, each stamped at ISR entry, so the host can compare the average period with the interval the central asked for
   (it keeps an exact 1.25 ms-unit clock): the difference is our clock's error in ppm (pm_* below are read by pa_ble_radio_poll). */
static volatile uint32_t s_pm_t0, s_pm_t1, s_pm_conn, s_pm_ok;
static volatile uint16_t s_pm_e0, s_pm_e1;
void pa_ble_isr_event(void)
{
    const uint32_t t = fm1_ticks();
    s_n_ev++;
    isr_event_body();
    if (drv.state == HW_CONN && ble_dg.last_evt != 0xFFFFu) {
        const uint16_t e = ble_dg.last_evt;
        if (!s_pm_ok || s_pm_conn != ble_dg.cind_ok || (uint16_t)(e - s_pm_e1) > 0x8000u) { s_pm_t0 = t; s_pm_e0 = e; s_pm_conn = ble_dg.cind_ok; s_pm_ok = 1u; }
        s_pm_t1 = t; s_pm_e1 = e;
    }
    isr_guard();
}
void pa_ble_isr_rx(void) { s_n_rx++; isr_rx_body(); isr_guard(); }

static uint32_t now_ms(void) { return fm1_ticks() / (1000u * FM1_TICKS_PER_US); }
/* a reply from the amp's router (SysEx replies, anything it answers): queued for the central when it has switched the MIDI notifications on */
/* the diagnostic channel's profile command (7D 03): read (write = 0) or set the experiment switches live (pa_ble_diag.h) */
uint32_t pa_ble_radio_profile(int write, uint32_t v)
{
    if (write) pa_ble_dgb.profile = v & 0x3Fu;
    return pa_ble_dgb.profile;
}

void pa_ble_radio_midi_tx(const uint8_t *data, uint32_t len)
{
    if (ble_midi_ready()) s_st[PA_BLE_ST_MIDI_OUT] += pa_ble_bridge_tx_bytes(data, len, now_ms());
}


/* Register read-back (a diagnostic for "on the air but nobody hears it"): two snapshots of the radio's registers, one right after the start-up and one about 3 s later, so a block that is
   not powered, not clocked, or loses its settings shows up. Read with jl_ram_read.py ble (address in the status word REGS_PTR). 72 words each:
   0..18 BT block 0x2FC00..0x2FC48, 19..22 = 0x2FC7C 0x2FC80 0x2FC84 0x2FCBC, 23..36 = baseband control 0x28000..0x28034, 37..68 = RF analog 0x11900..0x1197C, 69..71 spare. */
#define PA_BLE_REG_WORDS 72u
static uint32_t s_regs[2][PA_BLE_REG_WORDS];
static void snap_regs(uint32_t k)
{
    uint32_t i, n = 0u;
    for (i = 0; i < 19u; i++) s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)(0x2FC00u + 4u * i);
    s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)0x2FC7Cu;
    s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)0x2FC80u;
    s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)0x2FC84u;
    s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)0x2FCBCu;
    for (i = 0; i < 14u; i++) s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)(0x28000u + 4u * i);
    for (i = 0; i < 32u; i++) s_regs[k][n++] = *(volatile uint32_t *)(uintptr_t)(0x11900u + 4u * i);
}

static void publish_pointers(uint32_t buttons)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)PA_BLE_PTR_ADDR;
    p[1] = (uint32_t)(uintptr_t)s_st;
    p[2] = (uint32_t)(uintptr_t)&ble_dg;
    p[3] = (uint32_t)(uintptr_t)&fm1_ble_bc;
    p[4] = (uint32_t)sizeof ble_dg;
    p[5] = (uint32_t)(uintptr_t)s_prev;           /* the previous run's status block (valid when its last word is PREV) */
    p[6] = (s_trims_src ? 1u : 0u) | (s_trims_src == 1u ? 2u : 0u) | ((uint32_t)PA_BLE_ST_WORDS << 8);     /* bit 0 trims compiled in; bits 15:8 the number of status words, so a reader older or newer than the firmware can tell */
    p[7] = buttons;
    p[0] = 0x50454C42u;                          /* "BLEP", last */
}

/* ---- the application side the stack asks for (third_party/optimist_ble/src/ble.h, "the firmware implements these") ----
 * Placeholders for BLE-PORT-PLAN.md steps 0-3 that count and log; step 4 wires them to this firmware's BLE-MIDI framing and
 * SysEx bridge (ble_midi.c, usb_ble.c). */


BLE_API int ble_app_midi_peek(uint32_t *pkt, uint32_t *t_ms) { return pa_ble_bridge_tx_peek(pkt, t_ms); }   /* what the amp has queued for the central */
BLE_API void ble_app_midi_pop(void) { pa_ble_bridge_tx_pop(); }
BLE_API void ble_app_midi_in(uint32_t pkt, uint16_t ts, uint16_t last_ts)
{
    (void)ts; (void)last_ts;
    s_midi_in_n++;
    s_st[PA_BLE_ST_MIDI_IN] = s_midi_in_n;
    if (s_midi_in_n == 1u)
        pa_evlog(PA_EV_BLE, PA_EV_BLE_MIDI_IN, pkt);
    pa_ble_bridge_rx_push(pkt);                  /* the main loop hands it to the amp's MIDI router (pa_ble_bridge_poll) */
}
BLE_API void ble_app_state(void) { }
/* Pairing (FELUCCA_BLE_BOND): the keys of a bonded central. They are kept in RAM for this power-on only (a power-off forgets the bond: the peer pairs again). */
static uint8_t s_bond_rand[8], s_bond_ltk[16], s_peer_irk[16], s_peer_addr[6], s_peer_rand;
static uint16_t s_bond_ediv;
static uint32_t s_bonds;
BLE_API void ble_app_bond(const uint8_t rand[8], uint16_t ediv, const uint8_t ltk[16])
{
    uint32_t i;
    for (i = 0; i < 8u; i++) s_bond_rand[i] = rand[i];
    for (i = 0; i < 16u; i++) s_bond_ltk[i] = ltk[i];
    s_bond_ediv = ediv;
    s_bonds++;
    pa_bond_stash(rand, ediv, ltk);              /* kept in RAM here (an interrupt: no flash); the main loop writes it (pa_bond_poll) and the next power-on restores it */
}
BLE_API void ble_app_peer_id(const uint8_t irk[16], const uint8_t addr[6], uint8_t addr_rand)
{
    uint32_t i;
    for (i = 0; i < 16u; i++) s_peer_irk[i] = irk[i];
    for (i = 0; i < 6u; i++) s_peer_addr[i] = addr[i];
    s_peer_rand = addr_rand;
    pa_bond_stash_id(irk, addr, addr_rand);
}
/* the bond kept in the flash goes back into the stack's key slot (main.c, once the settings are loaded and before a central is likely to connect), so a central that bonded before the
 * power-off can encrypt with its old key; and the way back: FORGET clears the live key. */
void pa_ble_radio_restore_bond(void)
{
    pa_bond_t b;
    if (pa_bond_load(&b)) {
        ble_host_set_key(b.rand, b.ediv, b.ltk);
        pa_evlog(PA_EV_BLE, PA_EV_BLE_BOND_RESTORED, b.ediv);
    }
}
void pa_ble_radio_forget_key(void) { ble_host_clear_key(); }

/* ---- start-up policy ---- */
static void copy_rf_stats(void)
{
    s_st[PA_BLE_ST_RF_OPS] = fm1_ble_rf_stat.ops;
    s_st[PA_BLE_ST_RF_BBP_TO] = fm1_ble_rf_stat.bbp_timeouts;
    s_st[PA_BLE_ST_RF_SPI_TO] = fm1_ble_rf_stat.spi_timeouts;
    s_st[PA_BLE_ST_RF_BADOP] = fm1_ble_rf_stat.bad_op;
    s_st[PA_BLE_ST_RF_SECTIONS] = fm1_ble_rf_stat.sections;
    s_st[PA_BLE_ST_VCO_RES] = fm1_ble_rf_stat.scan_result;
    s_st[PA_BLE_ST_RF_BAND] = (uint32_t)fm1_ble_rf_stat.scan_found << 8 | fm1_ble_rf_stat.scan_band;
}

/* ---- RF PLL hardware calibration ---- */
static void pa_set_cap(uint32_t c)  { FM1_WL_VCO_BAND = (FM1_WL_VCO_BAND & ~(0x7Fu << 19)) | (c & 0x7Fu) << 19; }
static void pa_set_fbds(uint32_t v) { FM1_WL_VCO_FINE = (FM1_WL_VCO_FINE & ~(0xFFu << 5)) | (v & 0xFFu) << 5; }
static void pa_rn_en(void)          /* latch pulses for VCO bank and divider */
{
    FM1_WL_VCO_BAND &= ~(1u << 16);
    FM1_WL_VCO_BAND &= ~(1u << 18);
    FM1_WL_VCO_EN &= ~(1u << 24);
    fm1_delay_us(pa_vco_edge_us);
    FM1_WL_VCO_BAND |= 1u << 16;
    fm1_delay_us(pa_vco_edge_us);
    FM1_WL_VCO_EN |= 1u << 24;
    FM1_WL_VCO_BAND |= 1u << 18;
    fm1_delay_us(pa_vco_settle_us);
}
static uint32_t pa_vc_res(void)     /* PLL comparator response */
{
    uint32_t i, r;
    FM1_WL_CAL_START = (FM1_WL_CAL_START & 0x0FFFFFFFu) | 0x10000000u;
    for (i = 0; i < 8u; i++) FM1_WL_CAL_STROBE = 1;
    FM1_WL_CAL_STROBE = 0;
    r = FM1_WL_CAL_STROBE;
    pa_probes++;
    return ((r >> 16) & 2u) | ((r >> 18) & 1u);
}
static uint32_t pa_bank(uint32_t v)  /* map frequency divider to VCO bank */
{
    uint32_t r3 = 1u, r1 = 0u;
    for (;;) {
        if (pa_tbl[r3] > v) break;
        r3++; r1++;
        if (r3 >= 48u) { r1 = 47u; break; }
    }
    return (uint32_t)(uint8_t)(pa_tbl[47] - r1);
}
static void pa_vco_stock_scan(void)  /* calibrate VCO banks */
{
    uint32_t r12, r7 = 0u, r6, r5, r4 = 0u, i, res0 = 0u, found = 0u;
    if (pa_scan_disabled) return;
    *(volatile uint32_t *)0x11940u |= 1u << 24;
    pa_set_fbds(73u);
    for (r12 = 7u; r12 < 120u; r12 += 16u) {                 /* coarse: where does the comparator change with the bank? */
        uint32_t a, b, c;
        pa_set_cap(r12);       pa_rn_en(); a = pa_vc_res();
        pa_set_cap(r12 - 7u);  pa_rn_en(); b = pa_vc_res();
        pa_set_cap(r12 + 8u);  pa_rn_en(); c = pa_vc_res();
        res0 = a;
        if (b != a || c != a) { r7 = r12; found = 1u; break; }
    }
    if (!found) r7 = res0;
    pa_tbl[47] = (uint8_t)(r7 | 0xFu);
    r5 = 73u; r6 = 73u;
    for (i = 0; i < 47u; i++) {                             /* fine: per bank, sweep the divider until the comparator flips to high */
        if (i) r5 = r4;
        pa_set_cap((uint32_t)(uint8_t)(pa_tbl[47] - i));
        r4 = r6;
        for (;;) {
            uint32_t res;
            pa_set_fbds(r4 & 0xFFu); pa_rn_en(); res = pa_vc_res();
            if (res == 1u) break;
            if (res == 2u) {
                r6 = r4; r4 = r4 + 1u;
                if ((r4 & 0xFFu) <= 171u) continue;
                r6 = 171u; r4 = 171u; break;
            }
            r4 = r4 + 1u;
            if ((r4 & 0xFFu) <= 171u) continue;
            r4 = 171u; break;
        }
        pa_tbl[i] = (uint8_t)(((r6 & 0xFFu) + (r5 & 0xFFu) - 3u) / 2u);
    }
    pa_set_fbds(121u);
    pa_set_cap(pa_bank(121u));
    *(volatile uint32_t *)0x11940u &= ~(1u << 24);
    pa_tbl_ok = 1u;
}
static uint32_t pa_lut_w1(uint32_t entry, uint32_t cmd, uint32_t w)   /* Analog control table initialization */
{
    if (!pa_tbl_ok || (cmd & 0xFu) != 0xDu || entry > 127u) return w;
    pa_lut_patched++;
    return (w & ~(0x7Fu << 9)) | (pa_bank((w >> 21) & 0xFFu) << 9);
}

/* Power supply start-up sequence matching hardware requirements (see Optimist's DUAL-CORE.md section 3,
 * "the rails first"). This unit's boot leaves P33 reg 0 on the LDO13 rail (bits 3:2 = 01) and regs 5/7/8/2 short of stock's values; Optimist found
 * that a busy second core plus the radio crashed within seconds until the rails were set this way. VDC14 and SYSVDD already read 7 and 15 here. */
static void sup_field(uint16_t reg, uint32_t pos, uint32_t len, uint32_t val)
{
    const uint8_t mask = (uint8_t)(((1u << len) - 1u) << pos);
    plat_p33_write(reg, (uint8_t)((plat_p33_read(reg) & (uint8_t)~mask) | (uint8_t)((val << pos) & mask)));
}
static void sup_or(uint16_t reg, uint8_t bits) { plat_p33_write(reg, (uint8_t)(plat_p33_read(reg) | bits)); }
static void sup_raise(uint16_t reg, uint8_t mask, uint8_t target)
{
    uint8_t v = (uint8_t)(plat_p33_read(reg) & mask);
    while (v < target) {
        v++;
        plat_p33_write(reg, (uint8_t)((plat_p33_read(reg) & (uint8_t)~mask) | v));
        plat_delay_us(100u);
        v = (uint8_t)(plat_p33_read(reg) & mask);
    }
}
static void supply_init(void)
{
    const uint8_t r0b = plat_p33_read(0u);
    sup_field(0u, 2u, 1u, 1u); plat_delay_us(100u);     /* LDO13 on */
    sup_field(0u, 3u, 1u, 1u); plat_delay_us(100u);     /* DCDC13 on */
    sup_field(0u, 2u, 1u, 0u); plat_delay_us(100u);     /* LDO13 off: the core runs from the DC-DC */
    sup_or(1u, 0x20u);
    sup_or(5u, 0x40u);
    sup_field(6u, 3u, 2u, 1u);
    sup_or(7u, 0xA5u);
    sup_or(8u, 0x96u);
    sup_or(9u, 0x10u);
    sup_field(5u, 0u, 3u, 4u);
    sup_raise(6u, 7u, 7u);
    sup_field(5u, 4u, 2u, 3u);
    sup_or(0u, 0x80u);
    sup_field(0x16u, 0u, 6u, 0u);
    sup_or(2u, 0x40u);
    plat_p33_write(0x17u, (uint8_t)(plat_p33_read(0x17u) & 0xDFu));
    sup_raise(9u, 15u, 15u);
    plat_delay_us(1000u);
    {
        const uint8_t r0 = plat_p33_read(0u), r5 = plat_p33_read(5u), r7 = plat_p33_read(7u), r8 = plat_p33_read(8u), r2 = plat_p33_read(2u);
        uint32_t res = 0u;
        if (((r0 >> 2) & 3u) != 2u) res |= 2u;
        if ((r5 & 7u) != 4u || (plat_p33_read(6u) & 7u) != 7u || (plat_p33_read(9u) & 15u) != 15u) res |= 4u;
        s_st[PA_BLE_ST_SUP_R0] = ((uint32_t)r0b << 8) | r0;
        s_st[PA_BLE_ST_SUP_RES] = res;
        s_st[PA_BLE_ST_SUP_REGS] = ((uint32_t)r5 << 24) | ((uint32_t)r7 << 16) | ((uint32_t)r8 << 8) | r2;
    }
}

/* power-on, interrupts off, before core 1 starts (main.c). Each state is written BEFORE the step it names, so a unit that stops
 * answering says where (STARTING with the breadcrumb: read the status block after a reset into the download mode). */
void pa_ble_radio_boot(uint32_t buttons)
{
    uint32_t i;
    const uint32_t rst_src = plat_p33_read(0x0012u);      /* why this boot began: read before anything else touches the chip */
    fm1_ble_crumb_boot();                        /* the stack's breadcrumb of the last run becomes prev, the live one starts from zero */
    if (s_st[PA_BLE_ST_MAGIC] == 0x31454C42u && s_st[PA_BLE_ST_STATE] <= PA_BLE_S_FAILED) {   /* a warm reset: the old block is still in the SRAM */
        for (i = 0; i < PA_BLE_ST_WORDS; i++)
            s_prev[i] = s_st[i];
        s_prev[PA_BLE_ST_WORDS] = PREV_MAGIC;
    } else {
        s_prev[PA_BLE_ST_WORDS] = 0u;
    }
    for (uint32_t *q = _bleb_start; q < _bleb_end; q++)    /* the stack's state: entry.S does not clear this region */
        *q = 0u;
    pa_ble_dgb.magic = PA_BLE_DIAG_MAGIC;
    pa_ble_dgb.size = (uint32_t)sizeof pa_ble_dgb;
    pa_ble_dgb.profile = PA_BLE_PROF_DEFAULT;
    for (i = 0; i < PA_BLE_ST_WORDS; i++)
        s_st[i] = 0u;
    s_st[PA_BLE_ST_BUTTONS] = buttons;
    s_st[PA_BLE_ST_RST_SRC] = rst_src;
    s_st[PA_BLE_ST_MAGIC] = 0x31454C42u;         /* "BLE1" */
    publish_pointers(buttons);
    s_st[PA_BLE_ST_DIAG_PTR] = (uint32_t)(uintptr_t)&pa_ble_dgb;
    { extern volatile uint32_t pa_flprobe[]; s_st[PA_BLE_ST_FL_PTR] = (uint32_t)(uintptr_t)pa_flprobe; }

#if PA_BLE_SUPPLY
    if ((buttons & 0x1Fu) == 0x03u)                /* HOME + VOL- held: keep the old supply state (A/B test) */
        s_st[PA_BLE_ST_SUP_R0] = 0xFFFFu;
    else
        supply_init();
#endif

    const int asked = PA_BLE_AUTOSTART || ((buttons & 0x07u) == 0x06u);   /* VOL+ and VOL- held, HOME not */
    const int quiet = (buttons & 0x07u) == 0x01u;   /* HOME alone: start the radio but never advertise (a test of what the start-up alone does) */
    if (!asked) {
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_IDLE;
        s_boot_decision = 0u;
        return;
    }
    s_st[PA_BLE_ST_STATE] = PA_BLE_S_REQUESTED;
    s_boot_decision = 1u;
    pa_trims_init();
    if (!s_trims_src) {
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_NOCAL;
        s_st[PA_BLE_ST_ERR] = PA_BLE_E_NO_TRIMS;
        s_boot_decision = 2u;
    } else
    {
        struct ble_rf_trims t;
        uint8_t a[6], rnd;
        uint32_t t0;
        for (i = 0; i < sizeof t.x106; i++) t.x106[i] = s_trims.x106[i];
        for (i = 0; i < sizeof t.x107; i++) t.x107[i] = s_trims.x107[i];
        for (i = 0; i < sizeof t.x108; i++) t.x108[i] = s_trims.x108[i];
        for (i = 0; i < sizeof t.x187; i++) t.x187[i] = s_trims.x187[i];

        s_boot_flags |= 1u;
        s_st[PA_BLE_ST_START_MS] = now_ms();
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_STARTING;
        t0 = fm1_ticks();
        /* The watchdog stays armed. (Pack 112 disarmed it as an experiment: it did not stop the resets, and when a download-mode reset did not take, nothing recovered the unit.) */
        ble_hw_wl82_attach();                    /* the two vectors, masked: no radio register written yet */
        fm1_ble_crumb_irqs = 0;
        fm1_ble_step(FM1_BLE_STEP_STACK_INIT);
        rnd = ble_hw_addr(a);                    /* a random static address from the hardware random source */
        ble_init(a, rnd);
        {   /* The name goes into the ADVERTISEMENT itself (flags 3 B + name 2+14 B = 19 B), the 128-bit MIDI service UUID into the scan response (18 B):
             * a passive scanner then lists "Pocket AMP_BLE" from the first packet, without the SCAN_REQ / SCAN_RSP exchange. The stack puts the name only in the scan response. */
            uint8_t ad[31], sr[2 + 16], k = (uint8_t)(bgap.n > 26u ? 26u : bgap.n);   /* (a legacy PDU holds 31 B) */
            ad[0] = 2u; ad[1] = 0x01u; ad[2] = 0x06u;
            ad[3] = (uint8_t)(k + 1u); ad[4] = 0x09u;
            for (i = 0; i < k; i++) ad[5 + i] = (uint8_t)bgap.s[i];
            sr[0] = 17u; sr[1] = 0x07u;
            for (i = 0; i < 16u; i++) sr[2 + i] = BLE_UUID_MIDI_SVC[i];
            {   /* Manufacturer-specific advertising field when space permits */
                static const uint8_t sinco[5] = { 0x73, 0x69, 0x6E, 0x63, 0x6F };
                uint8_t n = (uint8_t)(5u + k);
                if (n + 7u <= 31u) {
                    ad[n++] = 6u; ad[n++] = 0xFFu;
                    for (i = 0; i < 5u; i++) ad[n++] = sinco[i];
                }
                ble_ll_set_adv_data(ad, n, sr, (uint8_t)sizeof sr);
            }
        }
        {   /* Clock-scaled timing delays: loop passes per (clock / reference) */
            const uint32_t ref_reg = *(volatile uint32_t *)0x40200u, ratio = 360u / ((ref_reg & 1u) ? 8u : 4u);
            uint32_t mode = 2u, edge = (5u * ratio * 4u + 359u) / 360u * 2u, settle = (100u * ratio * 4u + 359u) / 360u * 2u;   /* default: twice stock's estimate (6 / 100 us): the one setting that made the search lock */
            /* (VOL- / VOL+ now step the crystal trim, below) */
            pa_vco_edge_us = edge; pa_vco_settle_us = settle;
            s_st[PA_BLE_ST_VCO_MODE] = mode;
            s_st[PA_BLE_ST_VCO_REF] = ref_reg;
            s_st[PA_BLE_ST_VCO_DELAYS] = (mode != 1u ? 0x80000000u : 0u) | (edge << 16) | settle;
        }
        pa_scan_disabled = ((buttons & 0x06u) == 0x02u) ? 1u : 0u;   /* VOL- held: Optimist's captured PLL constants only (A/B) */
        ble_hw_wl82_start(&t);                   /* the radio start-up program and the baseband window: the part that can hang */
        /* Configure timing parameter T34 = 0x0901 for baseband window stability. */

        FM1_BLE_T34 = 0x0901u;
        if (pa_tbl_ok) {                       /* the captured program wrote its own band after the scan: put stock's measured one back and latch it */
            pa_set_fbds(121u);
            pa_set_cap(pa_bank(121u));
            pa_rn_en();
        }
        {
            uint32_t w, k;
            for (w = 0; w < 12u; w++) {
                uint32_t v = 0u;
                for (k = 0; k < 4u; k++) v |= (uint32_t)pa_tbl[4u * w + k] << (8u * k);
                s_st[PA_BLE_ST_PLL_T0 + w] = v;
            }
            s_st[PA_BLE_ST_PLL_STAT] = (pa_tbl_ok ? 1u : 0u) | (pa_scan_disabled ? 2u : 0u) | (pa_lut_patched << 8) | (pa_probes << 16);
            s_st[PA_BLE_ST_VCO_BAND_REG2] = FM1_WL_VCO_BAND;      /* 0x11938 now: the cap field is the bank in use */
        }
        snap_regs(0u);
        s_st[PA_BLE_ST_REGS_PTR] = (uint32_t)(uintptr_t)s_regs;
        s_st[PA_BLE_ST_VCO_BAND_REG] = FM1_WL_VCO_BAND;                 /* 0x11938 as the start-up program left it */
        if (s_st[PA_BLE_ST_VCO_MODE] == 3u && fm1_ble_rf_stat.scan_found) {   /* latch the band the search measured, the way each search step does (pulses on 0x11938 bits 16 / 18 and 0x11934 bit 24) */
            const uint32_t b = fm1_ble_rf_stat.scan_band;
            FM1_WL_VCO_BAND = (FM1_WL_VCO_BAND & ~(0x7Fu << 19)) | b << 19;
            FM1_WL_VCO_BAND &= ~(1u << 16);
            FM1_WL_VCO_BAND &= ~(1u << 18);
            FM1_WL_VCO_EN &= ~(1u << 24);
            fm1_delay_us(pa_vco_edge_us);
            FM1_WL_VCO_BAND |= 1u << 16;
            fm1_delay_us(pa_vco_edge_us);
            FM1_WL_VCO_EN |= 1u << 24;
            FM1_WL_VCO_BAND |= 1u << 18;
            fm1_delay_us(pa_vco_settle_us);
            s_st[PA_BLE_ST_VCO_BAND_REG2] = FM1_WL_VCO_BAND;
        }

        s_st[PA_BLE_ST_FEED_US_MAX] = 0u;
        s_st[PA_BLE_ST_START_US] = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        copy_rf_stats();
        if (fm1_ble_rf_stat.bbp_timeouts || fm1_ble_rf_stat.spi_timeouts || fm1_ble_rf_stat.bad_op)
            s_st[PA_BLE_ST_ERR] = PA_BLE_E_RF_TIMEOUTS;
        fm1_ble_step(FM1_BLE_STEP_STARTED);
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_UP;
        s_boot_flags |= 2u;

        if (quiet) {                             /* QUIET (HOME held at power-on): the radio is up and idle, no advertising, the BLE interrupts stay masked */
            s_quiet = 1u;
            s_st[PA_BLE_ST_WDT_REARM] = 3u;
            s_st[PA_BLE_ST_STEP] = fm1_ble_bc.now;
            return;
        }
        fm1_ble_step(FM1_BLE_STEP_ENABLE);
        ble_enable(1);
        /* Default RF transmit level */                           /* advertising programmed; the engine is not running until the interrupts are let go */
        fm1_ble_step(FM1_BLE_STEP_IRQS_ON);
        fm1_ble_irqs_hold(0);                    /* the two BLE interrupts unmasked; they are taken once the CPU's interrupts come on ('<' alone now runs the flash probe, not the old masked-interrupt test) */
        fm1_ble_step(FM1_BLE_STEP_RUNNING);
        s_en_ms = now_ms();
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_ADVERTISING;
        s_boot_flags |= 4u;
        s_st[PA_BLE_ST_STEP] = fm1_ble_bc.now;
    }
}

int pa_ble_radio_running(void) { return s_st[PA_BLE_ST_STATE] >= PA_BLE_S_UP; }

/* The OFF / BLE setting of the Settings > Bluetooth row. ble_enable(0 / 1) is the call the advertising-rate watchdog below already makes on the unit; a link that is up is dropped by it
 * (ble_ll_enable: LL_CONN -> ble_ll_disconnect) and the stack does not advertise again while it is off. A radio that was started QUIET (HOME held at power-on: the BLE interrupts are still
 * masked) is left alone. */
static volatile uint8_t s_user_off;
void pa_ble_radio_set_enabled(int on) {
    if (s_st[PA_BLE_ST_STATE] < PA_BLE_S_UP || s_st[PA_BLE_ST_STATE] == PA_BLE_S_FAILED || s_quiet)
        return;
    if (on && s_user_off) {
        s_user_off = 0u;
        ble_enable(1);
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_ADVERTISING;
    } else if (!on && !s_user_off) {
        s_user_off = 1u;
        ble_enable(0);
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_UP;
    }
}
int pa_ble_radio_link(void) {
    const uint32_t st = s_st[PA_BLE_ST_STATE];
    if (s_user_off) return 0;
    return st == PA_BLE_S_CONNECTED ? 2 : (st == PA_BLE_S_ADVERTISING ? 1 : 0);
}

/* main loop, every pass (cheap; works every 50 ms) */
void pa_ble_radio_poll(void)
{
    static uint8_t first = 1u, adv_seen, noadv_logged, was_conn, was_midi;
    static uint32_t last_ms, isr_logged;
    uint32_t now;

    s_st[PA_BLE_ST_POLLS]++;
    pa_ble_bridge_poll();                        /* BLE-MIDI that arrived in the interrupts goes to the amp now */
    s_st[PA_BLE_ST_ALIVE_MS] = now_ms();
    { static uint8_t snapped2; if (!snapped2 && s_st[PA_BLE_ST_REGS_PTR] && now_ms() > 3000u) { snapped2 = 1u; snap_regs(1u); } }
    if (first) {                                 /* the event log was cleared after the boot: write what the boot did now */
        first = 0u;
        pa_evlog(PA_EV_BLE, PA_EV_BLE_DECISION, s_boot_decision);
        if (s_boot_flags & 1u) pa_evlog(PA_EV_BLE, PA_EV_BLE_BEGIN, s_st[PA_BLE_ST_START_MS]);
        if (s_boot_flags & 2u) pa_evlog(PA_EV_BLE, PA_EV_BLE_UP, s_st[PA_BLE_ST_START_US]);
        if (s_boot_flags & 4u) pa_evlog(PA_EV_BLE, PA_EV_BLE_ENABLED, s_st[PA_BLE_ST_START_MS]);
        if (s_st[PA_BLE_ST_ERR]) pa_evlog(PA_EV_BLE, PA_EV_BLE_ERROR, s_st[PA_BLE_ST_ERR]);
    }
    if (s_st[PA_BLE_ST_STATE] < PA_BLE_S_UP)
        return;                                  /* nothing is running */
    if (s_storm && s_st[PA_BLE_ST_STATE] != PA_BLE_S_FAILED) {
        s_st[PA_BLE_ST_STATE] = PA_BLE_S_FAILED;
        s_st[PA_BLE_ST_ERR] = PA_BLE_E_ISR_STORM;
        pa_evlog(PA_EV_BLE, PA_EV_BLE_ERROR, PA_BLE_E_ISR_STORM);
    }
    now = now_ms();
    if (now - last_ms < 50u)
        return;
    last_ms = now;

    s_st[PA_BLE_ST_STEP] = fm1_ble_bc.now;
    s_st[PA_BLE_ST_IRQS] = fm1_ble_bc.irqs;
    s_st[PA_BLE_ST_ADV_STARTS] = ble_dg.adv_starts;
    s_st[PA_BLE_ST_ADV_EVENTS] = ble_dg.adv_events;
    s_st[PA_BLE_ST_ADV_RX] = ble_dg.adv_rx;
    s_st[PA_BLE_ST_SCAN_REQ] = ble_dg.scan_req;
    s_st[PA_BLE_ST_CIND_RX] = ble_dg.cind_rx;
    s_st[PA_BLE_ST_CIND_OK] = ble_dg.cind_ok;
    s_st[PA_BLE_ST_CIND_REJ] = ble_dg.cind_rej;
    s_st[PA_BLE_ST_ISR_MAX_US] = ble_dg.isr_max_us;
    copy_rf_stats();
    s_st[PA_BLE_ST_N_EVENT] = s_n_ev;
    s_st[PA_BLE_ST_N_RX] = s_n_rx;
    s_st[PA_BLE_ST_LINK_CLOSES] = ble_dg.closes;
    s_st[PA_BLE_ST_LINK_SUP_TO] = ble_dg.sup_timeouts;
    s_st[PA_BLE_ST_LINK_ESTAB_FAIL] = ble_dg.estab_fails;
    s_st[PA_BLE_ST_LINK_PEER_TERM] = ble_dg.peer_terms;
    s_st[PA_BLE_ST_LINK_CLOSE_WHY] = (uint32_t)ble_dg.close_reason | (uint32_t)ble_dg.close_by << 8 | (uint32_t)ble_dg.close_evt << 16;
    s_st[PA_BLE_ST_LINK_CLOSE_MS] = ble_dg.close_since_start_us / 1000u;
    s_st[PA_BLE_ST_LINK_ENC_REQ] = ble_dg.enc_req_n;
    s_st[PA_BLE_ST_LINK_ENC_ON] = ble_dg.enc_on_n;
    s_st[PA_BLE_ST_LINK_PDU_N] = ble_dg.pdu_n;
    s_st[PA_BLE_ST_LINK_NTF_N] = ble_dg.att_ntf_n;
    s_st[PA_BLE_ST_LINK_WCMD_N] = ble_dg.att_wcmd_n;
    s_st[PA_BLE_ST_LINK_CTL_RX] = ble_dg.ctl_rx_n;
    s_st[PA_BLE_ST_MIDI_DROP] = pa_ble_bridge_rx_dropped() + pa_ble_bridge_tx_dropped();
    s_st[PA_BLE_ST_PM_DT_US] = (s_pm_t1 - s_pm_t0) / FM1_TICKS_PER_US;
    s_st[PA_BLE_ST_PM_DE] = (uint16_t)(s_pm_e1 - s_pm_e0);
    { uint32_t k; for (k = 0; k < 8u; k++) s_st[PA_BLE_ST_CLS0 + k] = pa_ble_cls[k]; }
    s_st[PA_BLE_ST_LATE_CHM] = pa_ble_late[0];
    s_st[PA_BLE_ST_LATE_UPD] = pa_ble_late[1];
    {   /* CLOSE HISTORY: one entry each time the stack's close counter moves (a close and a new connection inside one 50 ms poll would be counted once) */
        static uint32_t seen_closes;
        if (ble_dg.closes != seen_closes) {
            const uint32_t k = (s_st[PA_BLE_ST_HIST_N] & 7u) * 6u + PA_BLE_ST_HIST0;
            s_st[k + 0] = ble_dg.now_us / 1000u;
            s_st[k + 1] = (uint32_t)ble_dg.close_reason | (uint32_t)ble_dg.close_by << 8 | (uint32_t)ble_dg.close_evt << 16;
            s_st[k + 2] = ble_dg.close_since_start_us / 1000u;
            s_st[k + 3] = (uint32_t)ble_dg.cind_interval | (uint32_t)ble_dg.cind_latency << 16;
            s_st[k + 4] = (uint32_t)ble_dg.cind_win_off | (uint32_t)ble_dg.cind_win_size << 16;
            s_st[k + 5] = (ble_dg.evt_irqs & 0xFFFFu) | (ble_dg.rx_irqs & 0xFFFFu) << 16;
            s_st[PA_BLE_ST_HIST_N]++;
            seen_closes = ble_dg.closes;
        }
    }
    {   /* ADVERTISING RATE WATCHDOG: a unit seen advertising ~12x faster than configured after a connection (pack 132, Windows: 121 events/s instead of 9.6/s, cause not found). Once a second:
         * count the events; when not connected and the rate stays above 5x the configured one (+3) for two seconds, restart advertising (ble_enable 0 / 1), and count it. */
        static uint32_t rate_ms, rate_ev, fast_run;
        if (!rate_ms) { rate_ms = now; rate_ev = ble_dg.adv_events; }
        else if (now - rate_ms >= 1000u) {
            const uint32_t d = ble_dg.adv_events - rate_ev, expect = 1600u / pa_ble_adv_units + 1u;
            rate_ms = now; rate_ev = ble_dg.adv_events;
            if (d > s_st[PA_BLE_ST_RATE_MAX]) s_st[PA_BLE_ST_RATE_MAX] = d;
            if (!ble_connected() && d > 5u * expect + 3u) {
                s_st[PA_BLE_ST_FAST_SECS]++;
                if (!s_st[PA_BLE_ST_FIRST_FAST_MS]) s_st[PA_BLE_ST_FIRST_FAST_MS] = now;
                if (++fast_run >= 2u) { fast_run = 0u; ble_enable(0); ble_enable(1); s_st[PA_BLE_ST_RATE_RESETS]++; }
            } else fast_run = 0u;
        }
    }
    if (s_storm) {
        s_st[PA_BLE_ST_STORM_IEN] = s_storm_regs[0];
        s_st[PA_BLE_ST_STORM_IPND] = s_storm_regs[1];
        s_st[PA_BLE_ST_STORM_G2PND] = s_storm_regs[2];
        s_st[PA_BLE_ST_STORM_STAT] = s_storm_regs[3];
        s_st[PA_BLE_ST_STORM_MS] = s_storm_regs[4];
    }
    {   static uint32_t wdt_ms;
        if (now - wdt_ms >= 1000u) { wdt_ms = now; s_st[PA_BLE_ST_WDT] = plat_wdt_read(); const uint32_t f0 = fm1_ticks(); plat_wdt_feed(); const uint32_t fus = (fm1_ticks() - f0) / FM1_TICKS_PER_US; s_st[PA_BLE_ST_FEED_US] = fus; if (fus > s_st[PA_BLE_ST_FEED_US_MAX]) s_st[PA_BLE_ST_FEED_US_MAX] = fus; fm1_delay_us(20u); s_st[PA_BLE_ST_WDT_AFTER_FEED] = plat_wdt_read(); }
    }

    if (!adv_seen && ble_dg.adv_events) {
        adv_seen = 1u;
        pa_evlog(PA_EV_BLE, PA_EV_BLE_ADV_SEEN, now - s_en_ms);
    } else if (!s_quiet && !adv_seen && !noadv_logged && (now - s_en_ms) > 2000u) {
        noadv_logged = 1u;
        s_st[PA_BLE_ST_ERR] = PA_BLE_E_NO_ADV;
        pa_evlog(PA_EV_BLE, PA_EV_BLE_ERROR, PA_BLE_E_NO_ADV);
    }
    {
        const uint8_t conn = (uint8_t)(ble_connected() != 0), midi = (uint8_t)(ble_midi_ready() != 0);
        s_st[PA_BLE_ST_CONNECTED] = conn;
        s_st[PA_BLE_ST_MIDI_READY] = midi;
        { static uint8_t prev_midi; if (midi && !prev_midi) s_st[PA_BLE_ST_MIDI_SUBS]++; prev_midi = midi; }
        if (conn != was_conn) {
            was_conn = conn;
            s_st[PA_BLE_ST_STATE] = s_user_off ? PA_BLE_S_UP : (conn ? PA_BLE_S_CONNECTED : PA_BLE_S_ADVERTISING);
            pa_evlog(PA_EV_BLE, conn ? PA_EV_BLE_CONNECT : PA_EV_BLE_DISCONNECT, ble_dg.cind_ok);
        }
        if (midi && !was_midi)
            pa_evlog(PA_EV_BLE, PA_EV_BLE_MIDI_ON, 0u);
        was_midi = midi;
    }
    if (ble_dg.isr_max_us > isr_logged + 50u) {
        isr_logged = ble_dg.isr_max_us;
        pa_evlog(PA_EV_BLE, PA_EV_BLE_ISR_MAX, isr_logged);
        if (isr_logged > 300u) {
            s_st[PA_BLE_ST_ERR] = PA_BLE_E_ISR_SLOW;
            pa_evlog(PA_EV_BLE, PA_EV_BLE_ERROR, PA_BLE_E_ISR_SLOW);
        }
    }
}

#endif /* PA_BLE */
