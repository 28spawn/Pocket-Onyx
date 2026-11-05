/*
 * include/pa_xip.h - verified reads of constant data that lives in the flash (XIP).
 *
 * With the second core running its code from the same flash, reads of constants on this core (the font, strings, the cabinet and amp
 * tables) can come back wrong: a string drew as its first letter repeated, a cabinet became a runaway signal. These helpers read every
 * byte twice and only accept two reads that agree (up to 5 tries). The number of disagreements is counted at SRAM 0x01C7D580:
 *   [0] reads that disagreed   [1] address of the last one   [2] reads that never agreed (the last value was used)
 * `tools/jl_ram_read.py slots` prints it: a non-zero count proves the flash reads are unreliable. Host builds read plainly.
 */
#ifndef PA_XIP_H
#define PA_XIP_H

#include <stdint.h>

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define PA_XIP_STATS ((volatile uint32_t *)(uintptr_t)0x01C7D580u)
static inline uint8_t pa_xip_byte(const void *p) {
    uint8_t a = 0;
    for (int t = 0; t < 5; t++) {
        a = *(const volatile uint8_t *)p;
        const uint8_t b = *(const volatile uint8_t *)p;
        if (a == b) return a;
        PA_XIP_STATS[0] = PA_XIP_STATS[0] + 1u; PA_XIP_STATS[1] = (uint32_t)(uintptr_t)p;
    }
    PA_XIP_STATS[2] = PA_XIP_STATS[2] + 1u;
    return a;
}
static inline void pa_xip_read(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        const uint32_t c = n > 64u ? 64u : n;
        uint8_t a[64], b[64];
        int ok = 0;
        for (int t = 0; t < 5 && !ok; t++) {
            for (uint32_t i = 0; i < c; i++) a[i] = ((const volatile uint8_t *)s)[i];
            for (uint32_t i = 0; i < c; i++) b[i] = ((const volatile uint8_t *)s)[i];
            ok = 1;
            for (uint32_t i = 0; i < c; i++) if (a[i] != b[i]) { ok = 0; PA_XIP_STATS[1] = (uint32_t)(uintptr_t)(s + i); break; }
            if (!ok) PA_XIP_STATS[0] = PA_XIP_STATS[0] + 1u;
        }
        if (!ok) PA_XIP_STATS[2] = PA_XIP_STATS[2] + 1u;
        for (uint32_t i = 0; i < c; i++) d[i] = a[i];
        d += c; s += c; n -= c;
    }
}
#else
#include <string.h>
static inline uint8_t pa_xip_byte(const void *p) { return *(const uint8_t *)p; }
static inline void pa_xip_read(void *dst, const void *src, uint32_t n) { memcpy(dst, src, n); }
#endif

#endif /* PA_XIP_H */
