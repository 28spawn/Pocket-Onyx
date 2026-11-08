/* SPDX-License-Identifier: MPL-2.0 */
/* Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments
 * Ported from chip/ota.c of the Pendel project's pendel-bsp ("step 1 lite").
 * src/sys/pa_ota_session.c - the firmware's half of the vendor's MIDI update, step 1 (include/pa_ota_session.h).
 * This file stays under MPL-2.0 (the licence text is in LICENSES/MPL-2.0.txt next to it); changes: the hooks are this firmware's (pa_ota_hw_*), the handshake is answered by
 * the existing code in usb_ble.c, the boot clean-up recognises the staging area by its own markers (this firmware keeps its settings, a capture and a preset bank in the same flash).
 *
 * Nothing here writes outside the five staging sectors, and nothing is committed before the host says "success".
 */
#include <stdint.h>
#include <string.h>
#include "../../include/pa_ota_session.h"
#include "../../include/pa_ota.h"

#define PA_OTA_IDENT      "Pocket AMP_105"             /* the package's identity string (the handshake answer; pa_ota_device_id() is the same) */
#define PA_OTA_LOADER_ID  "ota-Pocket AMP_105"         /* the identity a staged M-VAVE loader answers: the record names it */
#define PA_OTA_LOADER_MARK "ONYX-LOADER-0001"          /* the Onyx loader's own mark (loader/onyx_loader.c, tools/build_onyx_loader.ps1): it never writes the boot area, so the check of the package's boot area is skipped */
#define PA_OTA_BOARD      "pocketamp"

static uint32_t s_busy;

static uint32_t ota_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t ota_rd24(const uint8_t *p) { return ota_rd16(p) | (uint32_t)p[2] << 16; }
static uint32_t ota_rd32(const uint8_t *p) { return ota_rd24(p) | (uint32_t)p[3] << 24; }
static void ota_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void ota_wr32(uint8_t *p, uint32_t v) { ota_wr16(p, v); ota_wr16(p + 2, v >> 16); }

static uint32_t ota_crc16(const uint8_t *p, uint32_t n, uint32_t c)   /* poly 0x1021, init 0 */
{
    uint32_t i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++)
            c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
        c &= 0xFFFFu;
    }
    return c;
}

static uint32_t ota_jl_enc_k(uint8_t *p, uint32_t n, uint32_t k)   /* JieLi ENC from key k; -> the next key */
{
    while (n--) {
        *p++ ^= (uint8_t)k;
        k = ((k << 1) ^ (k & 0x8000u ? 0x1021u : 0u)) & 0xFFFFu;
    }
    return k;
}

static void ota_jl_enc(uint8_t *p, uint32_t n)   /* UFW header stream encoding, key 0xFFFF */
{
    ota_jl_enc_k(p, n, 0xFFFFu);
}

/* 8-to-7, LSB first */
uint32_t pa_ota_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}

uint32_t pa_ota_unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < max)
                out[o] = (uint8_t)acc;
            o++;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return o;
}

/* The buffers live in a scratch the platform lends for the session (the device: the NAM blob buffer in the pool, free while the main loop is inside the session): this firmware's RAM has no room for 2.5 KB more. */
static uint8_t *ota_rx;                          /* the received frame, 7-bit bytes */
static uint8_t *ota_dec;                         /* the decoded host message */
static uint8_t *ota_hdrb;                        /* 1 KB: the package header at first, then the 512-byte block buffer */
static uint8_t *ota_msg, *ota_wire;              /* the message sent, and its wire form */
#define PA_OTA_SCRATCH_NEED (PA_OTA_FRAME_MAX * 2u + 0x400u + 40u + 50u)
static int ota_scratch_bind(void)
{
    uint32_t n = 0;
    uint8_t *s = pa_ota_hw_scratch(&n);
    if (!s || n < PA_OTA_SCRATCH_NEED) return -1;
    ota_rx = s; ota_dec = s + PA_OTA_FRAME_MAX; ota_hdrb = s + 2u * PA_OTA_FRAME_MAX; ota_msg = ota_hdrb + 0x400u; ota_wire = ota_msg + 40u;
    return 0;
}

