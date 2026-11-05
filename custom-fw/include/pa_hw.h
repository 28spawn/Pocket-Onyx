/* pa_hw.h — hardware memory map & MMIO contract.
 * Hardware register definitions for JieLi AC791N SoC.
 * JieLi AC791N / pi32v2. XIP flash base 0x02000000, RAM from 0x01C00000. */
#ifndef PA_HW_H
#define PA_HW_H
#include <stdint.h>
#include "pa_config.h"

/* ---- MMIO access macros ---- */
#define PA_READ8(addr)         (*(volatile uint8_t  *)(uintptr_t)(addr))
#define PA_READ16(addr)        (*(volatile uint16_t *)(uintptr_t)(addr))
#define PA_READ32(addr)        (*(volatile uint32_t *)(uintptr_t)(addr))
#define PA_WRITE8(addr, val)   (*(volatile uint8_t  *)(uintptr_t)(addr) = (uint8_t)(val))
#define PA_WRITE16(addr, val)  (*(volatile uint16_t *)(uintptr_t)(addr) = (uint16_t)(val))
#define PA_WRITE32(addr, val)  (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(val))

/* ---- HW DSP / convolution accelerator ---- */
#define PA_ACCEL_REGS          0x01EE0400u  /* [+0]=status/cmd (bit7=ready,bit6=ack,write 0x149=START)
                                             * [+4]=command-block ptr, [+8]=aux */
#define PA_ACCEL_START         0x149u
#define PA_ACCEL_READY_BIT     0x80u
#define PA_ACCEL_ACK_BIT       0x40u
#define PA_ACCEL_CMD_BLOCK     0x01C1046Cu  /* [+0]cfg [+4]src_a [+8]src_b [+12]dst [+16]ctrl [+20]stride */
#define PA_ACCEL_BUSY_FLAG     0x01C0BFECu

/* Aliases used by hw_accel.c */
#define PA_HW_VEC_BASE         PA_ACCEL_REGS
#define PA_HW_VEC_STATUS_READY PA_ACCEL_READY_BIT
#define PA_HW_VEC_STATUS_ACK   PA_ACCEL_ACK_BIT

/* Control words select the op: */
#define PA_ACCEL_OP_FIR        0x15009010u  /* convolution (IR cab) */
#define PA_ACCEL_OP_MATMAC     0x1500A010u  /* matrix-vector MAC (GRU) */

/* ---- JL_ALNK0 Full-Duplex I2S Controller ---- */
#define JL_ALNK0_BASE          0x00012E00u
#define JL_ALNK0_CON0          (JL_ALNK0_BASE + 0x00u)
#define JL_ALNK0_CON1          (JL_ALNK0_BASE + 0x04u)
#define JL_ALNK0_CON2          (JL_ALNK0_BASE + 0x08u)
#define JL_ALNK0_CON3          (JL_ALNK0_BASE + 0x0Cu)
#define JL_ALNK0_ADR0          (JL_ALNK0_BASE + 0x10u)
#define JL_ALNK0_ADR1          (JL_ALNK0_BASE + 0x14u)
#define JL_ALNK0_ADR2          (JL_ALNK0_BASE + 0x18u)
#define JL_ALNK0_ADR3          (JL_ALNK0_BASE + 0x1Cu)
#define JL_ALNK0_LEN           (JL_ALNK0_BASE + 0x20u)
#define JL_ALNK0_SHN           (JL_ALNK0_BASE + 0x24u)

/* ---- JL_SPI1 Display Controller (IRQ 16) ---- */
#define JL_SPI1_BASE           0x00011D00u
#define JL_SPI1_CON            (JL_SPI1_BASE + 0x00u)
#define JL_SPI1_BAUD           (JL_SPI1_BASE + 0x04u)
#define JL_SPI1_BUF            (JL_SPI1_BASE + 0x08u)
#define JL_SPI1_ADR            (JL_SPI1_BASE + 0x0Cu)
#define JL_SPI1_CNT            (JL_SPI1_BASE + 0x10u)

