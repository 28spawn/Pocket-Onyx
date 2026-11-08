# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
	.text
	.file	"src/nam/pa_wavenet.c"
	.globl	pa_wavenet_reset
	.p2align	1
	.type	pa_wavenet_reset,@function
pa_wavenet_reset:                       ; @pa_wavenet_reset
; BB#0:
	[--sp] = {rets, r6-r4}
	r4 = r0
	if (r4 == 0) goto .LBB0_2
; BB#1:
	r0 = r4 + 7508
	r2 = 102468
	r1 = 0
	r6 = 0
	call memset
	r0 = 109976
	r0 += r4
	r1 = 0
	r2 = 46
	call memset
	r0 = 119396
	r5 = r4 + r0
	r1 = 0
	r2 = 192
	r0 = r5
	call memset
	r0 = 117884
	r0 += r4
	r1 = 0
	r2 = 948
	call memset
	r0 = 119588
	h[++r4=r0] = r6
	[r4+8] = r6
	[r4+4] = r6
	[r4+24] = r6
	[r4+20] = r6
	[r4+16] = r6
	[r4+12] = r6
	[r4+40] = r6
	[r4+36] = r6
	[r4+32] = r6
	[r4+28] = r6
	[r4+48] = r6
	[r4+44] = r6
	r0 = 0
	r1 = 0
	d[r5+244] = r1_r0
.LBB0_2:
	{pc, r6-r4} = [sp++]
.Lfunc_end0:
	.size	pa_wavenet_reset, .Lfunc_end0-pa_wavenet_reset

	.globl	pa_wavenet_init_default
	.p2align	1
	.type	pa_wavenet_init_default,@function
pa_wavenet_init_default:                ; @pa_wavenet_init_default
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -4
	r4 = r0
	if (r4 == 0) goto .LBB1_4
; BB#1:
	r2 = 119712
	r1 = 0
	r5 = 0
	call memset
	r1 = 3
	r2 = r5 + 0x3F800000  #
	b[r4+0] = r1
	r3 = r2
	d[r4+8] = r3_r2
	r2 = r5 + 0x3F000000
	r3 = 1051931443
	d[r4+24] = r3_r2
	r0 = 1045220557
	[r4+32] = r0
	r2 = r4 + 24
	r9 = r0 + 0x800000  #
	[sp] = r4                       ; 4-byte Folded Spill
	r10 = r2 + 12
	r7 = 1041865114
	r11 = r2 + 16
	r12 = r2 + 20
	r13 = r2 + 24
	r4 = r9 + 0xFF000000
	r14 = r2 + 40
	r15 = r2 + 56
.LBB1_2:                                ; =>This Inner Loop Header: Depth=1
	r0 = g_pa_wn_specs
	r0 = b[r5+r0] (u)
	r6 = r1 + r0
	r6 += -1
	[r2+r6<<2] = r9
	r8 = r0 << 2
	r3 = r6 + r8
	[r2+r3<<2] = r9
	r3 = r0 << 3
	r3 += r6
	[r2+r3<<2] = r9
	r0 = r0 * 0x9
	r0 += r1
	[r10+r0<<2] = r7
	[r11+r0<<2] = r7
	[r12+r0<<2] = r7
	[r13+r0<<2] = r4
	[r14+r0<<2] = r4
	[r15+r0<<2] = r4
	r5 += 8
	r1 = r0 + 18
	if (r5 != 184) goto .LBB1_2
; BB#3:
	r0 = 1051260355
	r2 = 1858  #
	r4 = [sp]                       ; 4-byte Folded Reload
	r1 = r4 + 7368
	[r1+0] = r0
	[r4+r2<<2] = r0
	r2 += 16
	[r4+r2<<2] = r0
	r2 = 0
	r3 = r2 + 0x3F800000
	d[r1+132] = r3_r2
	r0 = r1 + 140
	r2 = 102468
	r1 = 0
	r5 = 0
	call memset
	r0 = 109976
	r0 += r4
	r1 = 0
	r2 = 46
	call memset
	r0 = 119396
	r0 += r4
	r1 = 0
	r2 = 192
	call memset
	r0 = 117884
	r0 += r4
	r1 = 0
	r2 = 948
	call memset
	r0 = 119588
	h[++r0=r4] = r5
	[r0+8] = r5
	[r0+4] = r5
	[r0+24] = r5
	[r0+20] = r5
	[r0+16] = r5
	[r0+12] = r5
	[r0+40] = r5
	[r0+36] = r5
	[r0+32] = r5
	[r0+28] = r5
	[r0+48] = r5
	[r0+44] = r5
	[r0+52] = r5
	[r0+56] = r5
	r0 = 1
	b[r4+1] = r0
	b[r4+2] = r0
.LBB1_4:
	sp += 4
	{pc, r15-r4} = [sp++]
.Lfunc_end1:
	.size	pa_wavenet_init_default, .Lfunc_end1-pa_wavenet_init_default

	.p2align	1
	.type	wn_post_mix_c,@function
wn_post_mix_c:                          ; @wn_post_mix_c
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -52
	r12 = r3  #
	r4 = [sp+112]
	r13 = r2
	if (r4 == 0) goto .LBB2_17
; BB#1:
	r14 = [sp+108]
	r15 = [sp+104]
	r11 = 0  #
	[sp+4] = r0                     ; 4-byte Folded Spill
	r7 = 2147483647
	r6 = 0
	[sp+12] = r14                   ; 4-byte Folded Spill
	[sp+8] = r1                     ; 4-byte Folded Spill
	[sp] = r15                      ; 4-byte Folded Spill
.LBB2_2:                                ; =>This Inner Loop Header: Depth=1
	r2 = [r0+r6]
	[sp+28] = r4                    ; 4-byte Folded Spill
	ifs (r2 > -1) goto .LBB2_4
; BB#3:                                 ;   in Loop: Header=BB2_2 Depth=1
	r5_r4 = r3_r2
	r2 = 42949632
	r5_r4 = r4 * r2 (s)
	r2 = r5
.LBB2_4:                                ;   in Loop: Header=BB2_2 Depth=1
	r8 = [r1+r6]
	r9 = r8 >>> 31
	r3 = r2 >>> 31
	r4 = r8 + r2
	d[sp+40] = r3_r2                ; 8-byte Folded Spill
	r5 = r9 + r3 + c
	r8 = r4 - r7
	r9 = r5 - r11 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r2 = ~r2
	r3 = r8 | r9
	if ((r3 & r2) != 0) goto .LBB2_6
; BB#5:                                 ;   in Loop: Header=BB2_2 Depth=1
	r8 = r6
	r6 = r4 - 0x80000000
	r2 = -1
	r2 = r5 - r2 - !c
	r3 = psr
	r7 = sextra(r3, p:0, l:1)
	r3 = sextra(r3, p:3, l:1)
	r3 ^= r7
	r3 = ~r3
	r2 |= r6
	if ((r2 & r3) == 0) {
		r4 = r11 + 0x80000000
		r5 = r11 + 0xFFFFFFFF
	}
	r7 = r4
	r6 = r8
.LBB2_6:                                ;   in Loop: Header=BB2_2 Depth=1
	[r1+r6] = r7
	r5 = r0 + r6
	r2 = [r5+4] 
	ifs (r2 < 0) {
		r0 = 42949632
		r3_r2 = r2 * r0 (s)
		r2 = r3
	}
	r10 = r1 + r6
	r7 = r10
	r0 = [++r7=4]
	r1 = r0 >>> 31
	r3 = r2 >>> 31
	r8 = r0 + r2
	d[sp+32] = r3_r2                ; 8-byte Folded Spill
	r9 = r1 + r3 + c
	r0 = 2147483647
	r4 = r0
	r0 = r8 - r4
	r1 = r9 - r11 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r2 = ~r2
	r1 |= r0
	r0 = r4
	if ((r1 & r2) != 0) goto .LBB2_8
; BB#7:                                 ;   in Loop: Header=BB2_2 Depth=1
	r0 = r8 - 0x80000000
	r1 = -1
	r1 = r9 - r1 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r2 = ~r2
	r0 |= r1
	if ((r0 & r2) == 0) {
		r8 = r4 + 1
		r9 = r4 + 0x80000000
	}
	r0 = r8
.LBB2_8:                                ;   in Loop: Header=BB2_2 Depth=1
	[r7+0] = r0
	r8 = [r5+8]
	ifs (r8 <= -1) {
		r0 = 42949632
		r1_r0 = r8 * r0 (s)
		r8 = r1
	}
	r7 = r4
	r0 = [++r10=8]
	r1 = r0 >>> 31
	r9 = r8 >>> 31
	r4 = r0 + r8
	r5 = r1 + r9 + c
	r0 = r4 - r7
	r1 = r5 - r11 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r2 = ~r2
	r1 |= r0
	r0 = r7
	r3 = 0
	if ((r1 & r2) != 0) goto .LBB2_10
; BB#9:                                 ;   in Loop: Header=BB2_2 Depth=1
	r0 = r4 - 0x80000000
	r1 = -1
	r1 = r5 - r1 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r2 = ~r2
	r0 |= r1
	if ((r0 & r2) == 0) {
		r4 = r7 + 1
		r5 = r7 + 0x80000000
	}
	r0 = r4
	r3 = 0
.LBB2_10:                               ;   in Loop: Header=BB2_2 Depth=1
	[r10+0] = r0
	r11_r10 = d[r12+0]
	r0 = [r13+0]
	r1 = [r13+12]
	r2 = [r13+24]
	r5_r4 = d[sp+40]                ; 8-byte Folded Reload
	r11_r10 += r4 * r0 (s)
	r5_r4 = d[sp+32]                ; 8-byte Folded Reload
	r11_r10 += r4 * r1 (s)
	r0 = [r15+r6]
	r11_r10 += r8 * r2 (s)
	r11_r10 <<= 8
	r11_r10 >>>= 32
	r1 = r0 >>> 31
	r4 = r0 + r10
	r5 = r1 + r11 + c
	r0 = r4 - r7
	r1 = r5 - r3 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r3 = 0
	r2 = ~r2
	r1 |= r0
	r0 = r7
	if ((r1 & r2) != 0) goto .LBB2_12
; BB#11:                                ;   in Loop: Header=BB2_2 Depth=1
	r0 = r4 - 0x80000000
	r1 = -1
	r1 = r5 - r1 - !c
	r3 = psr
	r7 = sextra(r3, p:0, l:1)
	r3 = sextra(r3, p:3, l:1)
	r3 ^= r7
	r3 = ~r3
	r0 |= r1
	if ((r0 & r3) == 0) {
		r4 = 0x80000000
		r5 = 0xFFFFFFFF
	}
	r0 = r4
	r3 = 0
	r7 = 2147483647
.LBB2_12:                               ;   in Loop: Header=BB2_2 Depth=1
	[r14+r6] = r0
	r11_r10 = d[r12+8]
	r0 = [r13+4]
	r1 = [r13+16]
	r5_r4 = d[sp+40]                ; 8-byte Folded Reload
	r11_r10 += r4 * r0 (s)
	r0 = [r13+28]
	r5_r4 = d[sp+32]                ; 8-byte Folded Reload
	r11_r10 += r4 * r1 (s)
	r5_r4 = r9_r8
	r8 = r15 + r6
	r2 = r12
	r15 = r13
	r12 = [r8+4]
	d[sp+16] = r5_r4                ; 8-byte Folded Spill
	r11_r10 += r4 * r0 (s)
	r11_r10 <<= 8
	r11_r10 >>>= 32
	r13 = r12 >>> 31
	r4 = r12 + r10
	r5 = r13 + r11 + c
	r0 = r4 - r7
	r1 = r5 - r3 - !c
	r3 = psr
	r7 = sextra(r3, p:0, l:1)
	r3 = sextra(r3, p:3, l:1)
	r3 ^= r7
	r7 = 2147483647
	r3 = ~r3
	r1 |= r0
	r0 = r7
	if ((r1 & r3) != 0) goto .LBB2_14
; BB#13:                                ;   in Loop: Header=BB2_2 Depth=1
	r10 = r6
	r0 = r4 - 0x80000000
	r1 = -1
	r1 = r5 - r1 - !c
	r3 = psr
	r7 = sextra(r3, p:0, l:1)
	r3 = sextra(r3, p:3, l:1)
	r3 ^= r7
	r3 = ~r3
	r0 |= r1
	if ((r0 & r3) == 0) {
		r4 = 0x80000000
		r5 = 0xFFFFFFFF
	}
	r0 = r4
	r14 = [sp+12]                   ; 4-byte Folded Reload
	r7 = 2147483647
.LBB2_14:                               ;   in Loop: Header=BB2_2 Depth=1
	r14 += r6
	[r14+4] = r0
	r5 = r2
	r11_r10 = d[r5+16]
	r0 = [r15+8]
	r1 = [r15+20]
	r3 = [r15+32]
	r13_r12 = d[sp+40]              ; 8-byte Folded Reload
	r11_r10 += r12 * r0 (s)
	r13_r12 = d[sp+32]              ; 8-byte Folded Reload
	r11_r10 += r12 * r1 (s)
	r0 = [r8+8]
	r9_r8 = d[sp+16]                ; 8-byte Folded Reload
	r11_r10 += r8 * r3 (s)
	r11_r10 <<= 8
	r11_r10 >>>= 32
	r1 = r0 >>> 31
	r8 = r0 + r10
	r9 = r1 + r11 + c
	r0 = r8 - r7
	r1 = 0
	r1 = r9 - r1 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)  #
	r4 = [sp+28]                    ; 4-byte Folded Reload
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r1 |= r0
	r3 = r7
	r12 = r5
	r2 = ~r2  #
	r0 = [sp+4]                     ; 4-byte Folded Reload
	r13 = r15
	r15 = [sp]                      ; 4-byte Folded Reload
	r11 = 0
	if ((r1 & r2) != 0) goto .LBB2_16
; BB#15:                                ;   in Loop: Header=BB2_2 Depth=1
	r10 = r6
	r6 = r8 - 0x80000000
	r1 = -1
	r1 = r9 - r1 - !c
	r2 = psr
	r3 = sextra(r2, p:0, l:1)
	r2 = sextra(r2, p:3, l:1)
	r2 ^= r3
	r2 = ~r2
	r1 |= r6
	if ((r1 & r2) == 0) {
		r8 = r7 + 1
		r9 = r7 + 0x80000000
	}
	r3 = r8
	r6 = r10
.LBB2_16:                               ;   in Loop: Header=BB2_2 Depth=1
	[r14+8] = r3
	r6 += 12
	r1 = [sp+8]                     ; 4-byte Folded Reload
	r14 = [sp+12]                   ; 4-byte Folded Reload
	if (--r4 != 0) goto .LBB2_2
.LBB2_17:
	sp += 52
	{pc, r15-r4} = [sp++]
.Lfunc_end2:
	.size	wn_post_mix_c, .Lfunc_end2-wn_post_mix_c

	.p2align	1
	.type	wn_post_head_c,@function
wn_post_head_c:                         ; @wn_post_head_c
; BB#0:
	[--sp] = {rets, r15-r4}
	if (r2 == 0) goto .LBB3_9
; BB#1:
	r4 = 2147483647
	r14 = 0
	r15 = 0xFFFFFFFF
.LBB3_2:                                ; =>This Inner Loop Header: Depth=1
	r10 = [r0+0]
	ifs (r10 < 0) {
		r3 = 42949632
		r9_r8 = r10 * r3 (s)
		r10 = r9
	}
	r12 = [r1+0]
	r11 = r10 >>> 31
	r13 = r12 >>> 31
	r8 = r12 + r10
	r9 = r13 + r11 + c
	r7 = r8 - r4
	r3 = r9 - r14 - !c
	r6 = psr
	r5 = sextra(r6, p:0, l:1)
	r6 = sextra(r6, p:3, l:1)
	r5 ^= r6
	r5 = ~r5
	r3 |= r7
	r7 = r4
	if ((r3 & r5) != 0) goto .LBB3_4
; BB#3:                                 ;   in Loop: Header=BB3_2 Depth=1
	r3 = r8 - 0x80000000
	r5 = r9 - r15 - !c
	r6 = psr
	r7 = sextra(r6, p:0, l:1)
	r6 = sextra(r6, p:3, l:1)
	r6 ^= r7
	r6 = ~r6
	r3 |= r5
	if ((r3 & r6) == 0) {
		r8 = r4 + 1
		r9 = r4 + 0x80000000
	}
	r7 = r8
.LBB3_4:                                ;   in Loop: Header=BB3_2 Depth=1
	[r1+0] = r7
	r10 = [r0+4]
	ifs (r10 <= -1) {
		r3 = 42949632
		r7_r6 = r10 * r3 (s)
		r10 = r7
	}
	r11 = r10 >>> 31  #
	r6 = [r1+4] 
	r7 = r6 >>> 31
	r8 = r6 + r10
	r9 = r7 + r11 + c
	r3 = r8 - r4
	r5 = r9 - r14 - !c
	r6 = psr
	r7 = sextra(r6, p:0, l:1)
	r6 = sextra(r6, p:3, l:1)
	r6 ^= r7
	r6 = ~r6
	r3 |= r5
	r7 = r4
	if ((r3 & r6) != 0) goto .LBB3_6
; BB#5:                                 ;   in Loop: Header=BB3_2 Depth=1
	r3 = r8 - 0x80000000
	r5 = r9 - r15 - !c
	r6 = psr
	r7 = sextra(r6, p:0, l:1)
	r6 = sextra(r6, p:3, l:1)
	r6 ^= r7
	r6 = ~r6
	r3 |= r5
	if ((r3 & r6) == 0) {
		r8 = r4 + 1
		r9 = r4 + 0x80000000
	}
	r7 = r8
.LBB3_6:                                ;   in Loop: Header=BB3_2 Depth=1
	[r1+4] = r7
	r10 = [r0+8]
	ifs (r10 <= -1) {
		r3 = 42949632
		r7_r6 = r10 * r3 (s)
		r10 = r7
	}
	r11 = r10 >>> 31  #
	r6 = [r1+8] 
	r7 = r6 >>> 31
	r8 = r6 + r10
	r9 = r7 + r11 + c
	r3 = r8 - r4
	r5 = r9 - r14 - !c
	r6 = psr
	r7 = sextra(r6, p:0, l:1)
	r6 = sextra(r6, p:3, l:1)
	r6 ^= r7
	r6 = ~r6
	r3 |= r5
	r7 = r4
	if ((r3 & r6) != 0) goto .LBB3_8
; BB#7:                                 ;   in Loop: Header=BB3_2 Depth=1
	r3 = r8 - 0x80000000
	r5 = r9 - r15 - !c
	r6 = psr
	r7 = sextra(r6, p:0, l:1)
	r6 = sextra(r6, p:3, l:1)
	r6 ^= r7
	r6 = ~r6
	r3 |= r5
	if ((r3 & r6) == 0) {
		r8 = r4 + 1
		r9 = r4 + 0x80000000
	}
	r7 = r8
.LBB3_8:                                ;   in Loop: Header=BB3_2 Depth=1
	[r1+8] = r7
	r0 += 12
	r1 += 12
	if (--r2 != 0) goto .LBB3_2
.LBB3_9:
	{pc, r15-r4} = [sp++]
.Lfunc_end3:
	.size	wn_post_head_c, .Lfunc_end3-wn_post_head_c

	.globl	pa_wavenet_load_weights
	.p2align	1
	.type	pa_wavenet_load_weights,@function
pa_wavenet_load_weights:                ; @pa_wavenet_load_weights
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -1276
	r9 = r3
	r7 = r2
	r6 = r1
	r8 = -1
	if (r0 == 0) goto .LBB4_110
; BB#1:
	if (r6 == 0) goto .LBB4_110
; BB#2:
	r1 = r7 | 0x1
	r8 = -2
	if (r1 != 1871) goto .LBB4_110
; BB#3:
	r1 = 3
	[sp+120] = r0                   ; 4-byte Folded Spill
	b[r0+0] = r1
	if (r7 == 0) goto .LBB4_7
; BB#4:
	r4 = 0
	r5 = r6
.LBB4_5:                                ; =>This Inner Loop Header: Depth=1
	r0 = [r5++=4] 
	call __fpclassifyf
	if (r0 <= 1) goto .LBB4_11
; BB#6:                                 ;   in Loop: Header=BB4_5 Depth=1
	r4 += 1
	if (r4 < r7) goto .LBB4_5
.LBB4_7:
	r0 = r9
	call __fpclassifyf
	r8 = -3  #
	r5 = [sp+120]                   ; 4-byte Folded Reload
	if (r0 < 2) goto .LBB4_110
; BB#8:
	r0 = [r5+12] 
	call __fpclassifyf
	if (r0 <= 1) {
		[r5+12] = 0x3F800000
	}
	r0 = r5 + 24
	r2 = r7 << 2
	r1 = r6
	call memcpy
	if (r7 != 1871) goto .LBB4_12
; BB#9:
	r0 = 1876
	r0 = [r5+r0<<2]
	r4 = r5
	[++r4=8] = r0
	call __fpclassifyf
	if (r0 > 1) goto .LBB4_14
; BB#10:
	r9 = 0x3F800000
	goto .LBB4_13
.LBB4_11:
	r8 = -3
	goto .LBB4_110
.LBB4_12:
	r4 = r5 + 8
.LBB4_13:
	[r4+0] = r9
