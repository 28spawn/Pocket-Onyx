/* custom-fw/include/pa_usb_desc.h - the USB descriptors of the Pocket Amp (USB-AUDIO-PLAN.md, step A).
 *
 * Shared by the firmware (src/board/usb_dev.c) and the host test (test/test_usb_desc.c), which walks the tables and checks every length, number and link, so a mistake
 * is found on the PC and not by a USB host that refuses the device.
 *
 * Two functions in one configuration, the way the stock unit is split (Windows shows MI_00 for the audio and MI_03 for the MIDI):
 *   interface association 1 (interfaces 0..2): USB Audio Class 1.0
 *       0  audio control: input terminal 1 (USB streaming, 2 ch) -> output terminal 2 (speaker); input terminal 3 (microphone, 2 ch) -> output terminal 4 (USB streaming)
 *       1  audio streaming, playback (host -> unit): alt 0 no endpoint, alt 1 isochronous OUT endpoint 3, adaptive
 *       2  audio streaming, recording (unit -> host): alt 0 no endpoint, alt 1 isochronous IN endpoint 3, asynchronous
 *   interface association 2 (interfaces 3..4): USB MIDI, as before (3 audio control, 4 MIDI streaming, bulk endpoint 1 in and out)
 * The audio format: PCM, 2 channels, 3 bytes (24 bit), one rate, 44,100 Hz.
 * The audio endpoints are declared here and refused by the driver for now (SET_INTERFACE alt 1 stalls): step A is enumeration only.
 * Written from the USB Audio Class 1.0 and USB MIDI 1.0 specifications; no vendor source is used.
 */
#ifndef PA_USB_DESC_H
#define PA_USB_DESC_H

#include <stdint.h>

#define PA_USB_AUDIO_EP          3u        /* the isochronous endpoint number, both directions (the stock uses endpoint 3 as well) */
#define PA_USB_AUDIO_MAXP        294u      /* 49 samples x 6 bytes: a 48 kHz frame carries 48 samples (288 bytes), a 44.1 kHz one 44 or 45 (264 / 270); one sample of margin (the stock, 44.1 kHz only: 270 and 274) */
#define PA_USB_DESC_VERSION      0x0101u   /* bcdDevice: 1.01 = the audio layout (1.00 was MIDI only) */

#define PA_CFG_TOTAL 288

static const uint8_t PA_DEV_DESC[18] = {
    18, 1, 0x10, 0x01,                          /* USB 1.10 */
    0xEF, 0x02, 0x01,                           /* class: miscellaneous, interface association (the functions are described by the IADs) */
    64,                                         /* EP0 = 64 */
    0x09, 0x12, 0x01, 0x00,                     /* VID 0x1209, PID 0x0001 (pid.codes TEST id) */
    PA_USB_DESC_VERSION & 0xFF, PA_USB_DESC_VERSION >> 8, 1, 2, 0, 1    /* strings 1 and 2, no serial, 1 configuration */
};

static const uint8_t PA_CFG_DESC[PA_CFG_TOTAL] = {
    9, 2, PA_CFG_TOTAL & 0xFF, PA_CFG_TOTAL >> 8, 5, 1, 0, 0x80, 50,                  /* 5 interfaces, bus powered, 100 mA */

    /* ---- function 1: USB audio, interfaces 0..2 ------------------------------------------------------------------------------------------------------------ */
    8, 11, 0, 3, 1, 0, 0, 0,                                                          /* interface association: first 0, count 3, audio, no subclass */
    /* interface 0: audio control */
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    10, 0x24, 1, 0x00, 0x01, 52, 0, 2, 1, 2,                                          /* header: ADC 1.00, class-specific total 52, two streaming interfaces (1, 2) */
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,                               /* input terminal 1: USB streaming, 2 channels (left, right) */
    9, 0x24, 3, 2, 0x01, 0x03, 0, 1, 0,                                               /* output terminal 2: speaker, source 1 */
    12, 0x24, 2, 3, 0x01, 0x02, 0, 2, 0x03, 0x00, 0, 0,                               /* input terminal 3: microphone, 2 channels */
    9, 0x24, 3, 4, 0x01, 0x01, 0, 3, 0,                                               /* output terminal 4: USB streaming, source 3 */
    /* interface 1: audio streaming, playback */
    9, 4, 1, 0, 0, 1, 2, 0, 0,                                                        /* alt 0: no endpoint (zero bandwidth) */
    9, 4, 1, 1, 1, 1, 2, 0, 0,                                                        /* alt 1: one endpoint */
    7, 0x24, 1, 1, 1, 0x01, 0x00,                                                     /* general: terminal link 1, delay 1, PCM */
    14, 0x24, 2, 1, 2, 3, 24, 2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00,                                    /* format type I: 2 channels, 3 bytes, 24 bit, one rate: 44100 Hz */
    9, 5, 0x03, 0x09, PA_USB_AUDIO_MAXP & 0xFF, PA_USB_AUDIO_MAXP >> 8, 1, 0, 0,      /* endpoint 3 OUT, isochronous, adaptive, every frame */
    7, 0x25, 1, 1, 0, 0, 0,                                                           /* its audio endpoint descriptor: no controls */
    /* interface 2: audio streaming, recording */
    9, 4, 2, 0, 0, 1, 2, 0, 0,
    9, 4, 2, 1, 1, 1, 2, 0, 0,
    7, 0x24, 1, 4, 1, 0x01, 0x00,                                                     /* general: terminal link 4, delay 1, PCM */
    14, 0x24, 2, 1, 2, 3, 24, 2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00,
    9, 5, 0x83, 0x05, PA_USB_AUDIO_MAXP & 0xFF, PA_USB_AUDIO_MAXP >> 8, 1, 0, 0,      /* endpoint 3 IN, isochronous, asynchronous, every frame */
    7, 0x25, 1, 1, 0, 0, 0,

    /* ---- function 2: USB MIDI, interfaces 3..4 (the layout the unit had, renumbered) ---------------------------------------------------------------------- */
    8, 11, 3, 2, 1, 3, 0, 0,                                                          /* interface association: first 3, count 2, audio, MIDI streaming */
    9, 4, 3, 0, 0, 1, 1, 0, 0,                                                        /* interface 3: audio control (a MIDI function needs one) */
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 4,                                               /* header: ADC 1.00, one streaming interface (4) */
    9, 4, 4, 0, 2, 1, 3, 0, 0,                                                        /* interface 4: MIDI streaming, 2 bulk endpoints */
    7, 0x24, 1, 0x00, 0x01, 65, 0,                                                    /* MS header: bcdMSC 1.00, total 65 */
    6, 0x24, 2, 1, 1, 0,                                                              /* MIDI IN jack, embedded, id 1 */
    6, 0x24, 2, 2, 2, 0,                                                              /* MIDI IN jack, external, id 2 */
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,                                                     /* MIDI OUT jack, embedded, id 3, source 2 */
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,                                                     /* MIDI OUT jack, external, id 4, source 1 */
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,                                                    /* EP1 OUT, bulk, 64 */
    5, 0x25, 1, 1, 1,                                                                 /* its MS endpoint: embedded jack 1 */
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,                                                    /* EP1 IN, bulk, 64 */
    5, 0x25, 1, 1, 3                                                                  /* its MS endpoint: embedded jack 3 */
};

#endif /* PA_USB_DESC_H */
