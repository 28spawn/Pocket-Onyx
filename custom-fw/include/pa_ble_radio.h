/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/*
 * include/pa_ble_radio.h - the Bluetooth LE radio (the imported Optimist stack; src/board/ble_radio.c, BLE-PORT-PLAN.md).
 *
 * PA_BLE=0: both calls are empty. PA_BLE=1: the stack is linked, and the radio starts ONLY when asked:
 *   - at power-on with VOL+ and VOL- held together (and HOME not held), or
 *   - at every power-on when the build sets PA_BLE_AUTOSTART=1.
 * A normal power-on never touches a radio register. A start-up that hangs is cleared by a power cycle without the buttons.
 * Everything it does is logged: the status block at the SRAM address held in the pointer block 0x01C7D5D0 and the event log
 * (PA_EV_BLE). Read both with `tools/jl_ram_read.py ble`.
 */
#ifndef PA_BLE_RADIO_H
#define PA_BLE_RADIO_H

#include <stdint.h>

/* 0 by default: the Bluetooth / Wi-Fi radio hardware is never powered or touched unless the builder explicitly enables BLE (-Ble -> -DPA_BLE=1). */
#ifndef PA_BLE
#define PA_BLE 0
#endif
/* Follow PA_BLE: only run the radio supply sequence and auto-start when PA_BLE=1. */
#ifndef PA_BLE_SUPPLY
#define PA_BLE_SUPPLY PA_BLE   /* 1 = run the stock supply start-up (LDO to DC-DC hand-over etc.) before the radio and core 1 */
#endif
#ifndef PA_BLE_AUTOSTART
#define PA_BLE_AUTOSTART PA_BLE
#endif

/* the pointer block, fixed: [0] "BLEP", [1] address of the status block, [2] address of the stack's own counters (ble_dg, magic "BLEL"), [3] the
 * stack's breadcrumb (fm1_ble_bc), [4] size of ble_dg, [5] address of the PREVIOUS run's status block (its word PA_BLE_ST_WORDS is PREV when valid), [6] trims compiled in (1 yes), [7] boot buttons */
#define PA_BLE_PTR_ADDR 0x01C7D5D0u