.LBB4_14:
	r0 = [r5+12] 
	r1 = 981668463
	iff (r0 < r1) {
		[r5+12] = 0x3F800000
	}
	r0 = 110020
	r3 = r5 + r0
	r0 = 0
	r12 = 0  #
	b[r3+2] = r0
	b[r3+3] = r0
	b[r3+4] = r0
	r1 = [r5+24] 
	r2 = 117872
	[++r2=r5] = r1
	r1 = [r5+28] 
	[r2+4] = r1
	r1 = [r5+32] 
	[r2+8] = r1
	r1 = r3 + 12
	[sp+112] = r1                   ; 4-byte Folded Spill
	r1 = r3 + 16
	[sp+104] = r1                   ; 4-byte Folded Spill
	r1 = r3 + 20
	[sp+96] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 24
	[sp+60] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 28
	r1 = r3 + 32  #
	[sp+56] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 36  #
	[sp+52] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 40  #
	[sp+48] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 44  #
	[sp+44] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 48  #
	[sp+40] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 52  #
	[sp+36] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 56  #
	[sp+32] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 64  #
	[sp+28] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 68  #
	[sp+24] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 72  #
	[sp+20] = r1                    ; 4-byte Folded Spill
	r1 = r3 + 76  #
	[sp+16] = r1                    ; 4-byte Folded Spill
	r10 = r0 + 0x4B800000  #
	[sp+64] = r3                    ; 4-byte Folded Spill
	[sp+12] = r1                    ; 4-byte Folded Spill
	r1 = 3
	r2 = 0
.LBB4_15:                               ; =>This Loop Header: Depth=1
                                        ;     Child Loop BB4_16 Depth 2
                                        ;       Child Loop BB4_17 Depth 3
	r14 = 0  #
	[sp+68] = r2                    ; 4-byte Folded Spill
	r2 = r2 << 3
	r3 = g_pa_wn_specs
	r7 = b[r3+r2] (u)
	r2 = r7 << 3
	r5 = [sp+120]                   ; 4-byte Folded Reload
	r8 = r5 + r2
	r2 = r7 * 0xC
	r2 = r1 + r7  #
	[sp+76] = r2                    ; 4-byte Folded Spill
	r2 = r2 << 2
	r11 = r2 + 24
	r2 = r7 * 0x9
	r2 += r1
	[sp+80] = r2                    ; 4-byte Folded Spill
	r2 += 3
	[sp+72] = r2                    ; 4-byte Folded Spill
	r1 = r1 << 2
	r4 = r1 + 24
.LBB4_16:                               ;   Parent Loop BB4_15 Depth=1
                                        ; =>  This Loop Header: Depth=2
                                        ;       Child Loop BB4_17 Depth 3
	[sp+92] = r14                   ; 4-byte Folded Spill
	[sp+88] = r11                   ; 4-byte Folded Spill
	r9 = r11
	r15 = 0  #
	[sp+84] = r4                    ; 4-byte Folded Spill
	r11 = r4
	r6 = r0
	r13 = r12
	r4 = -822083589
.LBB4_17:                               ;   Parent Loop BB4_15 Depth=1
                                        ;     Parent Loop BB4_16 Depth=2
                                        ; =>    This Inner Loop Header: Depth=3
	r0 = [r5+r11]
	r0 = r0 * r10 (f)
	r14 = 1
	r12 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_20
; BB#18:                                ;   in Loop: Header=BB4_17 Depth=3
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_20
; BB#19:                                ;   in Loop: Header=BB4_17 Depth=3
	call lrintf
	r1 = r0
	r12 = r13
.LBB4_20:                               ;   in Loop: Header=BB4_17 Depth=3
	r0 = [sp+112]                   ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = [r5+r9]
	r1 = r0 * r10 (f)
	r5 = 0
	r0 = 0
	iff (r1 u<= r4) goto .LBB4_23
; BB#21:                                ;   in Loop: Header=BB4_17 Depth=3
	r2 = r4 + 0x80000000
	iff (r1 u>= r2) goto .LBB4_23
; BB#22:                                ;   in Loop: Header=BB4_17 Depth=3
	r0 = r1
	call lrintf
	r14 = r12
.LBB4_23:                               ;   in Loop: Header=BB4_17 Depth=3
	r13 = 1  #
	r1 = [sp+104]                   ; 4-byte Folded Reload
	[r1+r6<<2] = r0
	r0 = [r8+r11]
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_26
; BB#24:                                ;   in Loop: Header=BB4_17 Depth=3
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_26
; BB#25:                                ;   in Loop: Header=BB4_17 Depth=3
	call lrintf
	r5 = r0
	r13 = r14
.LBB4_26:                               ;   in Loop: Header=BB4_17 Depth=3
	r9 = r9 + 4  #
	r0 = [sp+96]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r5
	r11 = r11 + 4  #
	r5 = [sp+120]                   ; 4-byte Folded Reload
	r15 = r15 + 1
	r6 += 3
	ifs (r15 < r7) goto .LBB4_17
; BB#27:                                ;   in Loop: Header=BB4_16 Depth=2
	r9 = 0  #
	r0 = [sp+72]                    ; 4-byte Folded Reload
	r14 = [sp+92]                   ; 4-byte Folded Reload
	r0 += r14
	r0 = r0 << 2
	r0 += r5
	r12 = 1  #
	r0 = [r0+24] 
	r0 = r0 * r10 (f)
	r5 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_30
; BB#28:                                ;   in Loop: Header=BB4_16 Depth=2
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_30
; BB#29:                                ;   in Loop: Header=BB4_16 Depth=2
	call lrintf
	r1 = r0
	r5 = r13
.LBB4_30:                               ;   in Loop: Header=BB4_16 Depth=2
	r0 = [sp+112]                   ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = [sp+80]                    ; 4-byte Folded Reload
	r0 += r14  #
	r1 = [sp+120]                   ; 4-byte Folded Reload
	r0 = r0 << 2
	r0 += r1
	r0 = [r0+24] 
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_33
; BB#31:                                ;   in Loop: Header=BB4_16 Depth=2
	r1 = r4 + 0x80000000  #
	r4 = [sp+84]                    ; 4-byte Folded Reload
	r11 = [sp+88]                   ; 4-byte Folded Reload
	iff (r0 u>= r1) goto .LBB4_34
; BB#32:                                ;   in Loop: Header=BB4_16 Depth=2
	call lrintf
	r9 = r0
	r12 = r5
	goto .LBB4_34
.LBB4_33:                               ;   in Loop: Header=BB4_16 Depth=2
	r11 = [sp+88]                   ; 4-byte Folded Reload
	r4 = [sp+84]                    ; 4-byte Folded Reload
.LBB4_34:                               ;   in Loop: Header=BB4_16 Depth=2
	r0 = [sp+104]                   ; 4-byte Folded Reload
	[r0+r6<<2] = r9
	r0 = [sp+76]                    ; 4-byte Folded Reload
	r11 += r0  #
	r5 = [sp+120]                   ; 4-byte Folded Reload
	r4 += r0
	r14 = r14 + 1
	r0 = r6 + 2
	if (r14 != 3) goto .LBB4_16
; BB#35:                                ;   in Loop: Header=BB4_15 Depth=1
	r8 = 1  #
	r0 = [sp+80]                    ; 4-byte Folded Reload
	r0 = r0 << 2
	r11 = r5 + r0
	r0 = [r11+48]
	r0 = r0 * r10 (f)
	r5 = 0
	r7 = 1
	r1 = 0
	r4 = -822083589
	iff (r0 u<= r4) goto .LBB4_38
; BB#36:                                ;   in Loop: Header=BB4_15 Depth=1
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_38
; BB#37:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r1 = r0
	r7 = r12
.LBB4_38:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = [sp+96]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = [r11+60]
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_41
; BB#39:                                ;   in Loop: Header=BB4_15 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_41
; BB#40:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r5 = r0
	r8 = r7
.LBB4_41:                               ;   in Loop: Header=BB4_15 Depth=1
	r9 = 1  #
	r0 = [sp+60]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r5
	r0 = [r11+72]
	r0 = r0 * r10 (f)
	r5 = 0
	r7 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_44
; BB#42:                                ;   in Loop: Header=BB4_15 Depth=1
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_44
; BB#43:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r1 = r0
	r7 = r8
.LBB4_44:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = [sp+56]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = [r11+52]
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_47
; BB#45:                                ;   in Loop: Header=BB4_15 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_47
; BB#46:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r5 = r0
	r9 = r7
.LBB4_47:                               ;   in Loop: Header=BB4_15 Depth=1
	r8 = 1  #
	r0 = [sp+52]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r5
	r0 = [r11+64]
	r0 = r0 * r10 (f)
	r5 = 0
	r7 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_50
; BB#48:                                ;   in Loop: Header=BB4_15 Depth=1
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_50
; BB#49:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r1 = r0
	r7 = r9
.LBB4_50:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = [sp+48]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = [r11+76]
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_53
; BB#51:                                ;   in Loop: Header=BB4_15 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_53
; BB#52:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r5 = r0
	r8 = r7
.LBB4_53:                               ;   in Loop: Header=BB4_15 Depth=1
	r9 = 1  #
	r0 = [sp+44]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r5
	r0 = [r11+56]
	r0 = r0 * r10 (f)
	r5 = 0
	r7 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_56
; BB#54:                                ;   in Loop: Header=BB4_15 Depth=1
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_56
; BB#55:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r1 = r0
	r7 = r8
.LBB4_56:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = [sp+40]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = [r11+68]
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_59
; BB#57:                                ;   in Loop: Header=BB4_15 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_59
; BB#58:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r5 = r0
	r9 = r7
.LBB4_59:                               ;   in Loop: Header=BB4_15 Depth=1
	r8 = 1  #
	r0 = [sp+36]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r5
	r0 = [r11+80]
	r1 = [sp+80]                    ; 4-byte Folded Reload
	r12 = r1 + 15
	r0 = r0 * r10 (f)
	r5 = 0
	r7 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_62
; BB#60:                                ;   in Loop: Header=BB4_15 Depth=1
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_62
; BB#61:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r1 = r0
	r7 = r9
.LBB4_62:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = [sp+32]                    ; 4-byte Folded Reload
	[r0+r6<<2] = r1
	r0 = r12 << 2  #
	r1 = [sp+120]                   ; 4-byte Folded Reload
	r0 += r1
	r9 = r5 + 0x4A000000  #
	r0 = [r0+24] 
	r0 = r0 * r9 (f)
	iff (r0 u<= r4) goto .LBB4_65
; BB#63:                                ;   in Loop: Header=BB4_15 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_65
; BB#64:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r5 = r0
	r8 = r7
.LBB4_65:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = r5 << 24
	r12 = 1  #
	r1 = [sp+28]                    ; 4-byte Folded Reload
	[r1+r6<<2] = r0
	r0 = [sp+64]                    ; 4-byte Folded Reload
	r0 += 60
	r1 = r5 >>> 8
	[r0+r6<<2] = r1
	r0 = [r11+88]
	r1 = r0 * r9 (f)
	r5 = 0
	r7 = 1
	r0 = 0
	iff (r1 u<= r4) goto .LBB4_68
; BB#66:                                ;   in Loop: Header=BB4_15 Depth=1
	r2 = r4 + 0x80000000
	iff (r1 u>= r2) goto .LBB4_68
; BB#67:                                ;   in Loop: Header=BB4_15 Depth=1
	r0 = r1
	call lrintf
	r7 = r8
.LBB4_68:                               ;   in Loop: Header=BB4_15 Depth=1
	r1 = r0 << 24
	r2 = [sp+24]                    ; 4-byte Folded Reload
	[r2+r6<<2] = r1
	r0 = r0 >>> 8
	r1 = [sp+20]                    ; 4-byte Folded Reload
	[r1+r6<<2] = r0
	r0 = [r11+92]
	r0 = r0 * r9 (f)
	iff (r0 u<= r4) goto .LBB4_71
; BB#69:                                ;   in Loop: Header=BB4_15 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_71
; BB#70:                                ;   in Loop: Header=BB4_15 Depth=1
	call lrintf
	r5 = r0
	r12 = r7
.LBB4_71:                               ;   in Loop: Header=BB4_15 Depth=1
	r0 = r5 << 24
	r1 = [sp+16]                    ; 4-byte Folded Reload
	[r1+r6<<2] = r0
	r0 = r5 >>> 8
	r1 = [sp+12]                    ; 4-byte Folded Reload
	[r1+r6<<2] = r0
	r2 = [sp+68]                    ; 4-byte Folded Reload
	r2 += 1
	r0 = r6 + 17
	r1 = [sp+80]                    ; 4-byte Folded Reload
	r1 += 18
	if (r2 != 23) goto .LBB4_15
; BB#72:
	r0 = 27525
	r7 = r6 + r0  #
	r0 = [sp+120]                   ; 4-byte Folded Reload
	r13 = r7 + 2
	r5 = r7 + 1
	r14 = r0 + 7308
	r0 = r14 + 64
	r0 = r14 + 128  #
	[sp+104] = r0                   ; 4-byte Folded Spill
	r15 = 0  #
	[sp+96] = r0                    ; 4-byte Folded Spill
.LBB4_73:                               ; =>This Inner Loop Header: Depth=1
	r0 = [r14+r15]
	r0 = r0 * r10 (f)
	r9 = 1
	r11 = 1
	r1 = 0
	iff (r0 u<= r4) goto .LBB4_76
; BB#74:                                ;   in Loop: Header=BB4_73 Depth=1
	r2 = r4 + 0x80000000
	iff (r0 u>= r2) goto .LBB4_76
; BB#75:                                ;   in Loop: Header=BB4_73 Depth=1
	call lrintf
	r1 = r0
	r11 = r12
.LBB4_76:                               ;   in Loop: Header=BB4_73 Depth=1
	r8 = 0  #
	r0 = [sp+120]                   ; 4-byte Folded Reload
	[r0+r7<<2] = r1
	r0 = [sp+104]                   ; 4-byte Folded Reload
	r0 = [r0+r15]
	r1 = r0 * r10 (f)
	r0 = 0
	iff (r1 u<= r4) goto .LBB4_79
; BB#77:                                ;   in Loop: Header=BB4_73 Depth=1
	r2 = r4 + 0x80000000
	iff (r1 u>= r2) goto .LBB4_79
; BB#78:                                ;   in Loop: Header=BB4_73 Depth=1
	r0 = r1
	call lrintf
	r9 = r11
.LBB4_79:                               ;   in Loop: Header=BB4_73 Depth=1
	r12 = 1  #
	r1 = [sp+120]                   ; 4-byte Folded Reload
	[r1+r5<<2] = r0
	r0 = [sp+96]                    ; 4-byte Folded Reload
	r0 = [r0+r15]
	r0 = r0 * r10 (f)
	iff (r0 u<= r4) goto .LBB4_82
; BB#80:                                ;   in Loop: Header=BB4_73 Depth=1
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_82
; BB#81:                                ;   in Loop: Header=BB4_73 Depth=1
	call lrintf
	r8 = r0
	r12 = r9
.LBB4_82:                               ;   in Loop: Header=BB4_73 Depth=1
	r0 = [sp+120]                   ; 4-byte Folded Reload
	[r0+r13<<2] = r8
	r5 += 3
	r7 += 3
	r15 = r15 + 4
	r13 = r13 + 3
	if (r15 != 64) goto .LBB4_73
; BB#83:
	r0 = 1875  #
	r1 = [sp+120]                   ; 4-byte Folded Reload
	r0 = [r1+r0<<2]
	r1 = 0x4A000000
	r0 = r0 * r1 (f)
	iff (r0 u<= r4) goto .LBB4_87
; BB#84:
	r1 = r4 + 0x80000000
	iff (r0 u>= r1) goto .LBB4_87
; BB#85:
	r1 = [sp+112]                   ; 4-byte Folded Reload
	r4 = r1 + 260
	call lrintf
	r1 = r6 + 66
	[r4+r6<<2] = r0
	if (r1 > 1960) goto .LBB4_88
; BB#86:
	if (r12 == 0) {
		r0 = 1
		r1 = [sp+64]            ; 4-byte Folded Reload
		b[r1+2] = r0
	}
	goto .LBB4_88
.LBB4_87:
	r0 = [sp+112]                   ; 4-byte Folded Reload
	r0 = r0 + 260
	r1 = 0
	[r0+r6<<2] = r1
.LBB4_88:
	r5 = 119344
	r8 = 0  #
	r0 = [sp+120]                   ; 4-byte Folded Reload
	b[++r5=r0] = r8
	r0 = [sp+64]                    ; 4-byte Folded Reload
	r0 = b[r0+2] (u)
	if (r0 == 0) goto .LBB4_96
; BB#89:
	r7 = 29368320
	[r7+0] = 0x10000
	[r7+4] = 0x20000
	[r7+8] = 0x30000
	[r7+12] = 0x40000
	[r7+16] = 0x50000
	[r7+20] = 0x60000
	[r7+24] = 0x70000
	[r7+28] = 0x80000
	[r7+32] = 0x90000
	[r7+36] = 0xA0000
	[r7+40] = 0xB0000
	[r7+44] = 0xC0000
	r6 = r7 + 0x2000
	[r6+0] = 0x100000
	[r6+4] = 0x200000
	[r6+8] = 0x300000
	r9 = 2146360236
	r4 = r6 + 6144
	[r4+0] = r9
	[r4+4] = r9
	[r4+8] = r9
	[r4+12] = r9
	[r4+16] = r9
	[r4+20] = r9
	[r4+24] = r9
	[r4+28] = r9
	r0 = r8 + 2721
	r1 = r7
	d[sp+124] = r1_r0
	r0 = r6
	r1 = r4
	d[sp+132] = r1_r0
	r0 = 402669571
	r1 = 4194316
	d[sp+140] = r1_r0
	r0 = sp + 124
	call pa_hw_accel_submit_lean
	call pa_hw_accel_wait_lean
	call pa_hw_accel_broken
	if (r0 != 0) goto .LBB4_95
; BB#90:
	r10 = r8 + 0x800000  #
	r0 = [r7+0] 
	r11 = 0  #
	r2 = [r6+0] 
	d[sp+96] = r11_r10              ; 8-byte Folded Spill
	r11_r10 += r2 * r0 (s)  #
	r1 = [r7+4] 
	r12 = [r6+4]
	r8 = [r7+8]
	r14 = [r6+8]
	d[sp+104] = r3_r2               ; 8-byte Folded Spill
	r11_r10 += r12 * r1 (s)  #
	r0 = [r4+0] 
	d[sp+112] = r13_r12             ; 8-byte Folded Spill
	r13_r12 = r15_r14
	r11_r10 += r12 * r8 (s)
	r8 = 0
	r11_r10 >>= 24
	if (r0 != r10) goto .LBB4_95
; BB#91:
	r1_r0 = d[r7+12]
	r15_r14 = d[sp+112]             ; 8-byte Folded Reload
	r15 = r14 >>> 31
	r3_r2 = d[sp+104]               ; 8-byte Folded Reload
	r3 = r2 >>> 31
	r11_r10 = d[sp+96]              ; 8-byte Folded Reload
	d[sp+104] = r3_r2               ; 8-byte Folded Spill
	r11_r10 += r0 * r2 (s)  #
	r0 = [r7+20] 
	d[sp+112] = r15_r14             ; 8-byte Folded Spill
	r11_r10 += r1 * r14 (s)  #
	r1 = [r4+4] 
	r13 = r12 >>> 31
	r11_r10 += r0 * r12 (s)
	r11_r10 >>= 24
	if (r1 != r10) goto .LBB4_95
; BB#92:
	r1_r0 = d[r7+24]
	r11_r10 = d[sp+96]              ; 8-byte Folded Reload
	r15_r14 = d[sp+104]             ; 8-byte Folded Reload
	r11_r10 += r0 * r14 (s)  #
	r2 = [r7+32] 
	r15_r14 = r13_r12  #
	r0 = [r4+8] 
	r13_r12 = d[sp+112]             ; 8-byte Folded Reload
	r11_r10 += r1 * r12 (s)
	r13_r12 = r15_r14
	r11_r10 += r2 * r12 (s)
	r11_r10 >>= 24
	if (r0 != r10) goto .LBB4_95
; BB#93:
	r1_r0 = d[r7+36]
	r2 = [r7+44] 
	r7_r6 = d[sp+96]                ; 8-byte Folded Reload
	r11_r10 = d[sp+104]             ; 8-byte Folded Reload
	r7_r6 += r0 * r10 (s)  #
	r0 = [r4+12] 
	r11_r10 = d[sp+112]             ; 8-byte Folded Reload
	r7_r6 += r1 * r10 (s)
	r7_r6 += r2 * r12 (s)
	r7_r6 >>= 24
	if (r0 != r6) goto .LBB4_95
; BB#94:
	r0 = [r4+16] 
	r1 = 1
	if (r0 != r9) {
		r1 = 0
	}
	b[r5+0] = r1
	if (r0 != r9) goto .LBB4_96
	goto .LBB4_97
.LBB4_95:
	r0 = 0
	b[r5+0] = r0
.LBB4_96:
	r0 = [sp+64]                    ; 4-byte Folded Reload
	b[r0+2] = r8
.LBB4_97:
	r12 = wn_asm_mix
	r1 = r12
	;APP
	;NO_APP
	r0 = 0
	if (r1 == 0) goto .LBB4_109
; BB#98:
	r10 = wn_asm_head
	r1 = r10
	;APP
	;NO_APP
	if (r1 == 0) goto .LBB4_109
; BB#99:
	r5 = 625341585
	r1 = 1664525
	r2 = 1013904223
	r3 = 0x5A5A5A5A
.LBB4_100:                              ; =>This Inner Loop Header: Depth=1
	r4 = r5 * r1
	r4 += r2
	r5 = r4 >> 8
	r5 = r5 << 1
	r5 = r5 + 0xFF000000
	r6 = sp + 1144
	[r6+r0<<2] = r5
	r4 *= r1
	r4 += r2
	r5 = r4 >> 8
	r5 = r5 << 6
	r5 = r5 + 0xE0000000
	r6 = sp + 880
	[r6+r0<<2] = r5
	r6 = sp + 1012
	[r6+r0<<2] = r5
	r4 *= r1
	r5 = r4 + r2
	r4 = r5 >> 8
	r4 = r4 << 5
	r4 = r4 + 0xF0000000
	r6 = sp + 736
	[r6+r0<<2] = r4
	r4 = sp + 472
	[r4+r0<<2] = r3
	r4 = sp + 604
	[r4+r0<<2] = r3
	r0 += 1
	if (r0 != 33) goto .LBB4_100
