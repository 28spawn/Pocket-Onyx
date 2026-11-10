/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* boards/board_pocket_amp.h - the M-VAVE Pocket AMP (JieLi AC791N, 1 MB flash, ST7789 240x240). Included through pa_board.h only.
 * The port registers these bit masks apply to are in pa_hw.h. */
#ifndef PA_BOARD_POCKET_AMP_H
#define PA_BOARD_POCKET_AMP_H

#define PA_BOARD_ID            "pocket_amp"

/* What the unit reports as its name and model (the device-info record, sysex.c). */
#ifndef PA_DEVICE_NAME
#define PA_DEVICE_NAME         "Onyx"
#endif
#ifndef PA_DEVICE_MODEL
#define PA_DEVICE_MODEL        "Onyx"
#endif

/* ---- LCD (SPI1) ---- */
#define PA_LCD_CS_BIT          (1u << 1)    /* PB1 (gpio 17) */
#define PA_LCD_DC_BIT          (1u << 5)    /* PB5 (gpio 21) */
#define PA_LCD_SCK_BIT         (1u << 6)    /* PB6 (gpio 22, SPI1_CLK) */
#define PA_LCD_SDA_BIT         (1u << 7)    /* PB7 (gpio 23, SPI1_DO) */
#define PA_LCD_BL_PORTA_BIT    (1u << 9)    /* PA9 (gpio 9, active-low backlight) */

/* The panel's orientation and the 240 rows of the controller's 320 that are visible (st7735s.c init table). */
#define PA_BOARD_LCD_MADCTL    0x60u        /* MV|MX: landscape */
#define PA_BOARD_LCD_ROW_FIRST 40u          /* RASET 40..279 */
#define PA_BOARD_LCD_ROW_LAST  279u

/* ---- buttons and encoder (gpio_scan.c) ---- */
#define PA_ENC_PORTC_MASK      0x03u        /* PC0 (ENC_A) & PC1 (ENC_B) on JL_PORTC_IN */
#define PA_BTN0_PORTH_BIT      (1u << 6)    /* PH6 -> button bit 0 */
#define PA_BTN1_PORTH_BIT      (1u << 8)    /* PH8 -> button bit 1 */
#define PA_BTN2_PORTH_BIT      (1u << 9)    /* PH9 -> button bit 2 */
#define PA_BTN3_PORTA_BIT      (1u << 0)    /* PA0 -> button bit 3 */
#define PA_BTN4_PORTA_BIT      (1u << 1)    /* PA1 -> button bit 4 */
#define PA_BTN5_PORTA_BIT      (1u << 3)    /* PA3 -> button bit 5 */

/* ---- SARADC channels (gpio_scan.c) ---- */
#ifndef PA_POT_CH
#define PA_POT_CH              2            /* the master volume knob */
#endif
#define PA_VBAT_CH             9u           /* the battery voltage */

#endif /* PA_BOARD_POCKET_AMP_H */
