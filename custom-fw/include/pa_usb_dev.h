/* custom-fw/include/pa_usb_dev.h - the Pocket Amp's USB device driver (full speed, class-compliant MIDI).
 *
 * The AC791N's USB0 controller, polled (no USB interrupt): pa_usb_dev_poll() is called from the main
 * loop. A PC sees a "MIDI device" that Windows/macOS/Linux load their own driver for. It carries the
 * firmware's SysEx control protocol (pa_sysex, pa_diag) and the "enter upgrade" frame that resets the
 * unit into the mask-ROM download mode, so a unit can be driven, tested and recovered over the one
 * USB cable that also powers it.
 *
 * Registers and the attach sequence: the controller block at 0x11800, its SIE reached through the
 * CON1 indirect port, as proven on the same chip by the open FM-1 firmware (see
 * custom-fw/src/board/usb_dev.c for the facts used).
 *
 * VID/PID: 0x1209 / 0x0001 is the pid.codes TEST id: fine on the bench, replace before any release.
 */
#ifndef PA_USB_DEV_H
#define PA_USB_DEV_H

#include <stdint.h>
#include "pa_usb_ble.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
void     pa_usb_dev_init(pa_usb_ble_ctx_t *ctx);              /* ctx receives the MIDI bytes the host sends */
void     pa_usb_dev_poll(void);                               /* call often (the main loop does) */
int      pa_usb_dev_tx(const uint8_t *pkts, uint32_t len);    /* queue USB-MIDI event packets (len = 4 * n); -1 if full */
uint32_t pa_usb_dev_configured(void);
uint32_t pa_usb_dev_connected(void);                          /* 1 while a host is there: configured, not suspended, frames running (the status diamond) */                         /* 1 once a host has set the configuration */
void     pa_usb_dev_set_timer_prio(uint32_t prio);            /* BENCH: the timer interrupt's priority 0..7 (the default is 5, the audio interrupt's is 4) */
void     pa_usb_dev_set_service_mode(uint32_t mode);          /* BENCH (pack 159): 1 = a 2 kHz timer interrupt also serves the recording endpoint, 0 = the main loop only (default) */
#else
static inline void     pa_usb_dev_init(pa_usb_ble_ctx_t *ctx) { (void)ctx; }
static inline void     pa_usb_dev_poll(void) {}
static inline int      pa_usb_dev_tx(const uint8_t *pkts, uint32_t len) { (void)pkts; (void)len; return 0; }
static inline uint32_t pa_usb_dev_configured(void) { return 0u; }
static inline uint32_t pa_usb_dev_connected(void) { return 0u; }
static inline void     pa_usb_dev_set_service_mode(uint32_t mode) { (void)mode; }
static inline void     pa_usb_dev_set_timer_prio(uint32_t prio) { (void)prio; }
#endif

#endif /* PA_USB_DEV_H */