; BB#101:
	r0 = 2147483000
	[sp+880] = r0
	[sp+1012] = r0
	r0 = 0x400000
	[sp+1144] = r0
	r0 = -2147483000
	[sp+896] = r0
	[sp+1028] = r0
	r0 = -8388608
	[sp+1160] = r0
	r0 = -2446342
	[sp+436] = r0
	r0 = 10952678
	[sp+440] = r0
	r0 = -12733598
	[sp+444] = r0
	r0 = 13156198
	[sp+448] = r0
	r0 = -1027406
	[sp+452] = r0
	r0 = 2536046
	[sp+456] = r0
	r0 = 7120320
	[sp+460] = r0
	r0 = -2604846
	[sp+464] = r0
	r0 = 15177766
	[sp+468] = r0
	r0 = r8 + 0x28000000
	r1 = 199614
	d[sp+412] = r1_r0
	r0 = r0 + 0x10000000
	r1 = 87170
	d[sp+420] = r1_r0
	r0 = r0 + 0xA0000000
	r1 = 4294882315
	d[sp+428] = r1_r0
	r11 = 11
	[sp+8] = r11
	r0 = sp + 604
	[sp+4] = r0
	r4 = sp + 736
	r6 = sp + 1144
	r9 = sp + 1012
	r7 = sp + 436
	r5 = sp + 412
	r0 = r6  #
	[sp] = r4
	r1 = r9
	r2 = r7
	r3 = r5
	call wn_post_mix_c
	;APP
	;NO_APP
	[sp+8] = r11
	r0 = sp + 472
	r0 = r6  #
	[sp+4] = r0
	r1 = sp + 880
	r2 = r7  #
	[sp] = r4
	r3 = r5
	call r12
	r1_r0 = 0
.LBB4_102:                              ; =>This Inner Loop Header: Depth=1
	r2 = sp + 880
	r2 = [r2+r1<<2]
	r3 = sp + 1012
	r3 = [r3+r1<<2]
	if (r3 != r2) goto .LBB4_109
; BB#103:                               ;   in Loop: Header=BB4_102 Depth=1
	r2 = sp + 472
	r2 = [r2+r1<<2]
	r3 = sp + 604
	r3 = [r3+r1<<2]
	if (r3 != r2) goto .LBB4_109
; BB#104:                               ;   in Loop: Header=BB4_102 Depth=1
	r1 += 1
	ifs (r1 <= 32) goto .LBB4_102
; BB#105:
	r1_r0 = d[sp+1012]
	[sp+148] = r0
	[sp+280] = r0
	r0 = [sp+1020]
	[sp+152] = r1
	[sp+284] = r1
	r1 = [sp+1024]
	[sp+156] = r0
	[sp+288] = r0
	r0 = [r9+16]
	[sp+160] = r1
	[sp+292] = r1
	r1 = [sp+1032]
	[sp+164] = r0
	[sp+296] = r0
	r0 = [sp+1036]
	[sp+168] = r1
	[sp+300] = r1
	r1 = [sp+1040]
	[sp+172] = r0
	[sp+304] = r0
	r0 = [sp+1044]
	[sp+176] = r1
	[sp+308] = r1
	r1 = [sp+1048]
	[sp+180] = r0
	[sp+312] = r0
	r0 = [sp+1052]
	[sp+184] = r1
	[sp+316] = r1
	r1 = [sp+1056]
	[sp+188] = r0
	[sp+320] = r0
	r0 = [sp+1060]
	[sp+192] = r1
	[sp+324] = r1
	r1 = [sp+1064]
	[sp+196] = r0
	[sp+328] = r0
	r0 = [sp+1068]
	[sp+200] = r1
	[sp+332] = r1
	r1 = [sp+1072]
	[sp+204] = r0
	[sp+336] = r0
	r0 = [sp+1076]
	[sp+208] = r1
	[sp+340] = r1
	r1 = [sp+1080]
	[sp+212] = r0
	[sp+344] = r0
	r0 = [sp+1084]
	[sp+216] = r1
	[sp+348] = r1
	r1 = [sp+1088]
	[sp+220] = r0
	[sp+352] = r0
	r0 = [sp+1092]
	[sp+224] = r1
	[sp+356] = r1
	r1 = [sp+1096]
	[sp+228] = r0
	[sp+360] = r0
	r0 = [sp+1100]
	[sp+232] = r1
	[sp+364] = r1
	r1 = [sp+1104]
	[sp+236] = r0
	[sp+368] = r0
	r0 = [sp+1108]
	[sp+240] = r1
	[sp+372] = r1
	r1 = [sp+1112]
	[sp+244] = r0
	[sp+376] = r0
	r0 = [sp+1116]
	[sp+248] = r1
	[sp+380] = r1
	r1 = [sp+1120]
	[sp+252] = r0
	[sp+384] = r0
	r0 = [sp+1124]
	[sp+256] = r1
	[sp+388] = r1
	r1 = [sp+1128]
	[sp+260] = r0
	[sp+392] = r0
	r0 = [sp+1132]
	[sp+264] = r1
	[sp+396] = r1
	r1 = [sp+1136]
	[sp+268] = r0
	r2 = [sp+1140]
	[sp+400] = r0
	[sp+272] = r1
	[sp+404] = r1
	[sp+276] = r2
	[sp+408] = r2
	r5 = sp + 1144
	r1 = sp + 280
	r2 = 11
	r0 = r5
	call wn_post_head_c
	;APP
	;NO_APP
	r1 = sp + 148
	r2 = 11
	r0 = r5
	call r10
	r1_r0 = 0
.LBB4_106:                              ; =>This Inner Loop Header: Depth=1
	r2 = sp + 148
	r2 = [r2+r1<<2]
	r3 = sp + 280
	r3 = [r3+r1<<2]
	if (r3 != r2) goto .LBB4_109
; BB#107:                               ;   in Loop: Header=BB4_106 Depth=1
	r1 += 1
	ifs (r1 <= 32) goto .LBB4_106
; BB#108:
	r0 = 1
.LBB4_109:
	r1 = .L_MergedGlobals
	b[r1+3] = r0
	r4 = [sp+120]                   ; 4-byte Folded Reload
	r0 = r4
	call pa_wavenet_reset
	r0 = 1
	b[r4+1] = r0
	b[r4+2] = r0
.LBB4_110:
	r0 = r8
	sp += 1276
	{pc, r15-r4} = [sp++]
.Lfunc_end4:
	.size	pa_wavenet_load_weights, .Lfunc_end4-pa_wavenet_load_weights

	.globl	pa_wavenet_set_input_trim
	.p2align	1
	.type	pa_wavenet_set_input_trim,@function
pa_wavenet_set_input_trim:              ; @pa_wavenet_set_input_trim
; BB#0:
	if (r0 == 0) goto .LBB5_2
; BB#1:
	r2 = 1008981770
	iff (r1 u>= r2) {
		r2 = r1
	}
	r1 = 1101004800
	iff (r2 u<= r1) {
		r1 = r2
	}
	[r0+12] = r1
.LBB5_2:
	rts
.Lfunc_end5:
	.size	pa_wavenet_set_input_trim, .Lfunc_end5-pa_wavenet_set_input_trim

	.globl	pa_wavenet_get_input_trim
	.p2align	1
	.type	pa_wavenet_get_input_trim,@function
pa_wavenet_get_input_trim:              ; @pa_wavenet_get_input_trim
; BB#0:
	r1 = r0
	r0 = 0x3F800000
	if (r1 == 0) goto .LBB6_2
; BB#1:
	r1 = [r1+12] 
	r2 = 981668463
	iff (r1 > r2) {
		r0 = r1
	}
.LBB6_2:
	rts
.Lfunc_end6:
	.size	pa_wavenet_get_input_trim, .Lfunc_end6-pa_wavenet_get_input_trim

	.globl	pa_wavenet_step_sample
	.p2align	1
	.type	pa_wavenet_step_sample,@function
pa_wavenet_step_sample:                 ; @pa_wavenet_step_sample
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -124
	r10 = r0
	if (r0 == 0) goto .LBB7_33
; BB#1:
	r0 = b[r0+2] (u)
	if (r0 == 0) goto .LBB7_33
; BB#2:
	r7 = b[r10+0] (u)
	if (r7 == 8) goto .LBB7_4
; BB#3:
	if (r7 != 3) goto .LBB7_33
.LBB7_4:
	r2 = [r10+12]
	r0 = r2 * r1 (f)
	r3 = 981668463
	iff (r2 u<= r3) {
		r0 = r1
	}
	r1 = 0x40800000
	iff (r0 u<= r1) {
		r1 = r0
	}
	r13 = -1065353216
	iff (r1 u>= r13) {
		r13 = r1
	}
	r0 = r10 + 24
	r1 = sp + 112
	r2 = r7
.LBB7_5:                                ; =>This Inner Loop Header: Depth=1
	r3 = [r0++=4] 
	r3 = r13 * r3 (f)
	[r1++=4] = r3
	if (--r2 != 0) goto .LBB7_5
; BB#6:
	r12 = r7 << 2
	r0 = sp + 100
	r1 = 0
	r4 = 0
	r2 = r12
	call memset
	r11 = r10 + 24
	r0 = r10 + 7508
	r0 = r7 * r7  #
	[sp+72] = r0                    ; 4-byte Folded Spill
	r6 = r7  #
	[sp] = r0                       ; 4-byte Folded Spill
	r5 = 54988
	[sp+68] = r10                   ; 4-byte Folded Spill
	[sp+12] = r11                   ; 4-byte Folded Spill
	[sp+32] = r13                   ; 4-byte Folded Spill
	[sp+4] = r12                    ; 4-byte Folded Spill
.LBB7_7:                                ; =>This Loop Header: Depth=1
                                        ;     Child Loop BB7_8 Depth 2
                                        ;     Child Loop BB7_10 Depth 2
                                        ;       Child Loop BB7_11 Depth 3
                                        ;         Child Loop BB7_12 Depth 4
                                        ;         Child Loop BB7_14 Depth 4
                                        ;     Child Loop BB7_18 Depth 2
                                        ;       Child Loop BB7_19 Depth 3
                                        ;     Child Loop BB7_22 Depth 2
	r0 = r4 << 3
	r1 = g_pa_wn_specs
	r2 = r0 + r1  #
	[sp+28] = r4                    ; 4-byte Folded Spill
	r0 = r4 << 1
	r4 = r10 + r0  #
	r3 = h[r2+4] (u)
	r0 = h[r4+r5<<1] (u)
	r9 = r6
	r1 = r0 + 1
	r0 = 0
	if (r1 < r3) {
		r0 = r1
	}
	r8 = h[r2+6] (u)
	h[r4+r5<<1] = r0
	r4 = r0 * r7  #
	r1 = h[r2+2] (u)
	r5 = r8 * r7  #
	[sp+56] = r1                    ; 4-byte Folded Spill
	r4 += r5  #
	r2 = b[r2+0] (u)
	r4 = r4 << 2
	r6 = r7  #
	r1 = [sp+72]                    ; 4-byte Folded Reload
	r4 += r1
	r5 = sp + 112
.LBB7_8:                                ;   Parent Loop BB7_7 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	r1 = [r5++=4] 
	[r4++=4] = r1
	if (--r6 != 0) goto .LBB7_8
; BB#9:                                 ;   in Loop: Header=BB7_7 Depth=1
	r1 = r0 + r8  #
	r5 = [sp+56]                    ; 4-byte Folded Reload
	r4 = r2 + -1
	r4 *= r5
	r1 = r1 - r4
	r1 = r1 - r3
	r1 *= r7
	r1 = r7 * r2  #
	[sp+52] = r1                    ; 4-byte Folded Spill
	r15 = r7 * r3
	r12 = r7 * r5
	r5 = r9 << 2
	r5 += r11
	r0 = r0 - r4
	r0 = r0 - r3
	d[sp+44] = r1_r0                ; 4-byte Folded Spill
	r4 = [sp]                       ; 4-byte Folded Reload
	r0 = r2 * r4
	[sp+8] = r9                     ; 4-byte Folded Spill
	r0 += r9
	r0 += r7  #
	[sp+40] = r0                    ; 4-byte Folded Spill
	r0 += r7  #
	[sp+36] = r0                    ; 4-byte Folded Spill
	r0 += r4  #
	[sp+16] = r0                    ; 4-byte Folded Spill
	r0 += r7  #
	[sp+20] = r0                    ; 4-byte Folded Spill
	[sp+24] = r0                    ; 4-byte Folded Spill
	r1 = 0
.LBB7_10:                               ;   Parent Loop BB7_7 Depth=1
                                        ; =>  This Loop Header: Depth=2
                                        ;       Child Loop BB7_11 Depth 3
                                        ;         Child Loop BB7_12 Depth 4
                                        ;         Child Loop BB7_14 Depth 4
	r0 = [sp+40]                    ; 4-byte Folded Reload
	r0 += r1  #
	[sp+60] = r1                    ; 4-byte Folded Spill
	r0 = r0 << 2
	r0 += r10
	r11 = [r0+24]
	[sp+64] = r5                    ; 4-byte Folded Spill
	r13 = [sp+44]                   ; 4-byte Folded Reload
	r8 = [sp+52]                    ; 4-byte Folded Reload
	r0 = 0
	r1 = [sp+56]                    ; 4-byte Folded Reload
.LBB7_11:                               ;   Parent Loop BB7_7 Depth=1
                                        ;     Parent Loop BB7_10 Depth=2
                                        ; =>    This Loop Header: Depth=3
                                        ;         Child Loop BB7_12 Depth 4
                                        ;         Child Loop BB7_14 Depth 4
	r6 = r13
	r4 = r8
.LBB7_12:                               ;   Parent Loop BB7_7 Depth=1
                                        ;     Parent Loop BB7_10 Depth=2
                                        ;       Parent Loop BB7_11 Depth=3
                                        ; =>      This Inner Loop Header: Depth=4
	r6 += r3
	r4 += r15
	ifs (r6 < 0) goto .LBB7_12
; BB#13:                                ;   in Loop: Header=BB7_11 Depth=3
	r4 = r4 << 2
	r9 = 0  #
	r6 = [sp+72]                    ; 4-byte Folded Reload
	r4 += r6
	r14 = r5
.LBB7_14:                               ;   Parent Loop BB7_7 Depth=1
                                        ;     Parent Loop BB7_10 Depth=2
                                        ;       Parent Loop BB7_11 Depth=3
                                        ; =>      This Inner Loop Header: Depth=4
	r6 = r2 << 2
	r10 = [r4+r9<<2]
	r6 = [r14++=r6]
	r6 = r6 * r10 (f)
	r9 = r9 + 1
	r11 = r11 + r6 (f)
	if (r7 != r9) goto .LBB7_14
; BB#15:                                ;   in Loop: Header=BB7_11 Depth=3
	r13 += r1
	r8 += r12
	r0 += 1
	r5 += 4
	ifs (r0 < r2) goto .LBB7_11
; BB#16:                                ;   in Loop: Header=BB7_10 Depth=2
	r0 = [sp+36]                    ; 4-byte Folded Reload
	r1 = [sp+60]                    ; 4-byte Folded Reload
	r0 += r1
	r0 = r0 << 2
	r10 = [sp+68]                   ; 4-byte Folded Reload
	r0 += r10
	r0 = [r0+24] 
	r13 = [sp+32]                   ; 4-byte Folded Reload
	r0 = r13 * r0 (f)
	r0 = r11 + r0 (f)
	r4 = 1008981770
	r4 = r0 * r4 (f)
	r5 = 0
	iff (r0 u< r5) {
		r0 = r4
	}
	r4 = sp + 100
	r5 = [r4+r1<<2]
	r6 = sp + 88
	[r6+r1<<2] = r0
	r0 = r5 + r0 (f)
	[r4+r1<<2] = r0
	r0 = [sp+48]                    ; 4-byte Folded Reload
	r0 = r0 << 2
	r1 += 1
	r5 = [sp+64]                    ; 4-byte Folded Reload
	r5 += r0
	if (r1 != r7) goto .LBB7_10
; BB#17:                                ;   in Loop: Header=BB7_7 Depth=1
	r9 = 0  #
	r0 = [sp+16]                    ; 4-byte Folded Reload
	r0 = r0 << 2
	r11 = [sp+12]                   ; 4-byte Folded Reload
	r8 = r11 + r0
	r12 = [sp+4]                    ; 4-byte Folded Reload
	r14 = [sp+20]                   ; 4-byte Folded Reload
.LBB7_18:                               ;   Parent Loop BB7_7 Depth=1
                                        ; =>  This Loop Header: Depth=2
                                        ;       Child Loop BB7_19 Depth 3
	r0 = r9 + r14
	r0 = r0 << 2
	r0 += r10
	r6 = r7  #
	r3 = [r0+24] 
	r4 = sp + 88
	r0 = r8
.LBB7_19:                               ;   Parent Loop BB7_7 Depth=1
                                        ;     Parent Loop BB7_18 Depth=2
                                        ; =>    This Inner Loop Header: Depth=3
	r5 = [r4++=4] 
	r1 = [r0++=4] 
	r1 = r1 * r5 (f)
	r3 = r3 + r1 (f)
	if (--r6 != 0) goto .LBB7_19
; BB#20:                                ;   in Loop: Header=BB7_18 Depth=2
	r0 = sp + 112
	r0 = [r0+r9<<2]
	r0 = r3 + r0 (f)
	r3 = sp + 76
	[r3+r9<<2] = r0
	r9 = r9 + 1
	r8 += r12
	if (r9 != r7) goto .LBB7_18
; BB#21:                                ;   in Loop: Header=BB7_7 Depth=1
	r0 = sp + 76
	r1 = sp + 112
	r3 = r7
.LBB7_22:                               ;   Parent Loop BB7_7 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	r4 = [r0++=4] 
	[r1++=4] = r4
	if (--r3 != 0) goto .LBB7_22
; BB#23:                                ;   in Loop: Header=BB7_7 Depth=1
	r4 = [sp+28]                    ; 4-byte Folded Reload
	r4 += 1
	r5 = 54988
	r6 = [sp+24]                    ; 4-byte Folded Reload
	if (r4 != 23) goto .LBB7_7
; BB#24:
	r15 = 119588
	r0 = h[++r15=r10] (u)
	r0 += 1
	r3 = 0
	if (r0 <= 15) {
		r3 = r0
	}
	h[r15+0] = r3
	r0 = r7 << 4
	r0 += r6
	r1 = r3 * r7
	r1 = r1 << 2
	r1 += r10
	r9 = 119396
	r1 += r9
	r4 = sp + 100
	r6 = r7
.LBB7_25:                               ; =>This Inner Loop Header: Depth=1
	r5 = [r4++=4] 
	[r1++=4] = r5
	if (--r6 != 0) goto .LBB7_25
; BB#26:
	r0 = [r11+r0<<2]
	r1 = r2 + 1
	r1 *= r7  #
	r2 = [sp+8]                     ; 4-byte Folded Reload
	r1 += 3
	r1 *= r7
	r1 += r2
	r1 = r1 << 2
	r12 = r11 + r1
	r8 = 0x1E - r3
	r11 = r3 + -15
	r10 = 0
	r13 = 1073741808
	r14 = 29849
.LBB7_27:                               ; =>This Loop Header: Depth=1
                                        ;     Child Loop BB7_28 Depth 2
	r1 = r12
	r2 = r8
	r3 = r11
	r4 = 0
.LBB7_28:                               ;   Parent Loop BB7_27 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	r5 = r3
	ifs (r3 <= 0) {
		r5 = 0
	}
	r5 += r2  #
	r6 = [sp+68]                    ; 4-byte Folded Reload
	r5 = r5 & r13
	r5 += r3
	r5 *= r7
	r5 += r10
	r5 = r5 << 2
	r5 += r6  #
	r6 = [r1++=4] 
	r5 = [r5+r14<<2]
	r5 = r6 * r5 (f)
	r0 = r0 + r5 (f)
	r2 += -1
	r4 += 1
	r3 += 1
	if (r4 != 16) goto .LBB7_28
; BB#29:                                ;   in Loop: Header=BB7_27 Depth=1
	r10 = r10 + 1
	r12 = r12 + 64
	if (r10 != r7) goto .LBB7_27
; BB#30:
	r5 = [sp+68]                    ; 4-byte Folded Reload
	r1 = [r5+8] 
	r6 = r0 * r1 (f)
	r0 = r6
	call __fpclassifyf
	if (r0 > 1) goto .LBB7_32
; BB#31:
	r2 = 76248
	r1 = 0
	r4 = 0
	r0 = [sp+72]                    ; 4-byte Folded Reload
	call memset
	r0 = 109976
	r0 += r5
	r1 = 0
	r2 = 46
	call memset
	r0 = r5 + r9
	r1 = 0
	r2 = 192
	call memset
	h[r15+0] = r4
	[r15+8] = r4
	[r15+4] = r4
	[r15+24] = r4
	[r15+20] = r4
	[r15+16] = r4
	[r15+12] = r4
	[r15+40] = r4
	[r15+36] = r4
	[r15+32] = r4
	[r15+28] = r4
	[r15+48] = r4
	[r15+44] = r4
	[r15+52] = r4
	r6 = 0
.LBB7_32:
	r0 = r13 + 16
	iff (r6 u<= r0) {
		r0 = r6
	}
	r1 = 0xC0000000
	iff (r0 u>= r1) {
		r1 = r0
	}
.LBB7_33:
	r0 = r1
	sp += 124
	{pc, r15-r4} = [sp++]
.Lfunc_end7:
	.size	pa_wavenet_step_sample, .Lfunc_end7-pa_wavenet_step_sample

	.globl	pa_wavenet_set_voicing
	.p2align	1
	.type	pa_wavenet_set_voicing,@function
pa_wavenet_set_voicing:                 ; @pa_wavenet_set_voicing
; BB#0:
	if (r0 == 0) goto .LBB8_2
; BB#1:
	r2 = 0
	ifs (r1 <= 4) {
		r2 = r1
	}
	b[r0+4] = r2
