/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 The Onyx Authors (@28spawn) */
/* custom-fw/bench/bench.ld - the WaveNet benchmark, linked to run from SRAM at 0x01C02000,
 * the address the JieLi ROM download mode loads its own flash loaders to (jl-uboot-tool,
 * data/usb-loaders.yaml, wl82). Nothing is placed in flash.
 *
 * Layout: [ entry | text | rodata | data (cfg + results) ] is the uploaded image;
 *         [ bss | stack ] follow it and are not uploaded (bench_main zeroes the bss).
 */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x01C02000, LENGTH = 0x40000     /* 256 KB */
}
SECTIONS
{
    .text : {
        KEEP(*(.text.entry))
        *(.text*) *(.ram_text*) *(.rodata*)
        . = ALIGN(4);
    } > RAM
    .data : { . = ALIGN(4); *(.data*) *(.sdata*) . = ALIGN(4); _image_end = .; } > RAM
    .bss (NOLOAD) : { . = ALIGN(8); _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(8); _bss_end = .; } > RAM
    .stack (NOLOAD) : { . = ALIGN(8); bench_stack_bottom = .; . += 0x2000; bench_stack_top = .; } > RAM
    /DISCARD/ : { *(.comment) *(.note*) *(.pool*) }
}
ASSERT(bench_stack_top <= ORIGIN(RAM) + LENGTH(RAM), "the benchmark does not fit its 256 KB")