/* status block words (volatile uint32_t[PA_BLE_ST_WORDS]) */
enum {
    PA_BLE_ST_MAGIC = 0,    /* "BLE1" */
    PA_BLE_ST_STATE,        /* PA_BLE_S_* */
    PA_BLE_ST_STEP,         /* the stack's breadcrumb: 0xB1 << 24 | step << 16 | ...  (FM1_BLE_STEP_*: 3 RF_INIT, 4 BB_INIT, 5 STARTED, 6 ENABLE, 11 IRQS_ON, 12 RUNNING) */
    PA_BLE_ST_ERR,          /* PA_BLE_E_* of the last error, 0 none */
    PA_BLE_ST_START_MS,     /* TIMER4 ms when the start-up began */
    PA_BLE_ST_START_US,     /* microseconds the start-up (radio + baseband) took */
    PA_BLE_ST_BUTTONS,      /* the boot buttons */
    PA_BLE_ST_IRQS,         /* BLE interrupts taken (fm1_ble_bc.irqs) */
    PA_BLE_ST_ADV_STARTS,   /* the stack's counters, copied by the poll: */
    PA_BLE_ST_ADV_EVENTS,   /*   advertising events: must keep growing while advertising */
    PA_BLE_ST_ADV_RX,       /*   packets heard while advertising (scan requests, connects) */
    PA_BLE_ST_SCAN_REQ,
    PA_BLE_ST_CIND_RX,      /*   CONNECT_IND heard / accepted / rejected */
    PA_BLE_ST_CIND_OK,
    PA_BLE_ST_CIND_REJ,
    PA_BLE_ST_ISR_MAX_US,   /*   the longest BLE interrupt */
    PA_BLE_ST_CONNECTED,    /* 1 while a central is connected */
    PA_BLE_ST_MIDI_READY,   /* 1 while the central has the MIDI notifications on */
    PA_BLE_ST_RF_OPS,       /* the radio start-up program: ops run / BBP timeouts / RF-die SPI timeouts / bad op / sections entered / VCO band found */
    PA_BLE_ST_RF_BBP_TO,
    PA_BLE_ST_RF_SPI_TO,
    PA_BLE_ST_RF_BADOP,
    PA_BLE_ST_RF_SECTIONS,
    PA_BLE_ST_RF_BAND,
    PA_BLE_ST_POLLS,        /* main-loop polls (shows the loop is alive) */
    PA_BLE_ST_MIDI_IN,      /* BLE-MIDI messages received from the central */
    PA_BLE_ST_RST_SRC,      /* the chip's reset-source register (P33 0x12) as this boot found it: WHY this boot began (compare a power-on with a watchdog restart) */
    PA_BLE_ST_WDT,          /* the watchdog register read back, once a second (0x5D = armed, about 8 s) */
    PA_BLE_ST_ALIVE_MS,     /* TIMER4 ms at the last poll: after a restart, the previous run's last sign of life */
    PA_BLE_ST_N_EVENT,      /* BLE event interrupts (IRQ 45) and RX interrupts (IRQ 29) entered, counted at the vector (before the stack's handler) */
    PA_BLE_ST_N_RX,
    PA_BLE_ST_STORM_IEN,    /* if the guard fired (more than 2,000 interrupts inside 100 ms): the engine's registers at that moment */
    PA_BLE_ST_STORM_IPND,
    PA_BLE_ST_STORM_G2PND,
    PA_BLE_ST_STORM_STAT,
    PA_BLE_ST_STORM_MS,     /* TIMER4 ms when it fired */
    PA_BLE_ST_WDT_AFTER_FEED, /* the watchdog register read 20 us after a feed, once a second: 0x1D = the feed bit (0x40) was taken, 0x5D = it was NOT */
    PA_BLE_ST_WDT_REARM,    /* 2 = the watchdog was DISARMED just before the radio start-up (this bench build), 0 = left as the boot armed it */
    PA_BLE_ST_FEED_US,      /* how long one watchdog feed (3 P33 bus transfers) took, microseconds, measured once a second: last */
    PA_BLE_ST_FEED_US_MAX,  /* ... and the longest */
    PA_BLE_ST_SUP_R0,       /* P33 reg 0 (rail select: bits 3:2 = 01 LDO13, 10 DC-DC13) before << 8 | after the stock supply sequence; 0xFFFF = sequence skipped */
    PA_BLE_ST_SUP_RES,      /* the sequence's read-back checks: 0 = all good, bit0 P33 timeout, bit1 rail not on the DC-DC, bit2 a level wrong */
    PA_BLE_ST_SUP_REGS,     /* P33 regs after the sequence: reg5 << 24 | reg7 << 16 | reg8 << 8 | reg2 */
    PA_BLE_ST_VCO_RES,      /* the VCO search: last comparator word (0x11978) read, bit 17 = low, bit 18 = high */
    PA_BLE_ST_VCO_DELAYS,   /* delays used in the VCO search: edge us << 16 | settle us; bit 31 set when taken from the clock-scaled stock rule */
    PA_BLE_ST_VCO_REF,      /* P0 0x40200 as read (bit 0 picks stock's 8 MHz or 4 MHz reference) */
    PA_BLE_ST_VCO_MODE,     /* 1 original 2/50 us (VOL- held), 2 stock-scaled x2 = 6/100 us (default), 3 = 2 plus the measured band latched (VOL+ held) */
    PA_BLE_ST_VCO_BAND_REG, /* 0x11938 as the start-up program left it */
    PA_BLE_ST_VCO_BAND_REG2,/* 0x11938 after the measured band was latched (mode 3), else 0 */
    PA_BLE_ST_REGS_PTR,     /* address of the register read-back snapshots (2 x 72 words), see ble_radio.c snap_regs */
    PA_BLE_ST_PLL_T0, /* the 47-point PLL table measured on this chip, 4 bytes per word, entries 0..47 (47 = the upper bound) */
    PA_BLE_ST_PLL_T1,
    PA_BLE_ST_PLL_T2,
    PA_BLE_ST_PLL_T3,
    PA_BLE_ST_PLL_T4,
    PA_BLE_ST_PLL_T5,
    PA_BLE_ST_PLL_T6,
    PA_BLE_ST_PLL_T7,
    PA_BLE_ST_PLL_T8,
    PA_BLE_ST_PLL_T9,
    PA_BLE_ST_PLL_T10,
    PA_BLE_ST_PLL_T11,
    PA_BLE_ST_PLL_STAT,     /* bit0 table measured, bit1 scan skipped (VOL- held), [15:8] LUT word-1 entries patched, [31:16] comparator probes */
    PA_BLE_ST_RATE_MAX,     /* the most advertising events seen in one second (configured: 1600 / interval units, 4 at 250 ms) */
    PA_BLE_ST_FAST_SECS,    /* seconds (not connected) above 5x the configured rate + 3 */
    PA_BLE_ST_FIRST_FAST_MS,/* TIMER4 ms of the first such second, 0 = none */
    PA_BLE_ST_RATE_RESETS,  /* times advertising was restarted by the rate watchdog */
    PA_BLE_ST_LINK_CLOSES, /* connections closed so far */
    PA_BLE_ST_LINK_SUP_TO, /* closed by the supervision timeout */
    PA_BLE_ST_LINK_ESTAB_FAIL, /* connections that failed to establish */
    PA_BLE_ST_LINK_PEER_TERM, /* the peer sent LL_TERMINATE_IND */
    PA_BLE_ST_LINK_CLOSE_WHY, /* the LAST close: reason (LL error code) | who (0 local, 1 peer, 2 supervision, 3 establish, 4 procedure timeout, 5 protocol, 6 terminate unacked) << 8 | event counter << 16 */
    PA_BLE_ST_LINK_CLOSE_MS, /* how long that connection lasted, ms (since its CONNECT_IND) */
    PA_BLE_ST_LINK_ENC_REQ, /* LL_ENC_REQ received */
    PA_BLE_ST_LINK_ENC_ON, /* links that became encrypted */
    PA_BLE_ST_LINK_PDU_N, /* ATT / L2CAP signalling / SMP / LL encryption PDUs seen (MIDI excepted) */
    PA_BLE_ST_LINK_NTF_N, /* MIDI notifications sent */
    PA_BLE_ST_LINK_WCMD_N, /* MIDI write commands received */
    PA_BLE_ST_LINK_CTL_RX, /* link-layer control PDUs received */
    PA_BLE_ST_HIST_N,       /* connections closed that this history saw (the ring below keeps the last 8, oldest overwritten) */
    PA_BLE_ST_HIST0, /* the close history: 8 entries x 6 words from here: [0] stack clock ms at the close, [1] reason | who << 8 | event counter << 16, [2] the connection lasted ms, [3] CONNECT_IND interval | latency << 16, [4] window offset | window size << 16 (1.25 ms units), [5] event IRQs | RX IRQs << 16 (running totals, low 16 bits) */
    PA_BLE_ST_HIST1,
    PA_BLE_ST_HIST2,
    PA_BLE_ST_HIST3,
    PA_BLE_ST_HIST4,
    PA_BLE_ST_HIST5,
    PA_BLE_ST_HIST6,
    PA_BLE_ST_HIST7,
    PA_BLE_ST_HIST8,
    PA_BLE_ST_HIST9,
    PA_BLE_ST_HIST10,
    PA_BLE_ST_HIST11,
    PA_BLE_ST_HIST12,
    PA_BLE_ST_HIST13,
    PA_BLE_ST_HIST14,
    PA_BLE_ST_HIST15,
    PA_BLE_ST_HIST16,
    PA_BLE_ST_HIST17,
    PA_BLE_ST_HIST18,
    PA_BLE_ST_HIST19,
    PA_BLE_ST_HIST20,
    PA_BLE_ST_HIST21,
    PA_BLE_ST_HIST22,
    PA_BLE_ST_HIST23,
    PA_BLE_ST_HIST24,
    PA_BLE_ST_HIST25,
    PA_BLE_ST_HIST26,
    PA_BLE_ST_HIST27,
    PA_BLE_ST_HIST28,
    PA_BLE_ST_HIST29,
    PA_BLE_ST_HIST30,
    PA_BLE_ST_HIST31,
    PA_BLE_ST_HIST32,
    PA_BLE_ST_HIST33,
    PA_BLE_ST_HIST34,
    PA_BLE_ST_HIST35,
    PA_BLE_ST_HIST36,
    PA_BLE_ST_HIST37,
    PA_BLE_ST_HIST38,
    PA_BLE_ST_HIST39,
    PA_BLE_ST_HIST40,
    PA_BLE_ST_HIST41,
    PA_BLE_ST_HIST42,
    PA_BLE_ST_HIST43,
    PA_BLE_ST_HIST44,
    PA_BLE_ST_HIST45,
    PA_BLE_ST_HIST46,
    PA_BLE_ST_HIST47,
    PA_BLE_ST_FL_PTR,       /* address of pa_flprobe[] (flash_ram.c): the flash driver probe's results, 0x52504C46 "FLPR" in word 0 when it ran */
    PA_BLE_ST_PM_DT_US,     /* the current (or last) connection: microseconds between its first and last connection event, measured on our 24 MHz timer */
    PA_BLE_ST_PM_DE,        /* ... and the number of connection events between them (the central's interval x this = the true time) */
    PA_BLE_ST_MIDI_OUT,     /* USB-MIDI event packets queued for the central (SysEx replies and so on) */
    PA_BLE_ST_MIDI_DROP,    /* event packets dropped because a queue was full (receive + send) */
    PA_BLE_ST_CLS0,         /* connection events on a channel the map uses: seen (all links since power-on) ... */
    PA_BLE_ST_CLS1,         /* ... and heard (a packet received in the event) */
    PA_BLE_ST_CLS2,         /* events on a channel the remap moved (sparse maps): seen ... */
    PA_BLE_ST_CLS3,         /* ... and heard */
    PA_BLE_ST_CLS4,         /* the same four with the event counter taken one event earlier (which alignment is right shows in the split) */
    PA_BLE_ST_CLS5,
    PA_BLE_ST_CLS6,
    PA_BLE_ST_CLS7,
    PA_BLE_ST_LATE_CHM,     /* channel-map PDUs that arrived after their instant and were applied late (the link used to close with 0x28) */
    PA_BLE_ST_LATE_UPD,     /* the same for connection-update PDUs */
    PA_BLE_ST_DIAG_PTR,     /* address of the link instrumentation block (pa_ble_diag.h) */
    PA_BLE_ST_MIDI_SUBS,    /* times a central turned the MIDI notifications on (0 to 1) */
    PA_BLE_ST_WORDS         /* = 145 (jl_ram_read.py BLE_ST lists the same names in the same order) */
};
enum {
    PA_BLE_S_IDLE = 0,      /* compiled in, not asked to start */
    PA_BLE_S_REQUESTED = 1,
    PA_BLE_S_NOCAL = 2,     /* asked, but this build has no radio calibration (tools/gen_ble_trims.py) */
    PA_BLE_S_STARTING = 3,  /* the radio start-up program is running: if the unit stays here it hung in it */
    PA_BLE_S_UP = 4,        /* radio and baseband up, BLE interrupts still masked */
    PA_BLE_S_ADVERTISING = 5,
    PA_BLE_S_CONNECTED = 6,
    PA_BLE_S_FAILED = 7
};
enum {
    PA_BLE_E_NONE = 0,
    PA_BLE_E_NO_TRIMS = 1,
    PA_BLE_E_RF_TIMEOUTS = 2,   /* the start-up program hit a bounded wait that expired (BBP or RF-die SPI) */
    PA_BLE_E_NO_ADV = 3,        /* advertising was enabled but no advertising event counted within 2 s */
    PA_BLE_E_ISR_SLOW = 4,      /* a BLE interrupt took over 300 us */
    PA_BLE_E_NO_ADDR = 5,       /* the hardware random source gave no address */
    PA_BLE_E_ISR_STORM = 6      /* the BLE interrupts fired 100,000 times a second: both masked by the guard (the main loop would have starved) */
};