.LBB8_2:
	rts
.Lfunc_end8:
	.size	pa_wavenet_set_voicing, .Lfunc_end8-pa_wavenet_set_voicing

	.globl	pa_wavenet_get_voicing_name
	.p2align	1
	.type	pa_wavenet_get_voicing_name,@function
pa_wavenet_get_voicing_name:            ; @pa_wavenet_get_voicing_name
; BB#0:
	ifs (r0 <= 4) {
		r1 = k_voice_names
		r0 = [r1+r0<<2]
	} else {
		r0 = .L.str
	}
	rts
.Lfunc_end9:
	.size	pa_wavenet_get_voicing_name, .Lfunc_end9-pa_wavenet_get_voicing_name

	.globl	pa_wavenet_block_ready
	.p2align	1
	.type	pa_wavenet_block_ready,@function
pa_wavenet_block_ready:                 ; @pa_wavenet_block_ready
; BB#0:
	[--sp] = rets
	r1 = r0
	r0 = 0
	if (r1 == 0) goto .LBB10_5
; BB#1:
	r2 = 110022
	r2 = b[r1+r2] (u)
	if (r2 == 0) goto .LBB10_5
; BB#2:
	r2 = 110023
	r1 = b[r1+r2] (u)
	if (r1 != 0) goto .LBB10_5
; BB#3:
	call pa_hw_accel_broken
	r1 = r0
	r0 = 1
	if (r1 == 0) goto .LBB10_5
; BB#4:
	r0 = 0
.LBB10_5:
	pc = [sp++]
.Lfunc_end10:
	.size	pa_wavenet_block_ready, .Lfunc_end10-pa_wavenet_block_ready

	.globl	pa_wavenet_process_block_float
	.p2align	1
	.type	pa_wavenet_process_block_float,@function
pa_wavenet_process_block_float:         ; @pa_wavenet_process_block_float
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -24
	r4 = r2
	r14 = r1
	r9 = r0
	if (r0 == 0) goto .LBB11_34
; BB#1:
	if (r1 == 0) goto .LBB11_34
; BB#2:
	call plat_ticks24
	r2 = [r9+20]
	r1 = 1028443341
	[sp] = r0                       ; 4-byte Folded Spill
	[sp+20] = r2                    ; 4-byte Folded Spill
	iff (r2 > r1) goto .LBB11_4
; BB#3:
	r0 = b[r9+3] (u)
	if (r0 == 0) {
		r0 = 0x3F800000
	} else {
		r0 = 1068708659
	}
	[sp+20] = r0                    ; 4-byte Folded Spill
.LBB11_4:
	r0 = 119396
	r0 += r9
	[sp+16] = r0                    ; 4-byte Folded Spill
	r0 = 119588
	r7 = r9 + r0  #
	[sp+4] = r4                     ; 4-byte Folded Spill
	ifs (r4 < 1) goto .LBB11_31
; BB#5:
	r0 = 109976
	r0 += r9
	r0 = r9 + 7508  #
	[sp+12] = r0                    ; 4-byte Folded Spill
	r11 = 0  #
	[sp+8] = r0                     ; 4-byte Folded Spill
	r15 = [sp+4]                    ; 4-byte Folded Reload
.LBB11_6:                               ; =>This Inner Loop Header: Depth=1
	r0 = [r14+0]
	r1 = [r7+8] 
	r2 = [r7+4] 
	r3 = 1065317984
	r1 = r1 * r3 (f)  #
	[r7+4] = r0
	r0 = r0 - r2 (f)
	r3 = r0 + r1 (f)
	[r7+8] = r3
	r1 = [r7+24] 
	r10 = [r7+12]
	r0 = b[r9+4] (u)
	r2 = [r7+20] 
	r4 = [r7+16] 
	[r7+16] = r10
	[r7+12] = r3
	[r7+24] = r2
	if (r0 >= 5) {
		r0 = 0
	}
	r0 = r0 * 0x14
	r12 = k_hpf_profiles
	r13 = [++r12=r0]
	r8 = [r12+4]
	r3 = r3 * r13 (f)
	r6 = [r12+8]
	r5 = r8 * r10 (f)
	r3 = r3 + r5 (f)
	r5 = [r12+12]
	r4 = r6 * r4 (f)
	r6 = [r12+16]
	r3 = r3 + r4 (f)
	r2 = r5 * r2 (f)
	r2 = r3 - r2 (f)
	r1 = r6 * r1 (f)
	r1 = r2 - r1 (f)
	[r7+20] = r1
	r2 = [r7+28] 
	r8 = [r7+40]
	r4 = [r7+36] 
	r5 = [r7+32] 
	[r7+32] = r2
	[r7+28] = r1
	[r7+40] = r4
	r3 = k_lpf_profiles
	r6 = [++r0=r3]
	r1 = r1 * r6 (f)  #
	r3 = [r0+4] 
	r2 = r3 * r2 (f)  #
	r6 = [r0+8] 
	r1 = r1 + r2 (f)  #
	r2 = [r0+12] 
	r3 = r6 * r5 (f)  #
	r0 = [r0+16] 
	r1 = r1 + r3 (f)
	r2 = r2 * r4 (f)
	r1 = r1 - r2 (f)
	r0 = r0 * r8 (f)
	r1 = r1 - r0 (f)
	r0 = r9  #
	[r7+36] = r1
	call pa_wavenet_step_sample
	r1 = [r7+44] 
	r2 = [r7+48] 
	r0 = r0 - r1 (f)  #
	[r7+44] = r0
	r1 = 1065341472
	r1 = r2 * r1 (f)
	r0 = r0 + r1 (f)
	[r7+48] = r0
	r1 = b[r9+4] (u)
	r2 = [sp+20]                    ; 4-byte Folded Reload
	r4 = r2 * r0 (f)
	if (r1 == 0) goto .LBB11_17
; BB#7:                                 ;   in Loop: Header=BB11_6 Depth=1
	r0 = 1067030938
	iff (r4 u<= r0) goto .LBB11_12
; BB#8:                                 ;   in Loop: Header=BB11_6 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r11 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB11_11
; BB#9:                                 ;   in Loop: Header=BB11_6 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB11_11
; BB#10:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB11_11:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = 1041865114
	r0 = r1 * r0 (f)
	r1 = 1067030938
	r4 = r0 + r1 (f)
.LBB11_12:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = -1080452710
	iff (r4 u>= r0) goto .LBB11_28
; BB#13:                                ;   in Loop: Header=BB11_6 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r11 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB11_16
; BB#14:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB11_16
; BB#15:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB11_16:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = 1041865114
	r0 = r1 * r0 (f)
	r1 = -1080452710
	goto .LBB11_27
.LBB11_17:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = 1068708659
	iff (r4 u<= r0) goto .LBB11_22
; BB#18:                                ;   in Loop: Header=BB11_6 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r11 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB11_21
; BB#19:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB11_21
; BB#20:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB11_21:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = 1036831949
	r0 = r1 * r0 (f)
	r1 = 1068708659
	r4 = r0 + r1 (f)
.LBB11_22:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = -1078774989
	iff (r4 u>= r0) goto .LBB11_28
; BB#23:                                ;   in Loop: Header=BB11_6 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r11 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB11_26
; BB#24:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB11_26
; BB#25:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB11_26:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = 1036831949
	r0 = r1 * r0 (f)
	r1 = -1078774989
.LBB11_27:                              ;   in Loop: Header=BB11_6 Depth=1
	r4 = r0 + r1 (f)
.LBB11_28:                              ;   in Loop: Header=BB11_6 Depth=1
	r0 = r4
	call __fpclassifyf
	if (r0 > 1) goto .LBB11_30
; BB#29:                                ;   in Loop: Header=BB11_6 Depth=1
	r1 = 0
	r0 = [sp+8]                     ; 4-byte Folded Reload
	r2 = 76248
	call memset
	r1 = 0
	r2 = 46
	r0 = [sp+12]                    ; 4-byte Folded Reload
	call memset
	r1 = 0
	r2 = 192
	r0 = [sp+16]                    ; 4-byte Folded Reload
	call memset
	h[r7+0] = r11
	[r7+8] = r11
	[r7+4] = r11
	[r7+24] = r11
	[r7+20] = r11
	[r7+16] = r11
	[r7+12] = r11
	[r7+40] = r11
	[r7+36] = r11
	[r7+32] = r11
	[r7+28] = r11
	[r7+48] = r11
	[r7+44] = r11
	[r7+52] = r11
	r4 = 0
.LBB11_30:                              ;   in Loop: Header=BB11_6 Depth=1
	[r14++=4] = r4
	if (--r15 != 0) goto .LBB11_6
.LBB11_31:
	call plat_ticks24
	r2 = [sp+4]                     ; 4-byte Folded Reload
	ifs (r2 < 1) goto .LBB11_34
; BB#32:
	r1 = [sp]                       ; 4-byte Folded Reload
	r0 = r0 - r1
	if (r0 == 0) goto .LBB11_34
; BB#33:
	r0 = r0 * 0xF
	r0 = r0 / r2 (u)  #
	[r7+56] = r0
	r1 = [r7+52] 
	if (r0 > r1) {
		[r7+52] = r0
	}
.LBB11_34:
	sp += 24
	{pc, r15-r4} = [sp++]
.Lfunc_end11:
	.size	pa_wavenet_process_block_float, .Lfunc_end11-pa_wavenet_process_block_float

	.globl	pa_wavenet_set_defer
	.p2align	1
	.type	pa_wavenet_set_defer,@function
pa_wavenet_set_defer:                   ; @pa_wavenet_set_defer
; BB#0:
	r2 = 1
	if (r0 != 0) goto .LBB12_2
; BB#1:
	r2 = r0
.LBB12_2:
	r1 = .L_MergedGlobals
	b[r1+0] = r2
	if (r0 != 0) goto .LBB12_4
; BB#3:
	r0 = 0
	b[r1+1] = r0
	b[r1+2] = r0
.LBB12_4:
	rts
.Lfunc_end12:
	.size	pa_wavenet_set_defer, .Lfunc_end12-pa_wavenet_set_defer

	.globl	pa_wavenet_pre_valid
	.p2align	1
	.type	pa_wavenet_pre_valid,@function
pa_wavenet_pre_valid:                   ; @pa_wavenet_pre_valid
; BB#0:
	r0 = .L_MergedGlobals
	r0 = b[r0+1] (u)
	rts
.Lfunc_end13:
	.size	pa_wavenet_pre_valid, .Lfunc_end13-pa_wavenet_pre_valid

	.p2align	1
	.type	wn_condition_block,@function
wn_condition_block:                     ; @wn_condition_block
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -56
	r7 = r0
	r4 = 119592
	r4 += r7
	r8 = [r4+36]
	r13 = [r4+32]
	r6 = [r4+28] 
	r10 = [r4+24]
	r15_r14 = d[r4+16]
	r5 = [r4+12] 
	[sp+52] = r5                    ; 4-byte Folded Spill
	r12 = [r4+8]
	r0 = [r4+4] 
	r7 = b[r7+4] (u)
	[sp] = r4                       ; 4-byte Folded Spill
	r4 = [r4+0] 
	if (r7 >= 5) {
		r7 = 0
	}
	if (r3 == 0) goto .LBB14_3
; BB#1:
	r5 = r7 * 0x14  #
	[sp+48] = r1                    ; 4-byte Folded Spill
	r7 = k_lpf_profiles
	r7 += r5
	r11 = r0  #
	r1 = [r7+16] 
	[sp+44] = r1                    ; 4-byte Folded Spill
	r1 = [r7+12] 
	[sp+40] = r1                    ; 4-byte Folded Spill
	r1 = [r7+8] 
	[sp+36] = r1                    ; 4-byte Folded Spill
	r1 = [r7+4] 
	[sp+32] = r1                    ; 4-byte Folded Spill
	r1 = [r7+0] 
	r7 = k_hpf_profiles
	r5 += r7  #
	[sp+28] = r1                    ; 4-byte Folded Spill
	r1 = [r5+16] 
	[sp+24] = r1                    ; 4-byte Folded Spill
	r1 = [r5+12] 
	[sp+20] = r1                    ; 4-byte Folded Spill
	r1 = [r5+8] 
	[sp+16] = r1                    ; 4-byte Folded Spill
	r1 = [r5+4] 
	[sp+12] = r1                    ; 4-byte Folded Spill
	r1 = [r5+0] 
	[sp+8] = r1                     ; 4-byte Folded Spill
	r1 = [sp+108]
	[sp+4] = r1                     ; 4-byte Folded Spill
.LBB14_2:                               ; =>This Inner Loop Header: Depth=1
	r1 = [sp+48]                    ; 4-byte Folded Reload
	r0 = [r1++=4] 
	[sp+48] = r1                    ; 4-byte Folded Spill
	r1 = 1065317984
	r5 = r11 * r1 (f)  #
	r7 = [sp+52]                    ; 4-byte Folded Reload
	r0 = itof(r0)
	r1 = 0x35000000
	r0 = r0 * r1 (f)
	r1 = r0
	r0 = r0 - r4 (f)
	r11 = r5 + r0 (f)  #
	r0 = [sp+8]                     ; 4-byte Folded Reload
	[sp+52] = r12                   ; 4-byte Folded Spill
	r0 = r0 * r11 (f)  #
	r4 = [sp+12]                    ; 4-byte Folded Reload
	r4 = r4 * r12 (f)
	r0 = r4 + r0 (f)  #
	r4 = [sp+16]                    ; 4-byte Folded Reload
	r4 = r4 * r7 (f)
	r0 = r4 + r0 (f)  #
	r4 = [sp+20]                    ; 4-byte Folded Reload
	r4 = r4 * r14 (f)
	r0 = r0 - r4 (f)  #
	r4 = [sp+24]                    ; 4-byte Folded Reload
	r4 = r4 * r15 (f)
	r7 = r0 - r4 (f)  #
	r0 = [sp+28]                    ; 4-byte Folded Reload
	r0 = r0 * r7 (f)  #
	r4 = [sp+32]                    ; 4-byte Folded Reload
	r4 = r4 * r10 (f)
	r0 = r4 + r0 (f)  #
	r4 = [sp+36]                    ; 4-byte Folded Reload
	r4 = r4 * r6 (f)
	r0 = r4 + r0 (f)  #
	r4 = [sp+40]                    ; 4-byte Folded Reload
	r4 = r4 * r13 (f)
	r0 = r0 - r4 (f)  #
	r4 = [sp+44]                    ; 4-byte Folded Reload
	r4 = r4 * r8 (f)
	r5 = r0 - r4 (f)  #
	r0 = [sp+4]                     ; 4-byte Folded Reload
	r0 = r5 * r0 (f)
	r4 = 0x40800000
	r6 = r4
	iff (r0 u<= r6) {
		r4 = r0
	}
	r0 = r6 + 0x80000000
	r6 = r0
	iff (r4 u>= r6) {
		r0 = r4
	}
	r15 = r14  #
	[r2++=4] = r0
	r9 = r7
	r6 = r10
	r8 = r13
	r0 = r11
	r12 = r11
	r13 = r5
	r10 = r7
	r14 = r7
	r4 = r1
	if (--r3 != 0) goto .LBB14_2
	goto .LBB14_4
.LBB14_3:
	r7 = r14
	r9 = r10
	r5 = r13
	r11 = r12
.LBB14_4:
	r2 = [sp]                       ; 4-byte Folded Reload
	[r2+0] = r4
	[r2+4] = r0
	[r2+8] = r11
	r1 = [sp+52]                    ; 4-byte Folded Reload
	[r2+12] = r1
	[r2+16] = r7
	[r2+20] = r15
	[r2+24] = r9
	[r2+28] = r6
	[r2+32] = r5
	[r2+36] = r8
	sp += 56
	{pc, r15-r4} = [sp++]
.Lfunc_end14:
	.size	wn_condition_block, .Lfunc_end14-wn_condition_block

	.globl	pa_wavenet_pre_core0
	.p2align	1
	.type	pa_wavenet_pre_core0,@function
pa_wavenet_pre_core0:                   ; @pa_wavenet_pre_core0
; BB#0:
	[--sp] = {rets, r10-r4}
	sp += -4
	r8 = .L_MergedGlobals
	r7 = b[r8+0] (u)
	r10 = r3
	r4 = r2
	r9 = r1
	r6 = r0
	r5 = 0
	if (r6 == 0) goto .LBB15_8
; BB#1:
	if (r7 == 0) goto .LBB15_8
; BB#2:
	r0 = r4 + -1
	if (r0 > 63) goto .LBB15_8
; BB#3:
	r0 = b[r6+2] (u)
	if (r0 == 0) goto .LBB15_8
; BB#4:
	r0 = 110022
	r0 = b[r6+r0] (u)
	if (r0 == 0) goto .LBB15_8
; BB#5:
	r0 = 110023
	r0 = b[r6+r0] (u)
	if (r0 != 0) goto .LBB15_8
; BB#6:
	call pa_hw_accel_broken
	if (r0 != 0) goto .LBB15_8
; BB#7:
	r0 = [r6+12] 
	r1 = 981668463
	iff (r0 u<= r1) {
		r0 = r5 + 0x3F800000
	}
	r0 = r0 * r10 (f)
	r2 = 29383680
	r0 = r6  #
	[sp] = r0
	r1 = r9
	r3 = r4
	call wn_condition_block
	r5 = 1
.LBB15_8:
	b[r8+1] = r5
	sp += 4
	{pc, r10-r4} = [sp++]
.Lfunc_end15:
	.size	pa_wavenet_pre_core0, .Lfunc_end15-pa_wavenet_pre_core0

	.p2align	1
	.type	wn_post_block,@function
wn_post_block:                          ; @wn_post_block
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -24
	r4 = 119632
	r4 += r0
	r7 = [r4+4] 
	[sp+8] = r4                     ; 4-byte Folded Spill
	r13 = [r4+0]
	r4 = [sp+84]
	[sp+4] = r4                     ; 4-byte Folded Spill
	r4 = [sp+80]
	[sp] = r4                       ; 4-byte Folded Spill
	if (r3 == 0) goto .LBB16_27
; BB#1:
	r15 = 0  #
	r4 = b[r0+4] (u)
	r9 = r13  #
	[sp+20] = r4                    ; 4-byte Folded Spill
	r4 = r15 + 0x4A000000  #
	r0 = [r0+8] 
	r8 = 0  #
	[sp+16] = r0                    ; 4-byte Folded Spill
	r12 = 0  #
	r0 = [sp+76]
	[sp+12] = r0                    ; 4-byte Folded Spill
	r11 = 1325400063
.LBB16_2:                               ; =>This Inner Loop Header: Depth=1
	r0 = 1065341472
	r7 = r7 * r0 (f)  #
	r5 = [r1++=4] 
	r0 = [sp+16]                    ; 4-byte Folded Reload
	r6 = r0 * r5 (f)
	r0 = r4 + 0xF6000000
	r5 = r0
	iff (r6 u<= r0) {
		r5 = r6
	}
	r0 = r0 + 0x80000000
	r13 = r0
	iff (r5 u>= r0) {
		r13 = r5
	}
	r5 = r13 - r9 (f)  #
	r0 = [sp+12]                    ; 4-byte Folded Reload
	r7 = r7 + r5 (f)
	r14 = r7 * r0 (f)  #
	r0 = [sp+20]                    ; 4-byte Folded Reload
	if (r0 == 0) goto .LBB16_13
; BB#3:                                 ;   in Loop: Header=BB16_2 Depth=1
	r0 = 1067030938
	iff (r14 u<= r0) goto .LBB16_8
; BB#4:                                 ;   in Loop: Header=BB16_2 Depth=1
	r0 = r0 + 0x80000000
	r0 = r14 + r0 (f)
	r6 = r0 + r0 (f)
	r5 = r15 + 0x3F800000
	r0 = r5 + 0xC00000
	iff (r6 >= r0) goto .LBB16_7
; BB#5:                                 ;   in Loop: Header=BB16_2 Depth=1
	r5 = r4 + 0x75800000
	r0 = r0 + 0x80000000
	iff (r6 <= r0) goto .LBB16_7
; BB#6:                                 ;   in Loop: Header=BB16_2 Depth=1
	r0 = r6 * r6 (f)
	r5 = 1091567616
	r5 = r0 * r5 (f)
	r4 = 1104674816
	r5 = r5 + r4 (f)
	r0 = r0 + r4 (f)
	r4 = r15 + 0x4A000000
	r0 = r6 * r0 (f)
	r5 = r0 / r5 (f)
.LBB16_7:                               ;   in Loop: Header=BB16_2 Depth=1
	r0 = 1041865114
	r0 = r5 * r0 (f)
	r5 = 1067030938
	r14 = r0 + r5 (f)
.LBB16_8:                               ;   in Loop: Header=BB16_2 Depth=1
	r0 = -1080452710
	iff (r14 u>= r0) goto .LBB16_24
; BB#9:                                 ;   in Loop: Header=BB16_2 Depth=1
	r0 = r0 + 0x80000000
	r0 = r14 + r0 (f)
	r6 = r0 + r0 (f)
	r5 = r15 + 0x3F800000
	r0 = r5 + 0xC00000
	iff (r6 >= r0) goto .LBB16_12
; BB#10:                                ;   in Loop: Header=BB16_2 Depth=1
	r5 = r4 + 0x75800000
	r0 = r0 + 0x80000000
	iff (r6 <= r0) goto .LBB16_12
; BB#11:                                ;   in Loop: Header=BB16_2 Depth=1
	r0 = r6 * r6 (f)
	r5 = 1091567616
	r5 = r0 * r5 (f)
	r4 = 1104674816
	r5 = r5 + r4 (f)
	r0 = r0 + r4 (f)
	r4 = r15 + 0x4A000000
	r0 = r6 * r0 (f)
	r5 = r0 / r5 (f)
.LBB16_12:                              ;   in Loop: Header=BB16_2 Depth=1
	r0 = 1041865114
	r0 = r5 * r0 (f)
	r5 = -1080452710
	goto .LBB16_23