static int ota_send_msg(uint32_t cmd, const uint8_t *body, uint32_t n)   /* n <= 32 */
{
    uint32_t i, s = 0, w;
    ota_msg[0] = 0;
    ota_msg[1] = 0x59;
    ota_msg[2] = (uint8_t)cmd;
    ota_msg[3] = (uint8_t)n;
    ota_msg[4] = 0;
    ota_msg[5] = 0;
    for (i = 0; i < n; i++) {
        ota_msg[6 + i] = body[i];
        s += body[i];
    }
    ota_msg[6 + n] = (uint8_t)~s;
    ota_wire[0] = 0xF0;
    w = 1u + pa_ota_pack7(ota_msg, 7u + n, ota_wire + 1);
    ota_wire[w++] = 0xF7;
    return pa_ota_hw_send(ota_wire, w);
}

static void pa_ota_reply_identity(void)
{
    static const char ID[] = PA_OTA_IDENT;         /* == the package marker string */
    uint8_t id[27];
    uint32_t i;
    for (i = 0; i < sizeof id; i++)
        id[i] = i < sizeof ID - 1u ? (uint8_t)ID[i] : 0;
    ota_send_msg(0x11, id, sizeof id);
}

/* decode the pending frame; returns its decoded length (0 = not ours) */
static uint32_t ota_take(void)
{
    const uint8_t *p;
    uint32_t n, d, s = 0, i;
    if (!pa_ota_hw_frame(ota_rx, PA_OTA_FRAME_MAX, &n))
        return 0;
    d = pa_ota_unpack7(ota_rx, n, ota_dec, PA_OTA_FRAME_MAX);
    if (d < 7u || d > PA_OTA_FRAME_MAX || ota_dec[0] != 0 || ota_dec[1] != 0x59 || ota_rd24(ota_dec + 3) + 7u != d)
        return 0;
    for (i = 6; i + 1u < d; i++)
        s += ota_dec[i];
    if ((uint8_t)~s != ota_dec[d - 1u])
        return 0;
    return d;
}

/* one read request (3 tries x 5 s); data -> dst */
static uint32_t ota_deadline;                    /* 0 = none; else pa_ota_hw_ms() limit of the session */
static uint32_t ota_try_ms = 5000u;              /* a try's wait (a firmware's own transfer may ask less) */

static int ota_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    uint8_t q[8];
    uint32_t tries, t, d, i;
    if (ota_deadline && (int32_t)(pa_ota_hw_ms() - ota_deadline) > 0)
        return -1;                                  /* the whole session took too long: give up */
    q[0] = 0;
    ota_wr32(q + 1, addr);
    q[5] = (uint8_t)len;
    q[6] = (uint8_t)(len >> 8);
    q[7] = 0;
    for (tries = 0; tries < 3u; tries++) {
        if (ota_send_msg(0x30, q, 8))
            return -1;
        for (t = pa_ota_hw_ms(); pa_ota_hw_ms() - t < ota_try_ms;) {
            pa_ota_hw_idle();
            d = ota_take();
            if (!d)
                continue;
            if (d == 7u && ota_dec[2] == 0x11) {
                pa_ota_reply_identity();
                continue;
            }
            if (d == len + 15u && ota_dec[2] == 0x30 && ota_rd24(ota_dec + 3) == len + 8u &&
                ota_rd32(ota_dec + 7) == addr && ota_rd24(ota_dec + 11) == len) {
                for (i = 0; i < len; i++)
                    dst[i] = ota_dec[14 + i];
                return 0;
            }
        }
    }
    return -1;
}

static int ota_memeq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* flash.bin's boot area [0, 0x4000), which a writer other than Felucca's puts on the flash: a broken one is the
 * failure only the USB_KEY dongle mends. It must be one the ROM boots: the flash header and the boot record at
 * +0x20 (each ENC 0xFFFF, CRC16), the boot file at 0xA0 a single-bank BANKCB container (its 16-byte header and its
 * payload each encoded from their own start) loaded at 0x01C02000, the header's, the payload's and the record's
 * CRCs right; a container that names its board (m-bootloader: "AC79BLID" + the board at payload +0x40) names the
 * FM-1's. Any bootloader so made: standard bootloader versions, m-bootloader. 0 valid, -5 not, -4 read */