#if PA_BLE
int pa_ble_radio_running(void);                      /* 1 once the radio is started (state UP or later) */
void pa_ble_radio_boot(uint32_t boot_buttons);   /* power-on, interrupts still off, before core 1 starts */
void pa_ble_radio_poll(void);                    /* main loop, every pass */
void pa_ble_radio_restore_bond(void);            /* the bond kept in the flash goes back into the stack's key slot (main.c, after the settings are loaded) */
void pa_ble_radio_midi_tx(const uint8_t *data, uint32_t len);   /* queue raw MIDI bytes (a reply) for the central, when it has the MIDI notifications on */
uint32_t pa_ble_radio_profile(int write, uint32_t value);            /* the experiment switches (pa_ble_diag.h): read, or set when write != 0 */
int pa_ble_radio_link(void);                         /* for the icon: 0 not running / switched off, 1 advertising, 2 connected */
void pa_ble_radio_set_enabled(int on);               /* the OFF / BLE setting at run time: on = advertise (ble_enable(1)), off = stop advertising and drop a link (ble_enable(0)) */
void pa_ble_radio_mirror_trims(void);                /* mirror the radio calibration from the stock log into the shared sector */
#else
static inline int pa_ble_radio_running(void) { return 0; }
static inline void pa_ble_radio_boot(uint32_t boot_buttons) { (void)boot_buttons; }
static inline void pa_ble_radio_poll(void) { }
static inline void pa_ble_radio_restore_bond(void) { }
static inline void pa_ble_radio_midi_tx(const uint8_t *data, uint32_t len) { (void)data; (void)len; }
static inline uint32_t pa_ble_radio_profile(int write, uint32_t value) { (void)write; return value; }
static inline int pa_ble_radio_link(void) { return -1; }          /* -1: no radio in this build, the UI shows the setting instead */
static inline void pa_ble_radio_set_enabled(int on) { (void)on; }
static inline void pa_ble_radio_mirror_trims(void) { }
#endif

#endif /* PA_BLE_RADIO_H */