.LBB16_13:                              ;   in Loop: Header=BB16_2 Depth=1
	r0 = 1068708659
	iff (r14 u<= r0) goto .LBB16_18
; BB#14:                                ;   in Loop: Header=BB16_2 Depth=1
	r0 = r0 + 0x80000000
	r5 = r14 + r0 (f)
	r6 = r5 + r5 (f)
	r5 = r15 + 0x3F800000
	r0 = r5 + 0xC00000
	iff (r6 >= r0) goto .LBB16_17
; BB#15:                                ;   in Loop: Header=BB16_2 Depth=1
	r5 = r4 + 0x75800000
	r0 = r0 + 0x80000000
	iff (r6 <= r0) goto .LBB16_17
; BB#16:                                ;   in Loop: Header=BB16_2 Depth=1
	r4 = r6 * r6 (f)
	r0 = 1091567616
	r0 = r4 * r0 (f)
	r5 = 1104674816
	r0 = r0 + r5 (f)
	r5 = r4 + r5 (f)
	r4 = r15 + 0x4A000000
	r5 = r6 * r5 (f)
	r5 = r5 / r0 (f)
.LBB16_17:                              ;   in Loop: Header=BB16_2 Depth=1
	r0 = 1036831949
	r0 = r5 * r0 (f)
	r5 = 1068708659
	r14 = r0 + r5 (f)
.LBB16_18:                              ;   in Loop: Header=BB16_2 Depth=1
	r0 = -1078774989
	iff (r14 u>= r0) goto .LBB16_24
; BB#19:                                ;   in Loop: Header=BB16_2 Depth=1
	r0 = r0 + 0x80000000
	r0 = r14 + r0 (f)
	r6 = r0 + r0 (f)
	r5 = r15 + 0x3F800000
	r0 = r5 + 0xC00000
	iff (r6 >= r0) goto .LBB16_22
; BB#20:                                ;   in Loop: Header=BB16_2 Depth=1
	r5 = r4 + 0x75800000
	r0 = r0 + 0x80000000
	iff (r6 <= r0) goto .LBB16_22
; BB#21:                                ;   in Loop: Header=BB16_2 Depth=1
	r4 = r6 * r6 (f)
	r5 = 1091567616
	r5 = r4 * r5 (f)
	r0 = 1104674816
	r5 = r5 + r0 (f)
	r0 = r4 + r0 (f)
	r4 = r15 + 0x4A000000
	r0 = r6 * r0 (f)
	r5 = r0 / r5 (f)
.LBB16_22:                              ;   in Loop: Header=BB16_2 Depth=1
	r0 = 1036831949
	r0 = r5 * r0 (f)
	r5 = -1078774989
.LBB16_23:                              ;   in Loop: Header=BB16_2 Depth=1
	r14 = r0 + r5 (f)
.LBB16_24:                              ;   in Loop: Header=BB16_2 Depth=1
	iff (r14 > r14) goto .LBB16_26
	iff (r14 <= r14) goto .LBB16_26
; BB#25:                                ;   in Loop: Header=BB16_2 Depth=1
	r14 = 0
.LBB16_26:                              ;   in Loop: Header=BB16_2 Depth=1
	r6 = r14 * r4 (f)
	r0 = r11 + 0x80000000
	r5 = r0
	iff (r6 u>= r0) {
		r5 = r6
	}
	r6 = r11
	iff (r5 u<= r6) {
		r6 = r5
	}
	r0 = ftoi(r6) (trunc)
	[r2++=4] = r0
	iff (r13 u>= r15) {
		r10 = r13
	} else {
		r10 = r13 ^ 0x80000000
	}
	iff (r10 u<= r12) {
		r10 = r12
	}
	iff (r14 < r15) {
		r14 = r14 ^ 0x80000000
	}
	iff (r14 u<= r8) {
		r14 = r8
	}
	r9 = r13
	r8 = r14
	r12 = r10
	if (--r3 != 0) goto .LBB16_2
	goto .LBB16_28
.LBB16_27:
	r10 = 0
	r14 = 0
.LBB16_28:
	r0 = [sp+8]                     ; 4-byte Folded Reload
	[r0+0] = r13
	[r0+4] = r7
	r0 = [sp]                       ; 4-byte Folded Reload
	[r0+0] = r14
	r0 = [sp+4]                     ; 4-byte Folded Reload
	[r0+0] = r10
	sp += 24
	{pc, r15-r4} = [sp++]
.Lfunc_end16:
	.size	wn_post_block, .Lfunc_end16-wn_post_block

	.globl	pa_wavenet_post_core0
	.p2align	1
	.type	pa_wavenet_post_core0,@function
pa_wavenet_post_core0:                  ; @pa_wavenet_post_core0
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -276
	r9 = .L_MergedGlobals
	r4 = b[r9+2] (u)
	r3 = r2
	r8 = r1
	if (r4 == 0) goto .LBB17_6
; BB#1:
	r1 = 0
	b[r9+2] = r1
	if (r0 == 0) goto .LBB17_6
; BB#2:
	ifs (r3 < 1) goto .LBB17_5
; BB#3:
	r4 = 119088
	r1 = [r9+4]
	r4 += r0
	r5 = sp + 20
	r6 = 0x35000000
	r7 = r3
.LBB17_4:                               ; =>This Inner Loop Header: Depth=1
	r2 = [r4++=4] 
	r2 += r1
	r2 = itof(r2)
	r2 = r2 * r6 (f)
	[r5++=4] = r2
	if (--r7 != 0) goto .LBB17_4
.LBB17_5:
	r4 = [r9+8]
	r1 = sp + 12
	r2 = r8  #
	[sp+8] = r1
	r1 = sp + 16
	[sp+4] = r1
	r1 = sp + 20
	[sp] = r4
	call wn_post_block
	r2 = .L_MergedGlobals
	r1 = [r2+12] 
	r0 = [sp+16]
	iff (r0 > r1) {
		[r2+12] = r0
	}
	r1 = [r2+16] 
	r0 = [sp+12]
	iff (r0 > r1) {
		[r2+16] = r0
	}
.LBB17_6:
	sp += 276
	{pc, r15-r4} = [sp++]
.Lfunc_end17:
	.size	pa_wavenet_post_core0, .Lfunc_end17-pa_wavenet_post_core0

	.p2align	1
	.type	wn_P,@function
wn_P:                                   ; @wn_P
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -32
	r5_r4 = d[sp+88]
	r13 = r5 - r4  #
	[sp+20] = r0                    ; 4-byte Folded Spill
	r12 = [sp+84]
	r0 = r4 * 0xC
	r0 += r3
	r2 = r4 - r2  #
	[sp+28] = r0                    ; 4-byte Folded Spill
	r0 = [r12+12]
	r2 = r2 * 0xC
	r1 += r2
	[sp+24] = r1                    ; 4-byte Folded Spill
	r4 = .L_MergedGlobals
	if (r0 == 0) goto .LBB18_14
; BB#1:
	r1 = [r12+8]
	r7 = [r12+20]
	if (r13 == 0) goto .LBB18_13
; BB#2:
	r2 = [r12+0]
	r15 = [r12+16]
	r1 = r1 * 0xC
	r1 += r2
	r1 = r15 * 0x3  #
	[sp+16] = r1                    ; 4-byte Folded Spill
	r8 = [r12+24]
	r9 = 0  #
	r2 = [sp+20]                    ; 4-byte Folded Reload
	r11 = r2 + 36
	r1 = r1 << 2
	[sp+12] = r1                    ; 4-byte Folded Spill
	goto .LBB18_4
.LBB18_3:                               ;   in Loop: Header=BB18_4 Depth=1
	r0 = [r12+12]
.LBB18_4:                               ; =>This Loop Header: Depth=1
                                        ;     Child Loop BB18_10 Depth 2
	r1 = r13 - r9  #
	r4 = b[r4+3] (u)
	r2 = r15 - r7
	r10 = umin(r2, r1)  #
	r1 = [sp+16]                    ; 4-byte Folded Reload
	r3 = r9 * 0xC
	r2 = r1 + r3
	r1 = r7 * 0xC
	r14 = r0 + r1  #
	r0 = [sp+28]                    ; 4-byte Folded Reload
	r1 = r0 + r3  #
	r0 = [sp+24]                    ; 4-byte Folded Reload
	r0 += r3
	if (r4 == 0) goto .LBB18_6
; BB#5:                                 ;   in Loop: Header=BB18_4 Depth=1
	r4 = wn_asm_mix
	;APP
	;NO_APP
	[sp+8] = r10
	[sp+4] = r14
	r3 = r11  #
	[sp] = r2
	r2 = [sp+20]                    ; 4-byte Folded Reload
	call r4
	goto .LBB18_7
.LBB18_6:                               ;   in Loop: Header=BB18_4 Depth=1
	[sp+8] = r10
	[sp+4] = r14
	r3 = r11  #
	[sp] = r2
	r2 = [sp+20]                    ; 4-byte Folded Reload
	call wn_post_mix_c
.LBB18_7:                               ;   in Loop: Header=BB18_4 Depth=1
	if (r8 <= r7) goto .LBB18_11
; BB#8:                                 ;   in Loop: Header=BB18_4 Depth=1
	r0 = r8 - r7
	r0 = umin(r0, r10)
	if (r0 == 0) goto .LBB18_11
; BB#9:                                 ;   in Loop: Header=BB18_4 Depth=1
	r1 = [sp+12]                    ; 4-byte Folded Reload
	r1 += r14
	r2 = 0
.LBB18_10:                              ;   Parent Loop BB18_4 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	;APP
	{r6-r4} = [r14++]

	[r1++] = {r6-r4}

	;NO_APP
	r2 += 1
	if (r2 < r0) goto .LBB18_10
.LBB18_11:                              ;   in Loop: Header=BB18_4 Depth=1
	r0 = r10 + r7
	r7 = 0
	if (r0 < r15) {
		r7 = r0
	}
	r9 += r10
	r4 = .L_MergedGlobals
	if (r13 > r9) goto .LBB18_3
; BB#12:
	r1 = [r12+8]
.LBB18_13:
	[r12+20] = r7
	r2 = [r12+4]
	r0 = r1 + r13
	r1 = 0
	if (r0 >= r2) {
		r1 = r2
	}
	r0 = r0 - r1
	[r12+8] = r0
	sp += 32
	{pc, r15-r4} = [sp++]
.LBB18_14:
	r0 = b[r4+3] (u)
	if (r0 == 0) goto .LBB18_16
; BB#15:
	r3 = wn_asm_head
	;APP
	;NO_APP
	r1_r0 = d[sp+24]                ; 4-byte Folded Reload
	r2 = r13
	call r3
	sp += 32
	{pc, r15-r4} = [sp++]
.LBB18_16:
	r1_r0 = d[sp+24]                ; 4-byte Folded Reload
	r2 = r13
	call wn_post_head_c
	sp += 32
	{pc, r15-r4} = [sp++]
.Lfunc_end18:
	.size	wn_P, .Lfunc_end18-wn_P

	.p2align	1
	.type	wn_g_run,@function
wn_g_run:                               ; @wn_g_run
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -16
	r2 = [r0+24] 
	r5 = [r0+20] 
	r2 *= r5  #
	r3 = [r0+4] 
	r4 = [r0+8] 
	[sp+4] = r4                     ; 4-byte Folded Spill
	r11 = [r0+16]
	r10 = [r0+0]
	r2 = r2 << 2
	r13 = r3 + r2
	d[sp+8] = r1_r0                 ; 4-byte Folded Spill
	if (r11 != 6) goto .LBB19_4
; BB#1:
	r3 = [sp+4]                     ; 4-byte Folded Reload
	if (r1 == 0) goto .LBB19_17
; BB#2:
	r4 = r10  #
	r2 = [sp+8]                     ; 4-byte Folded Reload
	r1_r0 = d[r2+28]
	r0 = r0 * 0xC
	r10 = r4 + r0
	r1 = r1 * 0xC
	r11 = r4 + r1
	r1_r0 = d[r2+36]
	r0 = r0 * 0xC
	r14 = r4 + r0  #
	r0 = [r2+44] 
	r1 = r1 * 0xC  #
	r2 = [r2+48] 
	r15 = r4 + r1  #
	r1 = [sp+12]                    ; 4-byte Folded Reload
	r0 = r0 * 0xC
	r12 = r4 + r0
	r0 = r2 * 0xC
	r2 = r4 + r0
.LBB19_3:                               ; =>This Inner Loop Header: Depth=1
	;APP
	{r6-r4} = [r10++]

	{r9-r7} = [r11++]

	[r13++] = {r6-r4}

	[r13++] = {r9-r7}

	;NO_APP
	;APP
	{r6-r4} = [r14++]

	{r9-r7} = [r15++]

	[r13++] = {r6-r4}

	[r13++] = {r9-r7}

	;NO_APP
	;APP
	{r6-r4} = [r12++]

	{r9-r7} = [r2++]

	[r13++] = {r6-r4}

	[r13++] = {r9-r7}

	;NO_APP
	r0 = [r3++=4] 
	[r13+0] = r0
	[r13+4] = 0x200000
	r13 = r13 + 8
	if (--r1 != 0) goto .LBB19_3
	goto .LBB19_17
.LBB19_4:
	r14 = 0  #
	[sp] = r5                       ; 4-byte Folded Spill
	if (r11 < 2) goto .LBB19_10
; BB#5:
	r0 = [sp]                       ; 4-byte Folded Reload
	r3 = r0 + -6  #
	r2 = [sp+8]                     ; 4-byte Folded Reload
	r0 = 1
	r12 = r2 + 28
	r15 = r3 << 2
.LBB19_6:                               ; =>This Loop Header: Depth=1
                                        ;     Child Loop BB19_8 Depth 2
	if (r1 == 0) goto .LBB19_9
; BB#7:                                 ;   in Loop: Header=BB19_6 Depth=1
	r2 = [r12+r14<<2]
	r1 = r14 * 0xC
	r0 = [r12+r0<<2]
	r1 += r13
	r2 = r2 * 0xC
	r4 = r10
	r3 = r4 + r2  #
	r2 = [sp+12]                    ; 4-byte Folded Reload
	r0 = r0 * 0xC
	r0 += r4
.LBB19_8:                               ;   Parent Loop BB19_6 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	;APP
	{r6-r4} = [r3++]

	{r9-r7} = [r0++]

	[r1++] = {r6-r4}

	[r1++] = {r9-r7}

	;NO_APP
	r1 += r15
	if (--r2 != 0) goto .LBB19_8
.LBB19_9:                               ;   in Loop: Header=BB19_6 Depth=1
	r14 = r14 + 2  #
	r1 = [sp+12]                    ; 4-byte Folded Reload
	r0 = r14 | 0x1
	if (r0 < r11) goto .LBB19_6
.LBB19_10:
	r8 = [sp]                       ; 4-byte Folded Reload
	if (r14 >= r11) goto .LBB19_14
; BB#11:
	if (r1 == 0) goto .LBB19_17
; BB#12:
	r0 = [sp+8]                     ; 4-byte Folded Reload
	r0 += 28
	r1 = [r0+r14<<2]
	r0 = r14 * 0xC
	r0 += r13
	r1 = r1 * 0xC
	r7 = r10 + r1  #
	r1 = [sp+12]                    ; 4-byte Folded Reload
	r2 = r8 + -3
	r2 = r2 << 2
	r3 = r1
.LBB19_13:                              ; =>This Inner Loop Header: Depth=1
	;APP
	{r6-r4} = [r7++]

	[r0++] = {r6-r4}

	;NO_APP
	r0 += r2
	if (--r3 != 0) goto .LBB19_13
.LBB19_14:
	r3 = [sp+4]                     ; 4-byte Folded Reload
	if (r1 == 0) goto .LBB19_17
; BB#15:
	r0 = r11 * 0xC  #
	r2 = [sp+12]                    ; 4-byte Folded Reload
	r0 += r13
	r1 = r8 << 2
.LBB19_16:                              ; =>This Inner Loop Header: Depth=1
	r4 = r3
	r3 = [r4++=4] 
	r3 = r4  #
	[r0+0] = r3
	[r0+4] = 0x200000
	r0 += r1
	if (--r2 != 0) goto .LBB19_16
.LBB19_17:
	r5 = [sp+8]                     ; 4-byte Folded Reload
	r0 = [r5+16] 
	r6 = [sp+12]                    ; 4-byte Folded Reload
	if (r0 == 0) goto .LBB19_20
; BB#18:
	r0 = r5 + 28
	r1 = 0
.LBB19_19:                              ; =>This Inner Loop Header: Depth=1
	r2 = [r0+r1<<2]
	r2 += r6  #
	r4 = [r5+12] 
	r3 = 0
	if (r2 >= r4) {
		r3 = r4
	}
	r2 = r2 - r3
	[r0+r1<<2] = r2
	r2 = [r5+16] 
	r1 += 1
	if (r1 < r2) goto .LBB19_19
.LBB19_20:
	[r5+24] += r6
	r0 = r6 << 2
	[r5+8] += r0
	sp += 16
	{pc, r15-r4} = [sp++]
.Lfunc_end19:
	.size	wn_g_run, .Lfunc_end19-wn_g_run

	.globl	pa_wavenet_process_block_q21
	.p2align	1
	.type	pa_wavenet_process_block_q21,@function
pa_wavenet_process_block_q21:           ; @pa_wavenet_process_block_q21
; BB#0:
	[--sp] = {rets, r15-r4}
	sp += -996
	r14 = r2
	r15 = r0
	if (r0 == 0) goto .LBB20_44
; BB#1:
	if (r1 == 0) goto .LBB20_44
; BB#2:
	[sp+176] = r1                   ; 4-byte Folded Spill
	call plat_ticks24
	r2 = [r15+20]
	r1 = 1028443341
	r11 = r0  #
	[sp+184] = r2                   ; 4-byte Folded Spill
	iff (r2 > r1) goto .LBB20_4
; BB#3:
	r0 = b[r15+3] (u)
	if (r0 == 0) {
		r0 = 0x3F800000
	} else {
		r0 = 1068708659
	}
	[sp+184] = r0                   ; 4-byte Folded Spill
.LBB20_4:
	r0 = r14 + -1
	r5 = 0
	r10 = .L_MergedGlobals
	r4 = g_pa_wn_lean
	r1 = 109976
	r12 = r15 + r1
	r1 += 44
	r8 = r15 + r1
	r1 = 119344
	r7 = r15 + r1
	r1 = r1 + 244
	r6 = r15 + r1
	r9 = 0
	[sp+188] = r15                  ; 4-byte Folded Spill
	[sp+160] = r12                  ; 4-byte Folded Spill
	if (r0 > 63) goto .LBB20_10
; BB#5:
	r0 = b[r8+2] (u)
	if (r0 == 0) goto .LBB20_10
; BB#6:
	r0 = b[r8+3] (u)
	if (r0 != 0) goto .LBB20_10
; BB#7:
	call pa_hw_accel_broken
	r9 = 1
	if (r0 == 0) goto .LBB20_9
; BB#8:
	r9 = 0
.LBB20_9:
	if (r0 == 0) goto .LBB20_45
.LBB20_10:
	b[r4+0] = r5
	ifs (r14 < 1) goto .LBB20_39
; BB#11:
	[sp+88] = r11                   ; 4-byte Folded Spill
	[sp+92] = r9                    ; 4-byte Folded Spill
	[sp+100] = r8                   ; 4-byte Folded Spill
	r8 = r14  #
	[sp+96] = r7                    ; 4-byte Folded Spill
	r0 = r7 + 52
	r0 = r15 + 7508  #
	[sp+200] = r0                   ; 4-byte Folded Spill
	r12 = 0  #
	[sp+196] = r0                   ; 4-byte Folded Spill
	r11 = -822083585
	r13 = r11 + 0x80000000  #
	r1 = [sp+176]                   ; 4-byte Folded Reload
	r14 = 0
	r9 = 0
.LBB20_12:                              ; =>This Inner Loop Header: Depth=1
	r0 = [r1+r9<<2]
	r0 = itof(r0)  #
	r1 = [r6+8] 
	r3 = r12 + 0x35000000  #
	r2 = [r6+4] 
	r0 = r0 * r3 (f)
	r3 = 1065317984
	r1 = r1 * r3 (f)  #
	[r6+4] = r0
	r0 = r0 - r2 (f)
	r3 = r0 + r1 (f)
	r5 = r6  #
	[r6+8] = r3
	r1 = [r6+24] 
	r7 = [r6+12] 
	r0 = b[r15+4] (u)
	r2 = [r6+20] 
	r4 = [r6+16] 
	[r6+16] = r7
	[r6+12] = r3
	[r6+24] = r2
	if (r0 >= 5) {
		r0 = 0
	}
	r0 = r0 * 0x14
	r15 = k_hpf_profiles
	r6 = [++r15=r0]
	r10 = [r15+4]
	r3 = r3 * r6 (f)
	r7 = r10 * r7 (f)
	r3 = r3 + r7 (f)
	r7_r6 = d[r15+8]
	r4 = r6 * r4 (f)
	r6 = [r15+16]
	r3 = r3 + r4 (f)
	r2 = r7 * r2 (f)
	r2 = r3 - r2 (f)
	r1 = r6 * r1 (f)
	r1 = r2 - r1 (f)
	r6 = r5
	[r6+20] = r1
	r3_r2 = d[r6+28]
	r10 = [r6+40]
	r4 = [r6+36] 
	[r6+32] = r2
	[r6+28] = r1
	[r6+40] = r4
	r5 = k_lpf_profiles
	r7 = [++r0=r5]
	r1 = r1 * r7 (f)  #
	r5 = [r0+4] 
	r2 = r5 * r2 (f)  #
	r7 = [r0+8] 
	r1 = r1 + r2 (f)  #
	r2 = [r0+12] 
	r5 = r7 * r3 (f)  #
	r0 = [r0+16] 
	r1 = r1 + r5 (f)
	r2 = r2 * r4 (f)
	r1 = r1 - r2 (f)
	r0 = r0 * r10 (f)
	r1 = r1 - r0 (f)
	[r6+36] = r1
	r15 = [sp+188]                  ; 4-byte Folded Reload
	r0 = r15
	call pa_wavenet_step_sample
	r7 = r0  #
	r1 = [r6+44] 
	r0 = r7 - r1 (f)  #
	r2 = [r6+48] 
	r1 = 1065341472
	r1 = r2 * r1 (f)  #
	[r6+44] = r7
	r0 = r0 + r1 (f)
	[r6+48] = r0
	r1 = b[r15+4] (u)
	r2 = [sp+184]                   ; 4-byte Folded Reload
	r4 = r2 * r0 (f)
	if (r1 == 0) goto .LBB20_23