static int ota_boot_valid(uint32_t fl_off, uint32_t fl_len, uint8_t *b)
{
    uint32_t i, n, k = 0xFFFFu, len, dcrc, pwant = 0, pcrc = 0, fcrc = 0;
    if (fl_len < 0x4000u)
        return -5;
    if (ota_read(fl_off, b, 64))
        return -4;
    ota_jl_enc(b, 32);
    ota_jl_enc(b + 32, 32);
    if (ota_crc16(b + 2, 30, 0) != ota_rd16(b) || ota_crc16(b + 34, 30, 0) != ota_rd16(b + 32) ||
        ota_rd32(b + 36) != 0xA0u || b[44] != 0u)
        return -5;
    dcrc = ota_rd16(b + 34);
    len = ota_rd32(b + 40);
    if (len < 16u + 0x50u || len > 0x4000u - 0xA0u)
        return -5;
    for (i = 0; i < len; i += n) {
        uint32_t h = 0;
        n = len - i > 512u ? 512u : len - i;
        if (ota_read(fl_off + 0xA0u + i, b, n))
            return -4;
        if (!i) {                                           /* the container's header */
            ota_jl_enc(b, 16);
            if (ota_crc16(b, 14, 0) != ota_rd16(b + 14) || ota_rd16(b) != 1u || ota_rd16(b + 2) + 16u != len ||
                ota_rd32(b + 4) != 0x01C02000u || ota_rd32(b + 8) != 16u)
                return -5;
            pwant = ota_rd16(b + 12);
            fcrc = ota_crc16(b, 16, 0);
            h = 16;
        }
        k = ota_jl_enc_k(b + h, n - h, k);
        pcrc = ota_crc16(b + h, n - h, pcrc);
        fcrc = ota_crc16(b + h, n - h, fcrc);
        if (!i && ota_memeq(b + 16 + 0x40, (const uint8_t *)"AC79BLID", 8) &&
            !ota_memeq(b + 16 + 0x48, (const uint8_t *)PA_OTA_BOARD, sizeof PA_OTA_BOARD))
            return -5;                                      /* another board's */
    }
    return pcrc == pwant && fcrc == dcrc ? 0 : -5;
}

/* JLFS head "LOADER.BIN" in front of the staged loader (vm_update_defrag), written last */
static int ota_stage_head(uint32_t len)
{
    static const char NAME[16] = "LOADER.BIN";
    uint8_t h[32];
    uint32_t i;
    for (i = 0; i < 32u; i++)
        h[i] = 0;
    ota_wr16(h + 2, 0xFFFFu);
    ota_wr32(h + 4, PA_OTA_AREA);
    ota_wr32(h + 8, len + 32u);
    h[12] = 0x41u;
    h[13] = 0xFFu;
    ota_wr16(h + 14, 0xFFFFu);
    for (i = 0; i < 16u; i++)
        h[16 + i] = (uint8_t)NAME[i];
    ota_wr16(h, ota_crc16(h + 2, 30, 0));
    return pa_ota_hw_prog(PA_OTA_AREA, h, 32);
}

/* UFW header + entry list (hdr: 1 KiB) from the host -> offset / length of
 * flash.bin (entry type 0) and ota.bin (type 100), 0 if absent; -1 read, -2 CRC */
static int ota_ufw(uint8_t *hdr, uint32_t *fl_off, uint32_t *fl_len, uint32_t *ota_off, uint32_t *ota_len)
{
    uint32_t i, nent;
    *fl_off = *fl_len = *ota_off = *ota_len = 0;
    if (ota_read(0, hdr, 512) || ota_read(512, hdr + 512, 512))
        return -1;
    ota_jl_enc(hdr, 0x40);
    nent = ota_rd16(hdr + 8);
    if (ota_crc16(hdr + 2, 0x3E, 0) != ota_rd16(hdr) || nent == 0 || nent > 11u ||
        ota_crc16(hdr + 0x40, nent * 0x50u, 0) != ota_rd16(hdr + 2))
        return -2;
    for (i = 0; i < nent; i++) {
        uint8_t *e = hdr + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 0) {                             /* flash.bin */
            *fl_off = ota_rd32(e + 8);
            *fl_len = ota_rd32(e + 12);
        }
        if (ota_rd16(e) == 100) {                           /* ota.bin */
            *ota_off = ota_rd32(e + 8);
            *ota_len = ota_rd32(e + 12);
        }
    }
    return 0;
}