/* ---- JL_TIMER2 Encoder/Button Scan Timer ---- */
#define JL_TIMER2_CON          0x00010600u

/* ---- GPIO Ports A, B, C, H ---- */
#define JL_PORTA_BASE          0x00050000u
#define JL_PORTA_OUT           (JL_PORTA_BASE + 0x00u)
#define JL_PORTA_IN            (JL_PORTA_BASE + 0x04u)
#define JL_PORTA_DIR           (JL_PORTA_BASE + 0x08u)
#define JL_PORTA_DIE           (JL_PORTA_BASE + 0x0Cu)
#define JL_PORTA_PU            (JL_PORTA_BASE + 0x10u)
#define JL_PORTA_PD            (JL_PORTA_BASE + 0x14u)

#define JL_PORTB_BASE          0x00050040u
#define JL_PORTB_OUT           (JL_PORTB_BASE + 0x00u)
#define JL_PORTB_IN            (JL_PORTB_BASE + 0x04u)
#define JL_PORTB_DIR           (JL_PORTB_BASE + 0x08u)
#define JL_PORTB_DIE           (JL_PORTB_BASE + 0x0Cu)
#define JL_PORTB_PU            (JL_PORTB_BASE + 0x10u)
#define JL_PORTB_PD            (JL_PORTB_BASE + 0x14u)

#define JL_PORTC_BASE          0x00050080u
#define JL_PORTC_OUT           (JL_PORTC_BASE + 0x00u)
#define JL_PORTC_IN            (JL_PORTC_BASE + 0x04u)
#define JL_PORTC_DIR           (JL_PORTC_BASE + 0x08u)
#define JL_PORTC_DIE           (JL_PORTC_BASE + 0x0Cu)
#define JL_PORTC_PU            (JL_PORTC_BASE + 0x10u)
#define JL_PORTC_PD            (JL_PORTC_BASE + 0x14u)

#define JL_PORTH_BASE          0x000501C0u
#define JL_PORTH_OUT           (JL_PORTH_BASE + 0x00u)
#define JL_PORTH_IN            (JL_PORTH_BASE + 0x04u)
#define JL_PORTH_DIR           (JL_PORTH_BASE + 0x08u)
#define JL_PORTH_DIE           (JL_PORTH_BASE + 0x0Cu)
#define JL_PORTH_PU            (JL_PORTH_BASE + 0x10u)
#define JL_PORTH_PD            (JL_PORTH_BASE + 0x14u)

/* Pocket Amp LCD Pin Masks */
#define PA_LCD_CS_BIT          (1u << 1)    /* PB1 (gpio 17) */
#define PA_LCD_DC_BIT          (1u << 5)    /* PB5 (gpio 21) */
#define PA_LCD_SCK_BIT         (1u << 6)    /* PB6 (gpio 22, SPI1_CLK) */
#define PA_LCD_SDA_BIT         (1u << 7)    /* PB7 (gpio 23, SPI1_DO) */
#define PA_LCD_BL_PORTA_BIT    (1u << 9)    /* PA9 (gpio 9, active-low backlight) */

/* Pocket Amp Button & Encoder Pin Masks */
#define PA_ENC_PORTC_MASK      0x03u        /* PC0 (ENC_A) & PC1 (ENC_B) on JL_PORTC_IN */
#define PA_BTN0_PORTH_BIT      (1u << 6)    /* PH6 -> button bit 0 */
#define PA_BTN1_PORTH_BIT      (1u << 8)    /* PH8 -> button bit 1 */
#define PA_BTN2_PORTH_BIT      (1u << 9)    /* PH9 -> button bit 2 */
#define PA_BTN3_PORTA_BIT      (1u << 0)    /* PA0 -> button bit 3 */
#define PA_BTN4_PORTA_BIT      (1u << 1)    /* PA1 -> button bit 4 */
#define PA_BTN5_PORTA_BIT      (1u << 3)    /* PA3 -> button bit 5 */