; BB#13:                                ;   in Loop: Header=BB20_12 Depth=1
	r0 = 1067030938
	iff (r4 u<= r0) goto .LBB20_18
; BB#14:                                ;   in Loop: Header=BB20_12 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r12 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB20_17
; BB#15:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB20_17
; BB#16:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB20_17:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = 1041865114
	r0 = r1 * r0 (f)
	r1 = 1067030938
	r4 = r0 + r1 (f)
.LBB20_18:                              ;   in Loop: Header=BB20_12 Depth=1
	r10 = .L_MergedGlobals
	r0 = -1080452710
	iff (r4 u>= r0) goto .LBB20_35
; BB#19:                                ;   in Loop: Header=BB20_12 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r12 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB20_22
; BB#20:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB20_22
; BB#21:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB20_22:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = 1041865114
	r0 = r1 * r0 (f)
	r1 = -1080452710
	goto .LBB20_34
.LBB20_23:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = 1068708659
	iff (r4 u<= r0) goto .LBB20_28
; BB#24:                                ;   in Loop: Header=BB20_12 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r12 + 0x3F800000
	r10 = .L_MergedGlobals
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB20_27
; BB#25:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB20_27
; BB#26:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB20_27:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = 1036831949
	r0 = r1 * r0 (f)
	r1 = 1068708659
	r4 = r0 + r1 (f)
	goto .LBB20_29
.LBB20_28:                              ;   in Loop: Header=BB20_12 Depth=1
	r10 = .L_MergedGlobals
.LBB20_29:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = -1078774989
	iff (r4 u>= r0) goto .LBB20_35
; BB#30:                                ;   in Loop: Header=BB20_12 Depth=1
	r0 = r0 + 0x80000000
	r0 = r4 + r0 (f)
	r0 = r0 + r0 (f)
	r1 = r12 + 0x3F800000
	r2 = r1 + 0xC00000
	iff (r0 >= r2) goto .LBB20_33
; BB#31:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r1 + 0x80000000
	r2 = r1 + 0xC00000
	iff (r0 <= r2) goto .LBB20_33
; BB#32:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = r0 * r0 (f)
	r2 = 1091567616
	r2 = r1 * r2 (f)
	r3 = 1104674816
	r2 = r2 + r3 (f)
	r1 = r1 + r3 (f)
	r0 = r0 * r1 (f)
	r1 = r0 / r2 (f)
.LBB20_33:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = 1036831949
	r0 = r1 * r0 (f)
	r1 = -1078774989
.LBB20_34:                              ;   in Loop: Header=BB20_12 Depth=1
	r4 = r0 + r1 (f)
.LBB20_35:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = r4
	call __fpclassifyf
	if (r0 > 1) goto .LBB20_37
; BB#36:                                ;   in Loop: Header=BB20_12 Depth=1
	r1 = 0
	r0 = [sp+196]                   ; 4-byte Folded Reload
	r2 = 76248
	call memset
	r1 = 0
	r2 = 46
	r0 = [sp+160]                   ; 4-byte Folded Reload
	call memset
	r1 = 0
	r2 = 192
	r0 = [sp+200]                   ; 4-byte Folded Reload
	call memset
	h[r6+0] = r12
	[r6+8] = r12
	[r6+4] = r12
	[r6+24] = r12
	[r6+20] = r12
	[r6+16] = r12
	[r6+12] = r12
	[r6+40] = r12
	[r6+36] = r12
	[r6+32] = r12
	[r6+28] = r12
	[r6+48] = r12
	[r6+44] = r12
	[r6+52] = r12
	[r10+20] += 1
	r4 = 0
.LBB20_37:                              ;   in Loop: Header=BB20_12 Depth=1
	r0 = r12 + 0x4A000000
	r1 = r4 * r0 (f)
	r0 = r11
	iff (r1 u>= r0) {
		r0 = r1
	}
	r1 = r13
	iff (r0 u<= r1) {
		r1 = r0
	}
	r0 = ftoi(r1) (trunc)  #
	r1 = [sp+176]                   ; 4-byte Folded Reload
	[r1+r9<<2] = r0
	iff (r7 < r14) {
		r7 ^= 2147483648
	}
	iff (r4 < r14) {
		r4 ^= 2147483648
	}
	r0 = [r10+12]
	iff (r4 > r0) {
		[r10+12] = r4
	}
	r0 = [r10+16]
	iff (r7 > r0) {
		[r10+16] = r7
	}
	r9 = r9 + 1
	ifs (r9 < r8) goto .LBB20_12
; BB#38:
	r14 = r8  #
	r7 = [sp+96]                    ; 4-byte Folded Reload
	r8 = [sp+100]                   ; 4-byte Folded Reload
	r9 = [sp+92]                    ; 4-byte Folded Reload
	r11 = [sp+88]                   ; 4-byte Folded Reload
.LBB20_39:
	call plat_ticks24
	r3 = b[r10+24] (u)
	r0 = r0 - r11
	r1 = 29880064
	r2 = r1 + 64
	if (r3 != 0) goto .LBB20_41
; BB#40:
	r3 = 1
	b[r10+24] = r3
	r3 = 0
	[r1+0] = r3
	[r1+4] = r3
	[r1+8] = r3
	[r1+12] = r3
	[r1+16] = r3
	[r1+20] = r3
	[r1+24] = r3
	[r1+28] = r3
	[r1+32] = r3
	[r1+36] = r3
	[r1+40] = r3
	[r1+44] = r3
	[r1+48] = r3
	[r1+52] = r3
	[r1+56] = r3
	[r1+60] = r3
	[r2+0] = r3
	[r2+4] = r3
	[r2+8] = r3
	[r2+12] = r3
	[r2+16] = r3
	[r2+20] = r3
	[r2+24] = r3
	[r2+28] = r3
	[r2+32] = r3
	[r2+36] = r3
	[r2+40] = r3
	[r2+44] = r3
	[r2+48] = r3
	[r2+52] = r3
	[r2+56] = r3
	[r2+60] = r3
.LBB20_41:
	[r1+4] = r0
	r3 = [r1+8] 
	if (r0 > r3) {
		[r1+8] = r0
	}
	[r1+12] += 1
	r3 = [r10+20]
	[r1+16] = r3
	r3 = [r10+12]
	r4 = 1232348160
	r3 = r3 * r4 (f)
	r3 = ftou(r3) (trunc)
	[r1+20] = r3
	r3 = [r10+16]
	r3 = r3 * r4 (f)
	r3 = ftou(r3) (trunc)
	[r1+24] = r3
	r3 = 1148846080
	r4 = [sp+184]                   ; 4-byte Folded Reload
	r4 = r4 * r3 (f)
	r4 = ftou(r4) (trunc)
	[r1+28] = r4
	r4 = b[r15+4] (u)
	[r1+32] = r4
	r4 = b[r15+3] (u)
	[r1+36] = r4
	r4 = [r15+12]
	r3 = r4 * r3 (f)
	r3 = ftou(r3) (trunc)
	[r1+40] = r3
	r3 = [r7+4] 
	[r1+44] = r3
	r3 = [r7+8] 
	[r1+48] = r3
	r3 = [r7+24] 
	[r1+52] = r3
	r3 = b[r8+3] (u)
	[r1+56] = r3
	r3 = b[r8+4] (u)
	[r1+60] = r3
	[r2+0] = r9
	r3 = [r7+12] 
	[r2+4] = r3
	r3 = [r7+16] 
	[r2+8] = r3
	r3 = [r7+28] 
	[r2+12] = r3
	r3 = [r7+32] 
	[r2+16] = r3
	r3 = [r7+36] 
	[r2+20] = r3
	r3 = [r7+40] 
	[r2+24] = r3
	r3 = [r7+44] 
	[r2+28] = r3
	r3 = b[r10+3] (u)
	r4 = b[r7+0] (u)
	r3 = r3 << 1
	r3 |= r4
	[r2+32] = r3
	r2 = 1196311639
	[r1+0] = r2
	ifs (r14 < 1) goto .LBB20_44
; BB#42:
	if (r0 == 0) goto .LBB20_44
; BB#43:
	r0 = r0 * 0xF
	r0 = r0 / r14 (u)  #
	[r6+56] = r0
	r1 = [r6+52] 
	if (r0 > r1) {
		[r6+52] = r0
	}
.LBB20_44:
	sp += 996
	{pc, r15-r4} = [sp++]
.LBB20_45:
	r0 = b[r10+0] (u)
	r1 = 0
	[sp+60] = r1                    ; 4-byte Folded Spill
	r2 = 0
	if (r0 == 0) goto .LBB20_48
; BB#46:
	r0 = g_pa_wn_inline
	r0 = b[r0+0] (u)
	r2 = 1
	if (r0 == 0) goto .LBB20_48
; BB#47:
	r2 = 0
.LBB20_48:
	r0 = b[r10+1] (u)
	r1 = 1
	b[r4+0] = r1
	r1 = sp + 476
	if (r2 != 0) goto .LBB20_50
; BB#49:
	[sp+60] = r1                    ; 4-byte Folded Spill
.LBB20_50:
	[sp+40] = r2                    ; 4-byte Folded Spill
	if (r0 == 0) goto .LBB20_52
; BB#51:
	r13 = 29383680
	goto .LBB20_53
.LBB20_52:
	r0 = [r15+12]
	r1 = 981668463
	iff (r0 u<= r1) {
		r0 = r5 + 0x3F800000
	}
	r13 = sp + 220
	r0 = r15  #
	[sp] = r0
	r2 = r13  #
	r1 = [sp+176]                   ; 4-byte Folded Reload
	r3 = r14
	call wn_condition_block
.LBB20_53:
	[sp+32] = r6                    ; 4-byte Folded Spill
	call plat_ticks24
	r1 = [r12+52]
	r2 = r14  #
	[sp+36] = r0                    ; 4-byte Folded Spill
	[sp+180] = r1                   ; 4-byte Folded Spill
	r0 = r1 + 1
	[r12+52] = r0
	r0 = 0
	[sp+980] = r0
	if (r2 >= 32) {
		r2 = 32
	}
	r0 = 3
	r1 = r2 / r0 (u)  #
	[sp+168] = r2                   ; 4-byte Folded Spill
	[sp+192] = r1                   ; 4-byte Folded Spill
	[sp+984] = r1
	r1 = r2 << 1
	r1 = r1 / r0 (u)
	[sp+988] = r1
	[sp+992] = r2
	[sp+964] = r2
	r3 = r14 - r2
	r1 = r3 / r0 (u)
	r1 += r2
	[sp+20] = r1                    ; 4-byte Folded Spill
	[sp+968] = r1
	r1 = r3 << 1
	r0 = r1 / r0 (u)  #
	[sp+140] = r3                   ; 4-byte Folded Spill
	r0 += r2
	r2 = r15  #
	[sp+24] = r0                    ; 4-byte Folded Spill
	[sp+972] = r0
	[sp+976] = r14
	r0 = 117872
	r15 = r2 + r0
	r1 = b[r10+32] (u)
	r0 = r0 + 960
	r0 += r2
	[sp+72] = r0                    ; 4-byte Folded Spill
	r2 = sp + 980
	r0 = g_pa_wn_specs
	[sp+132] = r0                   ; 4-byte Folded Spill
	[sp+64] = r14                   ; 4-byte Folded Spill
	[sp+96] = r7                    ; 4-byte Folded Spill
	[sp+100] = r8                   ; 4-byte Folded Spill
	[sp+92] = r9                    ; 4-byte Folded Spill
	[sp+88] = r11                   ; 4-byte Folded Spill
	[sp+196] = r2                   ; 4-byte Folded Spill
	if (r1 == 0) goto .LBB20_55
; BB#54:
	r1 = [r10+28]
	goto .LBB20_58
.LBB20_55:
	r2 = 0
	r1 = 36
	r3 = r0
	r4 = 0
.LBB20_56:                              ; =>This Inner Loop Header: Depth=1
	r5 = r10
	[++r5=r1] = r2
	r6 = h[r3+2] (u)
	r7 = b[r3++=8] (u)
	[r5+92] = r4
	r5 = r7 * 0x9
	r4 += r5
	r4 += 21
	r5 = r7 + -1
	r5 *= r6
	r2 += r5
	r1 += 4
	r2 += 96
	if (r1 != 128) goto .LBB20_56
; BB#57:
	r1 = 1887
	[r10+28] = r1
	r2 = 1
	b[r10+32] = r2
.LBB20_58:
	r0 = r15 + 192
	[sp+172] = r0                   ; 4-byte Folded Spill
	r1 = r1 << 2
	r2 = [r10+36]
	r0 = [sp+188]                   ; 4-byte Folded Reload
	r3 = r0 + r1
	r1 = h[r12+0] (u)
	r2 = r2 * 0xC
	r2 += r0  #
	r0 = [sp+64]                    ; 4-byte Folded Reload
	r11 = r2 + 7508
	if (r0 == 0) goto .LBB20_62
; BB#59:
	r14 = r13  #
	[sp+164] = r3                   ; 4-byte Folded Spill
	r6 = r1 + 1
	r1 += -68
	if (r6 >= 69) {
		r6 = r1
	}
	r0 = 0
	r1 = 0
	d[sp+200] = r1_r0               ; 8-byte Folded Spill
	r13 = 0x4A000000  #
	r7 = [sp+72]                    ; 4-byte Folded Reload
	r1 = [sp+172]                   ; 4-byte Folded Reload
	r3 = [sp+64]                    ; 4-byte Folded Reload
.LBB20_60:                              ; =>This Inner Loop Header: Depth=1
	r2 = [r14++=4]
	r5 = r2 * r13 (f)
	r5 = ftoi(r5) (trunc)
	r5 = r6 * 0x3  #
	[r7++=4] = r5
	{r4, r0} = [r15+]
	r10 = r5 << 2
	r0 = r2 * r0 (f)
	r0 = r0 * r13 (f)
	r8 = ftoi(r0) (trunc)
	r0 = [r15+8]
	r5 = r10
	[++r5=r11] = r8
	r4 = r2 * r4 (f)
	r4 = r4 * r13 (f)
	r9 = ftoi(r4) (trunc)
	[r5+4] = r9
	r0 = r2 * r0 (f)
	r0 = r0 * r13 (f)
	r12 = ftoi(r0) (trunc)
	[r5+8] = r12
	if (r6 <= 31) {
		r0 = r11 + r10
		[++r0=828] = r8
		[r0+4] = r9
		[r0+8] = r12
	}
	r5 = 0
	[r1+8] = r5
	r6 += 1
	r9_r8 = d[sp+200]               ; 8-byte Folded Reload
	d[r1++=12] = r9_r8
	if (r6 <= 68) {
		r5 = r6
	}
	r6 = r5
	if (--r3 != 0) goto .LBB20_60
; BB#61:
	r12 = [sp+160]                  ; 4-byte Folded Reload
	r1 = h[r12+0] (u)
	r10 = .L_MergedGlobals
	r3 = [sp+164]                   ; 4-byte Folded Reload
.LBB20_62:
	r0 = [sp+188]                   ; 4-byte Folded Reload
	r2 = 119088
	r0 += r2
	r4 = r15 + 12  #
	[sp+52] = r0                    ; 4-byte Folded Spill
	r0 = [sp+180]                   ; 4-byte Folded Reload
	r13 = r0 & 0x1F
	r0 = 110032
	r0 += r3
	[sp+48] = r0                    ; 4-byte Folded Spill
	r0 = 6
	[sp+892] = r0
	r0 = 20
	[sp+896] = r0
	r0 = 69
	[sp+888] = r0
	[sp+876] = r11
	r0 = 29368320
	[sp+880] = r0
	r0 = 0
	[sp+900] = r0
	r2 = [sp+72]                    ; 4-byte Folded Reload
	[sp+884] = r2
	r3 = r1 + 1
	r2 = 0
	if (r3 >= 69) {
		r2 += -69
	}
	r1 += r2  #
	[sp+56] = r4                    ; 4-byte Folded Spill
	r2 = r1 + -4
	r4 = -70
	r3 = 0
	r11 = [sp+64]                   ; 4-byte Folded Reload
	r8 = [sp+140]                   ; 4-byte Folded Reload
	if (r2 <= r4) {
		r3 += -69
	}
	r2 += r3
	r2 += 69
	[sp+904] = r2
	r2 = r1 + 66  #
	r6 = [sp+188]                   ; 4-byte Folded Reload
	r3 = 0
	if (r2 >= 69) {
		r3 = 69
	}
	r2 = r2 - r3
	[sp+908] = r2
	r2 = r1 + 67
	r3 = 0
	if (r2 >= 69) {
		r3 = 69
	}
	r2 = r2 - r3
	[sp+912] = r2
	r2 = r1 + 68
	r3 = 0
	if (r2 >= 69) {
		r3 = 69
	}
	r2 = r2 - r3
	[sp+916] = r2
	r2 = r1 + 69
	r3 = 0
	if (r2 >= 69) {
		r3 = 69
	}
	r2 = r2 - r3
	r1 += 70
	[sp+920] = r2
	if (r1 >= 69) {
		r0 = 69
	}
	r0 = r1 - r0
	[sp+924] = r0
	r0 = sp + 876
	[sp+108] = r0                   ; 4-byte Folded Spill
	if (r13 == 0) goto .LBB20_64
; BB#63:
	r4 = sp + 876
	r0 = r4  #
	r1 = [sp+192]                   ; 4-byte Folded Reload
	call wn_g_run
	r5 = [sp+196]                   ; 4-byte Folded Reload
	r1_r0 = d[r5+4]
	r1 = r1 - r0
	r0 = r4
	call wn_g_run
	r1_r0 = d[r5+8]
	r1 = r1 - r0
	r0 = r4
	call wn_g_run
	r0 = 0
	goto .LBB20_65
.LBB20_64:
	call plat_ticks24
	r14 = r0  #
	r1 = [sp+192]                   ; 4-byte Folded Reload
	r4 = sp + 876
	r0 = r4
	call wn_g_run
	call plat_ticks24
	r9 = r0  #
	r5 = [sp+196]                   ; 4-byte Folded Reload
	r1 = [r5+4] 
	r2 = [r5+8] 
	r7 = r2 - r1
	call plat_ticks24
	r10 = r0
	r0 = r4
	r1 = r7
	call wn_g_run
	call plat_ticks24
	r12 = r0  #
	r1 = [r5+8] 
	r2 = [r5+12] 
	r5 = r2 - r1
	call plat_ticks24
	r7 = r0
	r1_r0 = r5_r4
	call wn_g_run
	r0 = r9 - r14
	r0 = r0 - r10
	r10 = .L_MergedGlobals
	r0 += r12
	r12 = [sp+160]                  ; 4-byte Folded Reload
	r4 = r0 - r7
	call plat_ticks24
	r0 += r4
.LBB20_65:
	r0 = r12 + 56  #
	[sp+192] = r0                   ; 4-byte Folded Spill
	r14 = 0  #
	[sp+116] = r0                   ; 4-byte Folded Spill
	r2 = [sp+168]                   ; 4-byte Folded Reload
	r0 = r2 << 2
	r0 += r6  #
	[sp+16] = r0                    ; 4-byte Folded Spill
	r1 = 118832
	r0 += r1  #
	[sp+28] = r0                    ; 4-byte Folded Spill
	r0 = r11 ^ 0xFFFFFFFF  #
	[sp+80] = r0                    ; 4-byte Folded Spill
	r1 = -33
	r0 = umax(r0, r1)
	r0 = 0x40 - r0
	r0 = r8 << 12  #
	[sp+76] = r0                    ; 4-byte Folded Spill
	[sp+44] = r0                    ; 4-byte Folded Spill
	r6 = sp + 760
	r0 = r2 << 12
	[sp+120] = r0                   ; 4-byte Folded Spill
	r0 = sp + 732
	[sp+68] = r0                    ; 4-byte Folded Spill
	r0 = sp + 788
	[sp+128] = r0                   ; 4-byte Folded Spill
	r0 = r2 + 1
	[sp+124] = r0                   ; 4-byte Folded Spill
	r0 = 0
	[sp+136] = r0                   ; 4-byte Folded Spill
	[sp+164] = r0                   ; 4-byte Folded Spill
	[sp+180] = r0                   ; 4-byte Folded Spill
	[sp+84] = r0                    ; 4-byte Folded Spill
	[sp+200] = r0                   ; 4-byte Folded Spill
	r11 = g_pa_wn_specs
	[sp+112] = r6                   ; 4-byte Folded Spill
.LBB20_66:                              ; =>This Loop Header: Depth=1
                                        ;     Child Loop BB20_68 Depth 2
                                        ;     Child Loop BB20_72 Depth 2
                                        ;     Child Loop BB20_86 Depth 2
                                        ;     Child Loop BB20_90 Depth 2
                                        ;     Child Loop BB20_100 Depth 2
	r5 = r14
	r15 = r5 + -1
	r0 = r15
	ifs (r5 <= 0) {
		r0 = 0
	}
	r1 = r10 + 128  #
	r3 = [sp+116]                   ; 4-byte Folded Reload
	r2 = [r1+r0<<2]
	r0 = r0 << 3
	r0 = b[r11+r0] (u)
	r2 = r2 << 2
	r2 += r3
	r0 = r0 * 0x24
	r1 = [r1+r5<<2]
	r4 = r5 << 3
	[sp+144] = r4                   ; 4-byte Folded Spill
	r14 = b[r11+r4] (u)
	r11 = r2 + r0
	r0 = r1 << 2
	r0 += r3
	r0 = r14 * 0x3  #
	[sp+152] = r0                   ; 4-byte Folded Spill
	r0 += 2
	r7 = r0 * 0xC  #
	[sp+156] = r0                   ; 4-byte Folded Spill
	r10 = r0 << 2  #
	r6 = [sp+140]                   ; 4-byte Folded Reload
	[sp+196] = r5                   ; 4-byte Folded Spill
	if (r6 == 0) goto .LBB20_71