static int ota_stage(void)                       /* steps 1..6; 0 = host said success */
{
    uint8_t *const hdr = ota_hdrb, *const b = ota_hdrb;       /* the header is finished with before the block buffer is first used */
    uint32_t i, n, ota_off, ota_len, fl_off, fl_len, c, len, own;
    int rc;
    /* 1. UFW header + entry list */
    pa_ota_hw_show(1, 0);
    if ((rc = ota_ufw(hdr, &fl_off, &fl_len, &ota_off, &ota_len)) != 0)
        return rc;
    if (!ota_off)
        return -3;
    /* 2. the package's writer (standard format, Felucca's, m-bootloader's installer, another firmware's), well
     *    made: its JLFS head and inner head right, loaded where the SPL runs it, small enough to stage */
    if (ota_read(ota_off, b, 512))
        return -6;
    len = ota_rd32(b + 8);
    if (ota_crc16(b + 2, 30, 0) != ota_rd16(b) || ota_rd32(b + 4) != 0x20u || b[12] != 0x41u ||
        ota_len != len + 0x20u || len > PA_OTA_AREA_LEN - 0x200u || ota_crc16(b + 0x22, 30, 0) != ota_rd16(b + 0x20) ||
        ota_rd32(b + 0x20 + 8) != 0x01C0A800u)
        return -7;
    for (i = 0x48, own = 0; i + 16u <= 0x48u + 64u && !own; i++)    /* first LZ4 literals: crt0_ldr.S */
        own = ota_memeq(b + i, (const uint8_t *)PA_OTA_LOADER_MARK, 16) && ota_rd32(b + 0x20 + 4) <= 0x14000u;
    /* 3. a flash image with it: Felucca's writer never writes its head (the app area only); any other may,
     *    so its boot area must be a bootable one for the FM-1 (no flash.bin: the writer brings its own) */
    if (fl_off && !own) {
        pa_ota_hw_show(2, 0);
        if ((rc = ota_boot_valid(fl_off, fl_len, b)) != 0)
            return rc;
    }
    /* 4. stage the package's loader: erase, body, verify, head last */
    pa_ota_hw_show(3, 0);
    if (ota_read(ota_off, b, 512))
        return -6;
    c = ota_rd16(b + 2);                                    /* outer dcrc of the body */
    for (i = 0; i < PA_OTA_AREA_LEN; i += 0x1000u)
        if (pa_ota_hw_erase(PA_OTA_AREA + i))
            return -8;
    {
        uint32_t crc = 0;
        for (i = 0; i < len; i += n) {
            n = len - i > 512u ? 512u : len - i;
            if (ota_read(ota_off + 0x20u + i, b, n))
                return -9;
            crc = ota_crc16(b, n, crc);
            if (pa_ota_hw_prog(PA_OTA_AREA + 32u + i, b, n))
                return -10;
        }
        if (crc != c)
            return -11;
        for (i = 0, crc = 0; i < len; i += n) {             /* read back from flash */
            n = len - i > 512u ? 512u : len - i;
            if (pa_ota_hw_read(PA_OTA_AREA + 32u + i, b, n))
                return -11;
            crc = ota_crc16(b, n, crc);
        }
        if (crc != c)
            return -11;
    }
    if (ota_stage_head(len))
        return -12;
    /* 6. the host confirms: nothing is committed without it */
    pa_ota_hw_show(4, 0);
    for (i = 0; i < 4u; i++)
        if (!ota_read(0xE0000000u, b, 8) && ota_memeq(b, (const uint8_t *)"success", 8))
            return 0;
    return -13;
}

static void ota_unstage(void)
{
    uint32_t i;
    for (i = 0; i < PA_OTA_AREA_LEN; i += 0x1000u)
        pa_ota_hw_erase(PA_OTA_AREA + i);
}

/* returns only when nothing was committed (refused, failed, or dry run) */
static int ota_session_inner(void);
int pa_ota_session_run(void)
{
    s_busy = 1u;
    const int rc = ota_session_inner();
    s_busy = 0u;
    return rc;
}

