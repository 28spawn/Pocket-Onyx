/* Interrupt services for the imported radio driver, on this firmware's interrupt model (main.c, audio_alnk.c):
 *   - the vector table is the word array at 0x01C7FE00, one handler address per IRQ; a handler is an assembly
 *     trampoline that saves the caller-saved registers, calls the C body and returns with rti (entry.S isr_alnk0);
 *   - the priority/enable bank for core 0 is at 0x01EEF100, one nibble per IRQ: bit 0 = enable, bits 3:1 = priority.
 *     The audio link (IRQ 11) is registered at priority 4; the radio goes in at 2, below it.
 * fm1_irq_attach() installs the vector and the priority with the enable bit CLEAR (the radio driver lets them go
 * with fm1_ble_irqs_hold(0) once the link layer is ready).
 * The trampolines for the two radio IRQs are defined in ble_radio.c (global assembly, only with PA_BLE=1). */
#pragma once
#include "fm1_cc.h"

#define FM1_ICFG(irq) (*(volatile uint32_t *)(0x01EEF100u + (((uint32_t)(irq) >> 3) * 4u)))
#define FM1_IRQ_VECTOR_TABLE 0x01C7FE00u

extern void pa_ble_tramp_event(void);
extern void pa_ble_tramp_rx(void);

extern uint32_t plat_ticks24(void);
extern volatile uint32_t g_irq_off_max_ticks;
extern volatile uint32_t g_irq_off_count;
extern volatile uint32_t s_irq_off_t0;

FM1_INLINE void fm1_irq_off(void) {
    s_irq_off_t0 = plat_ticks24();
    __asm__ volatile("cli" ::: "memory");
}
FM1_INLINE void fm1_irq_on(void)  {
    __asm__ volatile("sti" ::: "memory");
    uint32_t dt = plat_ticks24() - s_irq_off_t0;
    if (dt > g_irq_off_max_ticks) g_irq_off_max_ticks = dt;
    g_irq_off_count++;
}

/* Optimist's code asks "were the interrupts on?" with `fm1_icfg() & 0x200u` (bit 9 = the global enable, an emulator
 * model it flags as inferred) before it turns them off for the radio's RAM-resident register burst, and turns them
 * back on only if they were. We do not rely on that bit: the burst runs from the main context, where interrupts are
 * always on, so the answer is always "yes". (If it ever ran with interrupts off, this would turn them on.) */
FM1_INLINE uint32_t fm1_icfg(void) { return 0x200u; }

/* `isr` is the C body the driver names (isr_ble_event / isr_ble_rx); the matching trampoline is installed. */
static inline void fm1_irq_attach_tramp(uint32_t irq, void (*tramp)(void), uint32_t prio)
{
    volatile uint32_t *vec = (volatile uint32_t *)(FM1_IRQ_VECTOR_TABLE + irq * 4u);
    uint32_t shift = (irq & 7u) * 4u;
    *vec = (uint32_t)(uintptr_t)tramp;
    FM1_ICFG(irq) = (FM1_ICFG(irq) & ~(0xFu << shift)) | (((prio << 1) & 0xEu) << shift);   /* enable bit 0 left clear */
}
/* The driver calls fm1_irq_attach(irq, isr, prio): pick the trampoline by IRQ number (29 = RX, 45 = event). */
#define fm1_irq_attach(irq, isr, prio) \
    fm1_irq_attach_tramp((irq), (irq) == 45 ? pa_ble_tramp_event : pa_ble_tramp_rx, (prio))