; BB#67:                                ;   in Loop: Header=BB20_66 Depth=1
	r0 = .L_MergedGlobals
	r0 += 36
	r12 = r7  #
	[sp+104] = r0                   ; 4-byte Folded Spill
	r0 = [r0+r5<<2]
	r1 = g_pa_wn_specs
	r7 = r5  #
	r2 = [sp+144]                   ; 4-byte Folded Reload
	r1 += r2  #
	r5 = [sp+128]                   ; 4-byte Folded Reload
	r1 = h[r1+2] (u)
	[r5+16] = r14
	r2 = [sp+156]                   ; 4-byte Folded Reload
	r2 = r14 + -1  #
	[r5+20] = r2
	r2 *= r1
	r4 = r2 + 64
	r0 = r0 * 0xC  #
	[r5+12] = r4
	r3 = [sp+188]                   ; 4-byte Folded Reload
	r0 += r3
	r0 = r0 + 7508
	[sp+148] = r0                   ; 4-byte Folded Spill
	[sp+788] = r0
	r0 = 29376512
	[r5+4] = r0
	r0 = 0
	[r5+24] = r0
	r3 = [sp+80]                    ; 4-byte Folded Reload
	[r5+8] = r3
	r3 = [sp+160]                   ; 4-byte Folded Reload
	r5 = h[r3+r7<<1] (u)
	r3 = -64
	r8 = r3 - r2  #
	r3 = [sp+124]                   ; 4-byte Folded Reload
	r9 = r5 + r3
	r3 = 0
	if (r9 >= r4) {
		r3 = r8
	}
	r7 = r9 + r4
	r7 += r3
	r2 = r7 - r2
	if (r2 >= r4) {
		r0 = r4
	}
	r0 = r2 - r0  #
	r2 = [sp+128]                   ; 4-byte Folded Reload
	r7 = r12  #
	[r2+28] = r0
	r0 = [sp+76]                    ; 4-byte Folded Reload
	r0 += r3
	r0 += r1
	r2 = r0 + r5
	r5 = 8
.LBB20_68:                              ;   Parent Loop BB20_66 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	r0 = 0
	if (r2 >= r4) {
		r0 = r4
	}
	r0 = r2 - r0
	r3 = sp + 788
	[r3+r5<<2] = r0
	r0 = r5 + -6
	r5 += 1
	r2 += r1
	if (r0 < r14) goto .LBB20_68
; BB#69:                                ;   in Loop: Header=BB20_66 Depth=1
	r5 = [sp+196]                   ; 4-byte Folded Reload
	ifs (r5 < 1) goto .LBB20_71
; BB#70:                                ;   in Loop: Header=BB20_66 Depth=1
	r0 = r15 << 3  #
	r2 = [sp+104]                   ; 4-byte Folded Reload
	r1 = g_pa_wn_specs
	r0 += r1  #
	r5 = [sp+68]                    ; 4-byte Folded Reload
	r1 = b[r0+0] (u)
	r0 = h[r0+2] (u)
	r1 += -1
	r2 = [r2+r15<<2]
	r0 *= r1
	r1 = r0 + 64
	r0 = r2 * 0xC  #
	[r5+4] = r1
	r2 = [sp+188]                   ; 4-byte Folded Reload
	r0 += r2
	r0 = r0 + 7508
	[sp+732] = r0
	r0 = [sp+160]                   ; 4-byte Folded Reload
	r0 = h[r0+r15<<1] (u)
	r2 = [sp+124]                   ; 4-byte Folded Reload
	r2 += r0
	r0 = 0
	r3 = 0
	if (r2 >= r1) {
		r3 = r1
	}
	r1 = r2 - r3
	[r5+8] = r1
	[r5+16] = r4
	r1 = [sp+148]                   ; 4-byte Folded Reload
	[r5+12] = r1
	if (r9 >= r4) {
		r0 = r4
	}
	r0 = r9 - r0
	[r5+20] = r0
	r0 = 32
	[r5+24] = r0
	r5 = [sp+196]                   ; 4-byte Folded Reload
.LBB20_71:                              ;   in Loop: Header=BB20_66 Depth=1
	r12 = r11 + 24  #
	r1 = [sp+152]                   ; 4-byte Folded Reload
	r0 = r1 + r7
	[sp+148] = r0                   ; 4-byte Folded Spill
	r0 = [sp+120]                   ; 4-byte Folded Reload
	r2 = [sp+156]                   ; 4-byte Folded Reload
	r0 |= r2
	r15 = r0 | 0x18000000
	r8 = r10 | 0xC00000
	r0 = r14 * 0xC
	r11 = r0 + 8
	r7 = 0
	r10 = r1
.LBB20_72:                              ;   Parent Loop BB20_66 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	if (r13 == 0) goto .LBB20_74
; BB#73:                                ;   in Loop: Header=BB20_72 Depth=2
	[sp+4] = r8
	r0 = 29382656
	r3 = r7 + r0
	r0 = 2721
	[sp] = r15
	r1 = 29368320
	r2 = r10
	call pa_hw_accel_job_lean
	goto .LBB20_75
.LBB20_74:                              ;   in Loop: Header=BB20_72 Depth=2
	call plat_ticks24
	r4 = r0
	[sp+4] = r8
	r0 = 29382656
	r3 = r7 + r0
	r0 = 2721
	[sp] = r15
	r1 = 29368320
	r2 = r10
	call pa_hw_accel_job_lean
	r0 = [sp+200]                   ; 4-byte Folded Reload
	r4 = r0 - r4
	call plat_ticks24
	r0 += r4
	[sp+200] = r0                   ; 4-byte Folded Spill
.LBB20_75:                              ;   in Loop: Header=BB20_72 Depth=2
	if (r6 == 0) goto .LBB20_83
; BB#76:                                ;   in Loop: Header=BB20_72 Depth=2
	ifs (r5 < 1) goto .LBB20_79
; BB#77:                                ;   in Loop: Header=BB20_72 Depth=2
	if (r13 == 0) goto .LBB20_81
; BB#78:                                ;   in Loop: Header=BB20_72 Depth=2
	r0 = sp + 964
	r5 = [++r0=r7]
	r9 = [r0+4]
	[sp+8] = r9
	[sp+4] = r5
	r0 = sp + 732
	r0 = r12  #
	[sp] = r0
	r1 = 29383168
	r3_r2 = d[sp+168]               ; 4-byte Folded Reload
	call wn_P
	goto .LBB20_80
.LBB20_79:                              ;   in Loop: Header=BB20_72 Depth=2
	r0 = sp + 964
	r0 += r7
	{r9, r5} = [r0+]
	if (r13 == 0) goto .LBB20_82
.LBB20_80:                              ;   in Loop: Header=BB20_72 Depth=2
	r1 = r9 - r5
	r0 = sp + 788
	call wn_g_run
	r5 = [sp+196]                   ; 4-byte Folded Reload
	goto .LBB20_83
.LBB20_81:                              ;   in Loop: Header=BB20_72 Depth=2
	r4 = sp + 964
	call plat_ticks24
	r5 = [++r4=r7]
	r9 = [r4+4]
	[sp+8] = r9
	r4 = r0  #
	[sp+4] = r5
	r0 = sp + 732
	r0 = r12  #
	[sp] = r0
	r1 = 29383168
	r3_r2 = d[sp+168]               ; 4-byte Folded Reload
	call wn_P
	r0 = [sp+164]                   ; 4-byte Folded Reload
	r4 = r0 - r4
	call plat_ticks24
	r0 += r4
	[sp+164] = r0                   ; 4-byte Folded Spill
.LBB20_82:                              ;   in Loop: Header=BB20_72 Depth=2
	r4 = r9 - r5
	call plat_ticks24
	r5 = r0
	r0 = sp + 788
	r1 = r4
	call wn_g_run
	call plat_ticks24
	r0 = r0 - r5  #
	r1 = [sp+192]                   ; 4-byte Folded Reload
	r1 += r0
	[sp+192] = r1                   ; 4-byte Folded Spill
	r5 = [sp+196]                   ; 4-byte Folded Reload
	if (r5 != 1) {
		r0 = 0
	}
	r1 = [sp+180]                   ; 4-byte Folded Reload
	r1 += r0
	[sp+180] = r1                   ; 4-byte Folded Spill
.LBB20_83:                              ;   in Loop: Header=BB20_72 Depth=2
	r7 += 4
	r10 += r11
	if (r7 != 12) goto .LBB20_72
; BB#84:                                ;   in Loop: Header=BB20_66 Depth=1
	r11 = g_pa_wn_specs
	r0 = [sp+144]                   ; 4-byte Folded Reload
	r0 += r11  #
	r6 = [sp+112]                   ; 4-byte Folded Reload
	r1 = r14 + -1  #
	r0 = h[r0+2] (u)
	r10 = .L_MergedGlobals
	r2 = r10 + 36
	r3 = [r2+r5<<2]
	r0 *= r1
	r0 += 64
	r1 = r3 * 0xC  #
	[r6+4] = r0
	r15 = [sp+188]                  ; 4-byte Folded Reload
	r1 += r15
	r1 = r1 + 7508
	[sp+760] = r1
	r12 = [sp+160]                  ; 4-byte Folded Reload
	r1 = h[r12+r5<<1] (u)
	r1 += 1
	r3 = 0
	if (r1 >= r0) {
		r3 = r0
	}
	r0 = r1 - r3
	r14 = r5 + 1  #
	[r6+8] = r0
	ifs (r14 >= 23) goto .LBB20_87
; BB#85:                                ;   in Loop: Header=BB20_66 Depth=1
	r0 = r14 << 3
	r1 = r0 + r11
	r10 = 0  #
	r0 = b[r1+0] (u)
	r3 = r0 + -1  #
	r1 = h[r1+2] (u)
	r4 = r3 * r1
	r3 = [r2+r14<<2]
	r2 = r4 + 64
	r3 = r3 * 0xC  #
	[r6+16] = r2
	r9 = h[r12+r14<<1] (u)
	r3 += r15
	r3 = r3 + 7508
	r7 = r9 + 1  #
	[r6+12] = r3
	r5 = 0
	if (r7 >= r2) {
		r5 = r2
	}
	r5 = r7 - r5
	[r6+20] = r5
	r5 = 32
	r5 = r0 * 0x3  #
	[r6+24] = r5
	r6 = [sp+108]                   ; 4-byte Folded Reload
	[r6+16] = r0
	r5 += 2
	[r6+20] = r5
	[r6+12] = r2
	[sp+876] = r3
	r3 = 29368320
	[r6+4] = r3
	[r6+24] = r10
	r3 = [sp+72]                    ; 4-byte Folded Reload
	r3 = -64  #
	[r6+8] = r3
	r5 = r3 - r4
	r3 = 0
	if (r7 >= r2) {
		r3 = r5
	}
	r5 = r7 + r2
	r5 += r3
	r4 = r5 - r4
	if (r4 >= r2) {
		r10 = r2
	}
	r4 = r4 - r10  #
	r5 = [sp+108]                   ; 4-byte Folded Reload
	r3 += r1  #
	[r5+28] = r4
	r3 += r9
	r3 += 65
	r4 = 8
	r10 = .L_MergedGlobals
.LBB20_86:                              ;   Parent Loop BB20_66 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	r5 = 0
	if (r3 >= r2) {
		r5 = r2
	}
	r5 = r3 - r5
	r7 = sp + 876
	[r7+r4<<2] = r5
	r5 = r4 + -6
	r4 += 1
	r3 += r1
	if (r5 < r0) goto .LBB20_86
	goto .LBB20_88
.LBB20_87:                              ;   in Loop: Header=BB20_66 Depth=1
	r0 = 0
	[r6+16] = r0
	[r6+12] = r0
	[r6+20] = r0
	[r6+24] = r0
.LBB20_88:                              ;   in Loop: Header=BB20_66 Depth=1
	r0 = [sp+140]                   ; 4-byte Folded Reload
	if (r0 != 0) goto .LBB20_99
; BB#89:                                ;   in Loop: Header=BB20_66 Depth=1
	r0 = [sp+136]                   ; 4-byte Folded Reload
	r0 += 3
	[sp+136] = r0                   ; 4-byte Folded Spill
	call pa_hw_accel_wait_lean
	r5 = 0
	r15 = [sp+172]                  ; 4-byte Folded Reload
	r8 = [sp+192]                   ; 4-byte Folded Reload
	r9 = [sp+164]                   ; 4-byte Folded Reload
	r6 = [sp+148]                   ; 4-byte Folded Reload
.LBB20_90:                              ;   Parent Loop BB20_66 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	if (r13 == 0) goto .LBB20_92
; BB#91:                                ;   in Loop: Header=BB20_90 Depth=2
	r0 = r5 << 2
	r1 = sp + 980
	r0 = [++r1=r0]
	r3 = r15  #
	r1 = [r1+4] 
	d[sp+4] = r1_r0
	r0 = sp + 760
	r2 = 0
	r0 = r6  #
	[sp] = r0
	r1 = 29382656
	call wn_P
	goto .LBB20_93
.LBB20_92:                              ;   in Loop: Header=BB20_90 Depth=2
	r4 = r5 << 2
	r7 = sp + 980
	call plat_ticks24
	r1 = [++r7=r4]
	r4 = r0  #
	r2 = [r7+4] 
	r3 = r15  #
	[sp+8] = r2
	r0 = sp + 760
	r2 = 0
	d[sp] = r1_r0
	r0 = r6
	r1 = 29382656
	call wn_P
	r4 = r9 - r4
	call plat_ticks24
	r9 = r4 + r0
.LBB20_93:                              ;   in Loop: Header=BB20_90 Depth=2
	ifs (r14 >= 23) goto .LBB20_96
; BB#94:                                ;   in Loop: Header=BB20_90 Depth=2
	if (r13 == 0) goto .LBB20_97
; BB#95:                                ;   in Loop: Header=BB20_90 Depth=2
	r0 = sp + 980
	r1 = [r0+r5<<2]
	r5 += 1
	r0 = [r0+r5<<2]
	r1 = r0 - r1
	r0 = sp + 876
	call wn_g_run
	if (r5 != 3) goto .LBB20_90
	goto .LBB20_98
.LBB20_96:                              ;   in Loop: Header=BB20_90 Depth=2
	r5 += 1
	if (r5 != 3) goto .LBB20_90
	goto .LBB20_98
.LBB20_97:                              ;   in Loop: Header=BB20_90 Depth=2
	call plat_ticks24
	r1 = sp + 980
	r2 = [r1+r5<<2]
	r5 += 1
	r1 = [r1+r5<<2]
	r4 = r0
	r1 = r1 - r2
	r0 = sp + 876
	call wn_g_run
	r4 = r8 - r4
	call plat_ticks24
	r8 = r4 + r0
	if (r5 != 3) goto .LBB20_90
.LBB20_98:                              ;   in Loop: Header=BB20_66 Depth=1
	[sp+164] = r9                   ; 4-byte Folded Spill
	[sp+192] = r8                   ; 4-byte Folded Spill
	ifs (r14 < 23) goto .LBB20_66
	goto .LBB20_109
.LBB20_99:                              ;   in Loop: Header=BB20_66 Depth=1
	r0 = [sp+44]                    ; 4-byte Folded Reload
	r1 = [sp+156]                   ; 4-byte Folded Reload
	r0 |= r1
	r11 = r0 | 0x18000000
	r7 = 0
.LBB20_100:                             ;   Parent Loop BB20_66 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	r4 = r7 << 2
	r0 = 29383168
	r9 = r4 + r0  #
	r0 = [sp+156]                   ; 4-byte Folded Reload
	r0 *= r7  #
	r1 = [sp+152]                   ; 4-byte Folded Reload
	r0 = r0 << 2
	r5 = r1 + r0
	if (r13 == 0) goto .LBB20_102
; BB#101:                               ;   in Loop: Header=BB20_100 Depth=2
	[sp+4] = r8
	r0 = 2721
	[sp] = r11
	r1 = 29376512
	r2 = r5
	r3 = r9
	call pa_hw_accel_job_lean
	r0 = sp + 980
	r1 = [++r0=r4]
	r0 = [r0+4] 
	[sp+8] = r0
	r0 = sp + 760
	r2 = 0
	d[sp] = r1_r0
	r0 = [sp+148]                   ; 4-byte Folded Reload
	r1 = 29382656
	r3 = [sp+172]                   ; 4-byte Folded Reload
	call wn_P
	goto .LBB20_103
.LBB20_102:                             ;   in Loop: Header=BB20_100 Depth=2
	call plat_ticks24
	r10 = r0
	[sp+4] = r8
	r0 = 2721
	[sp] = r11
	r1 = 29376512
	r2 = r5
	r3 = r9
	call pa_hw_accel_job_lean
	r5 = sp + 980
	call plat_ticks24
	r9 = r0
	call plat_ticks24
	r1 = [++r5=r4]
	r5 = r0  #
	r2 = [r5+4] 
	[sp+8] = r2
	r0 = sp + 760
	r2 = 0
	d[sp] = r1_r0
	r0 = [sp+148]                   ; 4-byte Folded Reload
	r1 = 29382656
	r3 = [sp+172]                   ; 4-byte Folded Reload
	call wn_P
	r0 = [sp+200]                   ; 4-byte Folded Reload
	r0 = r0 - r10
	r0 += r9
	[sp+200] = r0                   ; 4-byte Folded Spill
	call plat_ticks24
	r0 = r0 - r5  #
	r1 = [sp+164]                   ; 4-byte Folded Reload
	r1 += r0
	[sp+164] = r1                   ; 4-byte Folded Spill
	r1 = [sp+196]                   ; 4-byte Folded Reload
	if (r1 != 1) {
		r0 = 0
	}
	r1 = [sp+84]                    ; 4-byte Folded Reload
	r1 += r0
	[sp+84] = r1                    ; 4-byte Folded Spill
	r10 = .L_MergedGlobals
.LBB20_103:                             ;   in Loop: Header=BB20_100 Depth=2
	ifs (r14 >= 23) goto .LBB20_106
; BB#104:                               ;   in Loop: Header=BB20_100 Depth=2
	if (r13 == 0) goto .LBB20_107
; BB#105:                               ;   in Loop: Header=BB20_100 Depth=2
	r0 = sp + 980
	r1 = [r0+r7<<2]
	r7 += 1
	r0 = [r0+r7<<2]
	r1 = r0 - r1
	r0 = sp + 876
	call wn_g_run
	if (r7 != 3) goto .LBB20_100
	goto .LBB20_108
.LBB20_106:                             ;   in Loop: Header=BB20_100 Depth=2
	r7 += 1
	if (r7 != 3) goto .LBB20_100
	goto .LBB20_108
.LBB20_107:                             ;   in Loop: Header=BB20_100 Depth=2
	call plat_ticks24
	r1 = sp + 980
	r2 = [r1+r7<<2]
	r7 += 1
	r1 = [r1+r7<<2]
	r4 = r0
	r1 = r1 - r2
	r0 = sp + 876
	call wn_g_run
	r0 = [sp+192]                   ; 4-byte Folded Reload
	r4 = r0 - r4
	call plat_ticks24
	r0 += r4
	[sp+192] = r0                   ; 4-byte Folded Spill
	if (r7 != 3) goto .LBB20_100
.LBB20_108:                             ;   in Loop: Header=BB20_66 Depth=1
	r0 = [sp+136]                   ; 4-byte Folded Reload
	r0 += 6
	[sp+136] = r0                   ; 4-byte Folded Spill
	r11 = g_pa_wn_specs
	ifs (r14 <= 22) goto .LBB20_66
	goto .LBB20_110
.LBB20_109:
	r15 = [sp+188]                  ; 4-byte Folded Reload
.LBB20_110:
	r9 = 402653232
	r0 = [sp+120]                   ; 4-byte Folded Reload
	r5 = r0 | r9
	if (r13 == 0) goto .LBB20_112
; BB#111:
	r0 = 4194316
	r0 = 2721  #
	[sp+4] = r0
	[sp] = r5
	r1 = [sp+56]                    ; 4-byte Folded Reload
	r3_r2 = d[sp+48]                ; 4-byte Folded Reload
	call pa_hw_accel_job_lean
	goto .LBB20_113
.LBB20_112:
	call plat_ticks24
	r4 = r0
	r0 = 4194316
	r0 = 2721  #
	[sp+4] = r0
	[sp] = r5
	r1 = [sp+56]                    ; 4-byte Folded Reload
	r3_r2 = d[sp+48]                ; 4-byte Folded Reload
	call pa_hw_accel_job_lean
	r0 = [sp+200]                   ; 4-byte Folded Reload
	r4 = r0 - r4
	call plat_ticks24
	r0 += r4
	[sp+200] = r0                   ; 4-byte Folded Spill
.LBB20_113:
	r14 = [sp+64]                   ; 4-byte Folded Reload
	r6 = [sp+168]                   ; 4-byte Folded Reload
	r0 = [sp+140]                   ; 4-byte Folded Reload
	if (r0 == 0) goto .LBB20_116