static int ota_session_inner(void)
{
    if (ota_scratch_bind()) return -15;
    uint8_t parm[112];
    uint8_t back[112];
    uint32_t i;
    int rc;
    ota_deadline = pa_ota_hw_ms() + 120000u;          /* step 1 normally takes ~5 s; a stalled host gives up after 2 min */
    rc = ota_stage();
    if (rc) {
        ota_unstage();
        pa_ota_hw_show(9, rc);
        return rc;
    }
    /* 5./7. UPDATA_PARM (update_mode_api_v2 layout), flash copy first */
    for (i = 0; i < sizeof parm; i++)
        parm[i] = 0;
    ota_wr16(parm + 2, 0x5A0Du);
    ota_wr16(parm + 4, 0x5A01u);
    ota_wr16(parm + 6, 0x5441u);
    for (i = 0; i + 1u < sizeof PA_OTA_LOADER_ID && i < 63u; i++)
        parm[8 + i] = (uint8_t)PA_OTA_LOADER_ID[i];     /* the loader's own USB identity */
    ota_wr32(parm + 72, PA_OTA_AREA);
    ota_wr16(parm, ota_crc16(parm + 2, 78, 0));
#if PA_OTA_DRYRUN
    ota_unstage();
    pa_ota_hw_show(9, 1);                                 /* 1 = dry run complete */
    return 1;
#endif
    if ((pa_ota_hw_prog(PA_OTA_RES, parm, sizeof parm) || pa_ota_hw_read(PA_OTA_RES, back, sizeof back) ||
        !ota_memeq(back, parm, sizeof parm))) {
        ota_unstage();
        pa_ota_hw_show(9, -14);
        return -14;
    }
    pa_ota_hw_show(5, 0);
    pa_ota_hw_wipe();                               /* only now: nothing of ours is erased for an update that did not start */
    pa_ota_hw_commit(parm);
    return 0;
}


/* a staging area an update left behind (we are running: the update finished or never started) is erased so the SPL does not run the loader again. This firmware keeps its settings, a capture and a preset
 * bank in the same flash, so the area is recognised by what only a staging leaves: the JLFS head "LOADER.BIN" at its start, or the update record's magic at 0xBCF00. 1 = erased, 0 = none, <0 = read error. */
int pa_ota_boot_cleanup(void)
{
    uint8_t h[32], r[16];
    uint32_t i;
    if (pa_ota_hw_read(PA_OTA_AREA, h, sizeof h) || pa_ota_hw_read(PA_OTA_RES, r, sizeof r))
        return -1;
    int used = ota_memeq(h + 16, (const uint8_t *)"LOADER.BIN", 11) || (ota_rd16(r + 2) == 0x5A0Du && ota_rd16(r + 4) == 0x5A01u && ota_rd16(r + 6) == 0x5441u);
    if (used)
        for (i = 0; i < PA_OTA_AREA_LEN; i += 0x1000u)
            pa_ota_hw_erase(PA_OTA_AREA + i);
    return used;
}

uint32_t pa_ota_session_busy(void) { return s_busy; }

#if (defined(PA_HOST_SIM) || defined(PA_HOST_BUILD)) && !defined(PA_OTA_NO_STUBS)
/* the host builds (the emulator, the browser, the host suites) have no USB and no update: the hooks refuse, so a session ends at once (the tests define their own and set PA_OTA_NO_STUBS) */
int      pa_ota_hw_send(const uint8_t *msg, uint32_t n) { (void)msg; (void)n; return -1; }
int      pa_ota_hw_frame(uint8_t *buf, uint32_t cap, uint32_t *n) { (void)buf; (void)cap; (void)n; return 0; }
uint8_t *pa_ota_hw_scratch(uint32_t *n) { *n = 0u; return 0; }
uint32_t pa_ota_hw_ms(void) { static uint32_t t; return t += 1000u; }
void     pa_ota_hw_idle(void) {}
int      pa_ota_hw_erase(uint32_t off) { (void)off; return -8; }
int      pa_ota_hw_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -8; }
int      pa_ota_hw_read(uint32_t off, void *p, uint32_t n) { (void)off; memset(p, 0xFF, n); return 0; }
void     pa_ota_hw_show(uint32_t step, int32_t code) { (void)step; (void)code; }
void     pa_ota_hw_wipe(void) {}
void     pa_ota_hw_commit(const uint8_t *parm) { (void)parm; }
#endif
