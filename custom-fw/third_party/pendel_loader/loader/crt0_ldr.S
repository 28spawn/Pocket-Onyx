/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca update loader entry. The SPL decompresses the loader to its load
 * address (0x01C0A800) and calls its first byte. Interrupts off,
 * our own stacks, clear .bss, then C. Never returns. */
#ifndef OTA_LOADER_MARK
#define OTA_LOADER_MARK "FELUCCA-LOADER-1"
#endif
	.section .text.crt0,"ax",@progbits
	.globl _start
_start:
	goto 1f
	.ascii OTA_LOADER_MARK	/* 16 bytes: the firmware recognises its loader in a package by this (ota.c) */
	.balign 2
1:	cli
	sp = _stack_top
	ssp = _sstack_top
	call ldr_cstart
	goto .