; BB#114:
	r0 = [r10+124]
	r1 = 1259  #
	r4 = [sp+68]                    ; 4-byte Folded Reload
	r0 = r0 * 0xC  #
	[r4+4] = r1
	r0 += r15
	r0 = r0 + 7508
	[sp+732] = r0
	r0 = [sp+100]                   ; 4-byte Folded Reload
	r1 = h[r0+0] (u)
	r0 = [r10+216]
	r2 = [sp+124]                   ; 4-byte Folded Reload
	r2 += r1
	r1 = 0
	r3 = 0
	if (r2 >= 1259) {
		r3 = 1259
	}
	r2 = r2 - r3
	[r4+8] = r2
	[r4+16] = r1
	[r4+12] = r1
	[r4+20] = r1
	r0 = r0 << 2
	r0 += r15  #
	[r4+24] = r1
	r1 = 110032
	r11 = r0 + r1
	if (r13 == 0) goto .LBB20_117
; BB#115:
	r0 = r11  #
	r4 = [sp+20]                    ; 4-byte Folded Reload
	[sp+8] = r4
	[sp+4] = r6
	r6 = sp + 732
	r8 = 29383168
	r1 = r8  #
	[sp] = r6
	r5 = [sp+172]                   ; 4-byte Folded Reload
	r3 = r5  #
	r2 = [sp+168]                   ; 4-byte Folded Reload
	call wn_P
	r0 = r11  #
	r7 = [sp+24]                    ; 4-byte Folded Reload
	r1 = r8  #
	[sp+8] = r7
	r3 = r5  #
	[sp+4] = r4
	[sp] = r6
	r2 = [sp+168]                   ; 4-byte Folded Reload
	call wn_P
	[sp+8] = r14
	d[sp] = r7_r6
	r0 = r11  #
	r6 = [sp+168]                   ; 4-byte Folded Reload
	r1 = r8
	r2 = r6
	r3 = r5
	call wn_P
	r0 = 4194316
	[sp+4] = r0
	r0 = [sp+44]                    ; 4-byte Folded Reload
	r2 = r0 | r9  #
	r3 = [sp+28]                    ; 4-byte Folded Reload
	r0 = r6 * 0xC  #
	[sp] = r2
	r0 += r15  #
	r4 = [sp+48]                    ; 4-byte Folded Reload
	r1 = 117884
	r1 += r0
	r0 = 119088
	r3 += r0
	r0 = 2721
	r2 = r4
	call pa_hw_accel_job_lean
	goto .LBB20_118
.LBB20_116:
	r0 = [sp+136]                   ; 4-byte Folded Reload
	r0 += 1
	[sp+196] = r0                   ; 4-byte Folded Spill
	call pa_hw_accel_wait_lean
	r7 = [sp+96]                    ; 4-byte Folded Reload
	r8 = [sp+100]                   ; 4-byte Folded Reload
	r9 = [sp+92]                    ; 4-byte Folded Reload
	r11 = [sp+88]                   ; 4-byte Folded Reload
	r4 = [sp+48]                    ; 4-byte Folded Reload
	goto .LBB20_119
.LBB20_117:
	call plat_ticks24
	r0 = r11  #
	[sp+196] = r0                   ; 4-byte Folded Spill
	r4 = [sp+20]                    ; 4-byte Folded Reload
	[sp+8] = r4
	[sp+4] = r6
	r5 = sp + 732
	r6 = 29383168
	r1 = r6  #
	[sp] = r5
	r9 = [sp+172]                   ; 4-byte Folded Reload
	r3 = r9  #
	r2 = [sp+168]                   ; 4-byte Folded Reload
	call wn_P
	call plat_ticks24
	[sp+156] = r0                   ; 4-byte Folded Spill
	call plat_ticks24
	r10 = r0  #
	r7 = [sp+24]                    ; 4-byte Folded Reload
	r0 = r11  #
	[sp+8] = r7
	r1 = r6  #
	[sp+4] = r4
	r3 = r9  #
	[sp] = r5
	r2 = [sp+168]                   ; 4-byte Folded Reload
	call wn_P
	call plat_ticks24
	r12 = r0
	call plat_ticks24
	[sp+8] = r14
	r8 = r0  #
	[sp+4] = r7
	r0 = r11  #
	[sp] = r5
	r1 = r6  #
	r6 = [sp+168]                   ; 4-byte Folded Reload
	r2 = r6
	r3 = r9
	call wn_P
	call plat_ticks24
	r4 = r0
	call plat_ticks24
	r5 = r0
	r0 = 4194316
	[sp+4] = r0
	r0 = [sp+44]                    ; 4-byte Folded Reload
	r1 = 402653232
	r2 = r0 | r1  #
	r3 = [sp+28]                    ; 4-byte Folded Reload
	r0 = r6 * 0xC  #
	[sp] = r2
	r0 += r15  #
	r7 = [sp+48]                    ; 4-byte Folded Reload
	r1 = 117884
	r1 += r0
	r0 = 119088
	r3 += r0
	r0 = 2721
	r2 = r7
	call pa_hw_accel_job_lean
	r0 = [sp+164]                   ; 4-byte Folded Reload
	r1 = [sp+196]                   ; 4-byte Folded Reload
	r0 = r0 - r1  #
	r1 = [sp+156]                   ; 4-byte Folded Reload
	r0 += r1
	r0 = r0 - r10
	r10 = .L_MergedGlobals
	r0 += r12
	r12 = [sp+160]                  ; 4-byte Folded Reload
	r0 = r0 - r8
	r0 += r4
	[sp+164] = r0                   ; 4-byte Folded Spill
	r0 = [sp+200]                   ; 4-byte Folded Reload
	r4 = r0 - r5
	call plat_ticks24
	r0 += r4
	r4 = r7  #
	[sp+200] = r0                   ; 4-byte Folded Spill
.LBB20_118:
	r0 = [sp+136]                   ; 4-byte Folded Reload
	r0 += 2
	[sp+196] = r0                   ; 4-byte Folded Spill
	r7 = [sp+96]                    ; 4-byte Folded Reload
	r8 = [sp+100]                   ; 4-byte Folded Reload
	r9 = [sp+92]                    ; 4-byte Folded Reload
	r11 = [sp+88]                   ; 4-byte Folded Reload
.LBB20_119:
	r5 = [r4+192]
	[r10+4] = r5
	r4 = [sp+52]                    ; 4-byte Folded Reload
	if (r6 == 0) goto .LBB20_123
; BB#120:
	r0 = [sp+60]                    ; 4-byte Folded Reload
	if (r0 == 0) goto .LBB20_123
; BB#121:
	r0 = 0
	r1 = r0 + 0x35000000  #
	r2 = [sp+60]                    ; 4-byte Folded Reload
.LBB20_122:                             ; =>This Inner Loop Header: Depth=1
	r3 = [r4+r0<<2]
	r3 += r5
	r3 = itof(r3)
	r3 = r3 * r1 (f)
	r0 += 1
	[r2++=4] = r3
	if (r0 < r6) goto .LBB20_122
.LBB20_123:
	r0 = 117884
	r1 = r14 * 0xC  #
	r3 = [sp+56]                    ; 4-byte Folded Reload
	r1 += r15
	r0 += r1
	r1 = 0
.LBB20_124:                             ; =>This Inner Loop Header: Depth=1
	r2 = [r0+r1<<2]
	[r3+r1<<2] = r2
	r1 += 1
	if (r1 != 45) goto .LBB20_124
; BB#125:
	r0 = 0
.LBB20_126:                             ; =>This Inner Loop Header: Depth=1
	r1 = h[r12+r0<<1] (u)
	r1 += r14  #
	r6 = [sp+132]                   ; 4-byte Folded Reload
	r2 = h[r6+2] (u)
	r3 = b[r6++=8] (u)
	r3 += -1
	r2 *= r3  #
	[sp+132] = r6                   ; 4-byte Folded Spill
	r3 = r2 + 64
	r2 = 0
	if (r1 >= r3) {
		r2 = r3
	}
	r1 = r1 - r2
	h[r12+r0<<1] = r1
	r0 += 1
	if (r0 != 23) goto .LBB20_126
; BB#127:
	call pa_hw_accel_wait_lean
	r4 = [sp+124]                   ; 4-byte Folded Reload
	if (r14 < 33) goto .LBB20_131
; BB#128:
	r0 = [sp+60]                    ; 4-byte Folded Reload
	if (r0 == 0) goto .LBB20_131
; BB#129:
	r0 = [sp+72]                    ; 4-byte Folded Reload
	r0 = r0 + 252  #
	r1 = [sp+60]                    ; 4-byte Folded Reload
	r2 = [sp+16]                    ; 4-byte Folded Reload
	r1 += r2
	r2 = 0x35000000
.LBB20_130:                             ; =>This Inner Loop Header: Depth=1
	r3 = r4
	r6 = [r0+r3<<2]
	r6 += r5
	r6 = itof(r6)
	r6 = r6 * r2 (f)
	[r1++=4] = r6
	r4 = r3 + 1
	if (r3 < r14) goto .LBB20_130
.LBB20_131:
	if (r13 == 0) goto .LBB20_133
; BB#132:
	call plat_ticks24
	r1 = [sp+36]                    ; 4-byte Folded Reload
	r2 = r0 - r1
	r0 = 5
	goto .LBB20_134
.LBB20_133:
	r0 = [sp+200]                   ; 4-byte Folded Reload
	[r7+4] = r0
	r0 = [sp+164]                   ; 4-byte Folded Reload
	[r7+12] = r0
	r0 = [sp+192]                   ; 4-byte Folded Reload
	[r7+16] = r0
	call plat_ticks24
	r1 = [sp+36]                    ; 4-byte Folded Reload
	r0 = r0 - r1
	[r7+28] = r0
	r0 = [sp+180]                   ; 4-byte Folded Reload
	[r7+32] = r0
	r0 = 9
	r2 = [sp+84]                    ; 4-byte Folded Reload
.LBB20_134:
	r1 = r7 + 4
	[r1+r0<<2] = r2
	r0 = [sp+196]                   ; 4-byte Folded Reload
	[r7+8] = r0
	r0 = [r7+24] 
	if (r0 == 0) goto .LBB20_141
; BB#135:
	call plat_ticks24
	r0 = r0 - r11
	r1 = 35001
	[r7+24] = r0
	if (r0 < r1) goto .LBB20_138
; BB#136:
	r0 = b[r8+4] (u)
	r0 += 1
	r1 = r0.b0 (u)
	b[r8+4] = r0
	if (r1 < 16) goto .LBB20_141
; BB#137:
	r0 = 1
	b[r8+3] = r0
	r0 = 0
	b[r15+2] = r0
	goto .LBB20_141
.LBB20_138:
	r0 = r0 >> 6
	if (r0 > 524) goto .LBB20_141
; BB#139:
	r0 = b[r8+4] (u)
	if (r0 == 0) goto .LBB20_141
; BB#140:
	r0 += -1
	b[r8+4] = r0
.LBB20_141:
	r0 = [sp+40]                    ; 4-byte Folded Reload
	if (r0 == 0) goto .LBB20_143
; BB#142:
	r0 = [sp+184]                   ; 4-byte Folded Reload
	[r10+8] = r0
	r0 = 1
	b[r10+2] = r0
	r6 = [sp+32]                    ; 4-byte Folded Reload
	goto .LBB20_39
.LBB20_143:
	r0 = sp + 212
	r3 = r14  #
	[sp+8] = r0
	r0 = sp + 216
	[sp+4] = r0
	r0 = [sp+184]                   ; 4-byte Folded Reload
	r0 = r15  #
	[sp] = r0
	r1 = [sp+60]                    ; 4-byte Folded Reload
	r2 = [sp+176]                   ; 4-byte Folded Reload
	call wn_post_block
	r10 = .L_MergedGlobals
	r1 = [r10+12]
	r0 = [sp+216]
	iff (r0 > r1) {
		[r10+12] = r0
	}
	r1 = [r10+16]
	r0 = [sp+212]
	r6 = [sp+32]                    ; 4-byte Folded Reload
	r9 = [sp+92]                    ; 4-byte Folded Reload
	r11 = [sp+88]                   ; 4-byte Folded Reload
	iff (r0 > r1) {
		[r10+16] = r0
	}
	r14 = [sp+64]                   ; 4-byte Folded Reload
	r15 = [sp+188]                  ; 4-byte Folded Reload
	r7 = [sp+96]                    ; 4-byte Folded Reload
	r8 = [sp+100]                   ; 4-byte Folded Reload
	goto .LBB20_39
.Lfunc_end20:
	.size	pa_wavenet_process_block_q21, .Lfunc_end20-pa_wavenet_process_block_q21

	.type	g_pa_wn_specs,@object   ; @g_pa_wn_specs
	.section	.rodata,"a",@progbits
	.globl	g_pa_wn_specs
	.p2align	1
g_pa_wn_specs:
	.byte	6                       ; 0x6
	.zero	1
	.short	1                       ; 0x1
	.short	6                       ; 0x6
	.short	0                       ; 0x0
	.byte	6                       ; 0x6
	.zero	1
	.short	3                       ; 0x3
	.short	16                      ; 0x10
	.short	6                       ; 0x6
	.byte	6                       ; 0x6
	.zero	1
	.short	7                       ; 0x7
	.short	36                      ; 0x24
	.short	22                      ; 0x16
	.byte	6                       ; 0x6
	.zero	1
	.short	17                      ; 0x11
	.short	86                      ; 0x56
	.short	58                      ; 0x3a
	.byte	6                       ; 0x6
	.zero	1
	.short	41                      ; 0x29
	.short	206                     ; 0xce
	.short	144                     ; 0x90
	.byte	6                       ; 0x6
	.zero	1
	.short	101                     ; 0x65
	.short	506                     ; 0x1fa
	.short	350                     ; 0x15e
	.byte	6                       ; 0x6
	.zero	1
	.short	239                     ; 0xef
	.short	1196                    ; 0x4ac
	.short	856                     ; 0x358
	.byte	6                       ; 0x6
	.zero	1
	.short	1                       ; 0x1
	.short	6                       ; 0x6
	.short	2052                    ; 0x804
	.byte	6                       ; 0x6
	.zero	1
	.short	3                       ; 0x3
	.short	16                      ; 0x10
	.short	2058                    ; 0x80a
	.byte	6                       ; 0x6
	.zero	1
	.short	7                       ; 0x7
	.short	36                      ; 0x24
	.short	2074                    ; 0x81a
	.byte	6                       ; 0x6
	.zero	1
	.short	17                      ; 0x11
	.short	86                      ; 0x56
	.short	2110                    ; 0x83e
	.byte	6                       ; 0x6
	.zero	1
	.short	41                      ; 0x29
	.short	206                     ; 0xce
	.short	2196                    ; 0x894
	.byte	6                       ; 0x6
	.zero	1
	.short	101                     ; 0x65
	.short	506                     ; 0x1fa
	.short	2402                    ; 0x962
	.byte	6                       ; 0x6
	.zero	1
	.short	239                     ; 0xef
	.short	1196                    ; 0x4ac
	.short	2908                    ; 0xb5c
	.byte	15                      ; 0xf
	.zero	1
	.short	1                       ; 0x1
	.short	15                      ; 0xf
	.short	4104                    ; 0x1008
	.byte	15                      ; 0xf
	.zero	1
	.short	13                      ; 0xd
	.short	183                     ; 0xb7
	.short	4119                    ; 0x1017
	.byte	6                       ; 0x6
	.zero	1
	.short	1                       ; 0x1
	.short	6                       ; 0x6
	.short	4302                    ; 0x10ce
	.byte	6                       ; 0x6
	.zero	1
	.short	3                       ; 0x3
	.short	16                      ; 0x10
	.short	4308                    ; 0x10d4
	.byte	6                       ; 0x6
	.zero	1
	.short	7                       ; 0x7
	.short	36                      ; 0x24
	.short	4324                    ; 0x10e4
	.byte	6                       ; 0x6
	.zero	1
	.short	17                      ; 0x11
	.short	86                      ; 0x56
	.short	4360                    ; 0x1108
	.byte	6                       ; 0x6
	.zero	1
	.short	41                      ; 0x29
	.short	206                     ; 0xce
	.short	4446                    ; 0x115e
	.byte	6                       ; 0x6
	.zero	1
	.short	101                     ; 0x65
	.short	506                     ; 0x1fa
	.short	4652                    ; 0x122c
	.byte	6                       ; 0x6
	.zero	1
	.short	239                     ; 0xef
	.short	1196                    ; 0x4ac
	.short	5158                    ; 0x1426
	.size	g_pa_wn_specs, 184

	.type	.L.str,@object          ; @.str
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str:
	.asciz	"Clean"
	.size	.L.str, 6

	.type	k_voice_names,@object   ; @k_voice_names
	.section	.rodata,"a",@progbits
	.p2align	2
k_voice_names:
	.long	.L.str
	.long	.L.str.1
	.long	.L.str.2
	.long	.L.str.3
	.long	.L.str.4
	.size	k_voice_names, 20

	.type	g_pa_wn_inline,@object  ; @g_pa_wn_inline
	.comm	g_pa_wn_inline,1,1
	.type	g_pa_wn_lean,@object    ; @g_pa_wn_lean
	.bss
	.weak	g_pa_wn_lean
g_pa_wn_lean:
	.byte	0                       ; 0x0
	.size	g_pa_wn_lean, 1

	.type	.L.str.1,@object        ; @.str.1
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str.1:
	.asciz	"Crunch"
	.size	.L.str.1, 7

	.type	.L.str.2,@object        ; @.str.2
.L.str.2:
	.asciz	"High Gain"
	.size	.L.str.2, 10

	.type	.L.str.3,@object        ; @.str.3
.L.str.3:
	.asciz	"Fuzz"
	.size	.L.str.3, 5

	.type	.L.str.4,@object        ; @.str.4
.L.str.4:
	.asciz	"Bass"
	.size	.L.str.4, 5

	.type	k_hpf_profiles,@object  ; @k_hpf_profiles
	.section	.rodata,"a",@progbits
	.p2align	2
k_hpf_profiles:
	.long	1065319445              ; float 0.997987091
	.long	3221191701              ; float -1.99597418
	.long	1065319445              ; float 0.997987091
	.long	3221191667              ; float -1.99597013
	.long	1065285743              ; float 0.995978295
	.long	1065302586              ; float 0.996982216
	.long	3221174842              ; float -1.99396443
	.long	1065302586              ; float 0.996982216
	.long	3221174765              ; float -1.99395525
	.long	1065252108              ; float 0.993973493
	.long	1065277328              ; float 0.995476722
	.long	3221149584              ; float -1.99095345
	.long	1065277328              ; float 0.995476722
	.long	3221149412              ; float -1.99093294
	.long	1065201783              ; float 0.990973889
	.long	1065294162              ; float 0.996480107
	.long	3221166418              ; float -1.99296021
	.long	1065294162              ; float 0.996480107
	.long	3221166314              ; float -1.99294782
	.long	1065235316              ; float 0.992972612
	.long	1065319445              ; float 0.997987091
	.long	3221191701              ; float -1.99597418
	.long	1065319445              ; float 0.997987091
	.long	3221191667              ; float -1.99597013
	.long	1065285743              ; float 0.995978295
	.size	k_hpf_profiles, 100

	.type	k_lpf_profiles,@object  ; @k_lpf_profiles
	.p2align	2
k_lpf_profiles:
	.long	1060289834              ; float 0.698198915
	.long	1068678442              ; float 1.39639783
	.long	1060289834              ; float 0.698198915
	.long	1067896136              ; float 1.30313969
	.long	1056617520              ; float 0.489655972
	.long	1057587694              ; float 0.53713882
	.long	1065976302              ; float 1.07427764
	.long	1057587694              ; float 0.53713882
	.long	1062788637              ; float 0.847139179
	.long	1050301237              ; float 0.301416069
	.long	1041879177              ; float 0.150209561
	.long	1050267785              ; float 0.300419122
	.long	1041879177              ; float 0.150209561
	.long	3206886651              ; float -0.645339667
	.long	1048319497              ; float 0.246177807
	.long	1041008180              ; float 0.137230694
	.long	1049396788              ; float 0.274461389
	.long	1041008180              ; float 0.137230694
	.long	3208044331              ; float -0.714342772
	.long	1049021123              ; float 0.263265699
	.long	1054585086              ; float 0.429084718
	.long	1062973694              ; float 0.858169436
	.long	1054585086              ; float 0.429084718
	.long	1056967752              ; float 0.500187397
	.long	1046304465              ; float 0.216151491
	.size	k_lpf_profiles, 100

	.type	.L_MergedGlobals,@object ; @_MergedGlobals
	.local	.L_MergedGlobals
	.comm	.L_MergedGlobals,220,16
	.weak	wn_asm_mix
	.weak	wn_asm_head

	.set s_wn_defer_ok, .L_MergedGlobals
	.size	s_wn_defer_ok, 1
	.set s_wn_pre_valid, .L_MergedGlobals+1
	.size	s_wn_pre_valid, 1
	.set s_wn_post_pending, .L_MergedGlobals+2
	.size	s_wn_post_pending, 1
	.set s_wn_asm, .L_MergedGlobals+3
	.size	s_wn_asm, 1
	.set s_wn_hb, .L_MergedGlobals+4
	.size	s_wn_hb, 4
	.set s_wn_post_makeup, .L_MergedGlobals+8
	.size	s_wn_post_makeup, 4
	.set wn_dg_out, .L_MergedGlobals+12
	.size	wn_dg_out, 4
	.set wn_dg_in, .L_MergedGlobals+16
	.size	wn_dg_in, 4
	.set wn_dg_nan, .L_MergedGlobals+20
	.size	wn_dg_nan, 4
	.set pa_wavenet_process_block_q21.dg_init, .L_MergedGlobals+24
	.size	pa_wavenet_process_block_q21.dg_init, 4
	.set wn_block_run.whw, .L_MergedGlobals+28
	.size	wn_block_run.whw, 4
	.set wn_block_run.lay_ok, .L_MergedGlobals+32
	.size	wn_block_run.lay_ok, 4
	.set wn_block_run.colb, .L_MergedGlobals+36
	.size	wn_block_run.colb, 92
	.set wn_block_run.wo, .L_MergedGlobals+128
	.size	wn_block_run.wo, 92