/* ---- audio engine control reg ---- */
#define PA_AUDIO_CTRL_REG      0x00020000u  /* bit8 gates the engine during model swap */
#define PA_AUDIO_CTRL_GATE     0x100u

/* ---- Audio stream node graph ---- */
#define PA_NODE_LIST_HEAD      0x01C0BA20u  /* [+60]=current node */
#define PA_STREAM_CTX          0x01C1B220u  /* [+556]=active stream */
#define PA_NODE_REGISTRY       0x01C1C1C8u  /* install_audio_node registry */
/* node struct offsets used by the scheduler/control: */
#define PA_NODE_NEXT           0            /* [node+0]  linked-list next (0 = end) */
#define PA_NODE_ACTIVE         4            /* [node+4]  active flag (slot bypass toggles this) */
#define PA_NODE_SIZE           12           /* [node+12] buffer/frame size */
#define PA_NODE_VTABLE         28           /* [node+28] op table; method[0]=ctrl, method[40]=run */
#define PA_NODE_RUN_METHOD     40

/* ---- NAM runtime state ---- */
#define PA_NAM_MODEL_HOLDER    0x01C10B70u  /* [+108]=active model record */
#define PA_NAM_BUSY_FLAG       0x01C10B7Au
/* GRU activation LUT pairs (slope/offset), piecewise-linear tanh/sigmoid: */
#define PA_NAM_ACT1_SLOPE      0x01C0DA40u
#define PA_NAM_ACT1_OFFSET     0x01C0D240u
#define PA_NAM_ACT2_SLOPE      0x01C0CA40u
#define PA_NAM_ACT2_OFFSET     0x01C0C240u

/* ---- Timer4: the 24 MHz cycle counter the firmware times itself with ----
 * The control/counter/period layout (CON, CNT, PRD) is the JieLi timer layout, and
 * 0x00010800 was confirmed as a TIMER (not a watchdog) because it has a period register
 * and because the firmware's own cycle counter and delay use it. */
#define JL_TIMER4_BASE          0x00010800u

/* ---- P33 Companion MCU & Watchdog Timer (WDT) ----
 * The AC791N PMU/watchdog registers are reached through a two-register sideband
 * mailbox (CON 0x00013E08, DAT 0x00013E0C). The watchdog register is P33 0x80 (P3_WDT_CON),
 * and the PMU soft reset register is P33 0xA0 (P3_PR_PWR, bit 4).
 *
 * Timer4 (JL_TIMER4_BASE = 0x00010800) is the 24 MHz cycle counter and MUST NOT be
 * confused with watchdog registers. */
#define JL_P33_CON             0x00013E08u
#define JL_P33_DAT             0x00013E0Cu

#define JL_P3_WDT_CON          0x0080u
#define JL_P3_VLD_KEEP         0x0017u   /* bit 6 = WDT_EXPT_EN: clear it for a plain watchdog reset */
#define JL_P3_VLD_KEEP         0x0017u
#define JL_P3_RST_SRC          0x0012u
#define JL_P3_PR_PWR           0x00A0u

/* Hardware control registers for bootloader / UBOOT handover */
#define JL_CPU0_WR_LIMIT_EN    0x01EEE348u
#define JL_CPU1_CTRL           0x01EEE004u
#define JL_UBOOT_MAILBOX       0x01C7FD80u

#define JL_WDT_BASE            0xFFFFFFFFu
#define JL_WDT_CON             (JL_WDT_BASE + 0x00u)
#define JL_WDT_CLEAR_KEY       0x5Au

/* The watchdog and the timer must never share a register block: plat_wdt_feed() writes
 * the clear key there, and writing it into TIMER4's control register reconfigures (and
 * with the clear key's low bits, stops) the timer the whole firmware times itself with.
 * This is a compile-time check, so the two can never collide again. */
_Static_assert(JL_WDT_BASE != JL_TIMER4_BASE,
               "the watchdog and Timer4 must not share a register block");

#endif /* PA_HW_H */

