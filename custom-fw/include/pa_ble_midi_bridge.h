/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/include/pa_ble_midi_bridge.h - the two queues between the Bluetooth radio stack's interrupts and the amp's MIDI router (usb_ble.c).
 *
 * The radio stack decodes BLE-MIDI into USB-MIDI event packets (cin | status << 8 | data1 << 16 | data2 << 24, ble_midi.h) and calls ble_app_midi_in() in its interrupt;
 * it asks for packets to send by calling ble_app_midi_peek() / ble_app_midi_pop() in its interrupt. The amp's router (pa_usb_midi_rx_packet, the SysEx replies) lives in the
 * main loop. Each queue has one producer and one consumer, on different sides, so no lock is needed. Pure C: no hardware.
 *
 *   BLE interrupt  --rx_push-->  [RX queue]  --poll (main loop)-->  consumer(pkt4)  = pa_usb_midi_rx_packet
 *   main loop      --tx_bytes--> [TX queue]  --peek / pop (interrupt)-->  the stack frames it into a notification */
#ifndef PA_BLE_MIDI_BRIDGE_H
#define PA_BLE_MIDI_BRIDGE_H

#include <stdint.h>

void     pa_ble_bridge_set_rx_consumer(void (*consumer)(const uint8_t pkt4[4]));
void     pa_ble_bridge_rx_push(uint32_t pkt);                       /* interrupt side: one decoded event packet (dropped and counted when the queue is full) */
void     pa_ble_bridge_poll(void);                                  /* main loop: hand the queued packets to the consumer */
/* main loop: raw MIDI bytes (channel messages, SysEx F0..F7) -> USB-MIDI event packets in the TX queue. A message that does not fit whole is dropped whole (a
 * half SysEx would corrupt the reader). Returns the number of event packets queued. */
uint32_t pa_ble_bridge_tx_bytes(const uint8_t *data, uint32_t len, uint32_t t_ms);
int      pa_ble_bridge_tx_peek(uint32_t *pkt, uint32_t *t_ms);      /* interrupt side */
void     pa_ble_bridge_tx_pop(void);                                /* interrupt side */
uint32_t pa_ble_bridge_rx_dropped(void);
uint32_t pa_ble_bridge_tx_dropped(void);
uint32_t pa_ble_bridge_tx_queued(void);                             /* event packets queued in all */

#endif
