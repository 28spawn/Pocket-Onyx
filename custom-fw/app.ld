/* custom-fw/app.ld — Pocket Amp memory map (QW-3, adapted from fm1-x0x/firmware/app.ld).
 *
 * The sizes are the DEVICE's, not the stock app's: the app partition in the flash is
 * 0x005000..0x0B4FFF, so the XIP region is 0xB0000 (720,896) bytes.  The stock's own
 * app is 0xB0874 bytes - 2,164 bytes MORE than the partition it lives in - so copying
 * that number here would let the linker build an image that runs into the next
 * partition.  The region is the limit that matters, and the ASSERTs below make the
 * memory map fail at link time if the regions ever overlap.
 */
MEMORY
{
    XIP     (rx)  : ORIGIN = 0x02000120, LENGTH = 0xB0000   /* the app partition */
    RAMTEXT (rwx) : ORIGIN = 0x01C00000, LENGTH = 0x400    /* core 1's entry stub; the UI's text buffer starts at 0x01C00400 (ui_5x7.c) */
    RAMFN   (rwx) : ORIGIN = 0x01C01000, LENGTH = 0x1000   /* core 0 code that runs from SRAM (PA_RAMFN): 4 KB between the UI strings and the amp's accelerator matrices at 0x01C02000 */
    RAM     (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL    (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x54000
    BLERAM  (rw)  : ORIGIN = 0x01C74100, LENGTH = 0x2400   /* the Bluetooth radio's zero-initialised state (6 KB), carved from the low end of the user stack: the stack never went below 5.5 KB deep of 24 KB (bench readout 2026-11-02) */
    FLASHFN (rwx) : ORIGIN = 0x01C76500, LENGTH = 0x800   /* the flash driver's RAM code (flash_ram.c, .flashfn): runs with the flash controller in command mode, so it cannot live in the flash; carved from the low end of the user stack (it was 17.9 KB, deepest use 5.5 KB) */
    NOINIT  (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(.text*) *(.rodata*) . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text.*)) . = ALIGN(4); } > RAMTEXT AT > XIP   /* (the bare name .ram_text goes to .ramfn below: the imported BLE radio driver puts its register burst there) */
    _rt_load = LOADADDR(.ram_text);
    .c1_text : { _c1_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1_end = .; } > RAMTEXT AT > XIP
    _c1_load = LOADADDR(.c1_text);
    _rt_end = _c1_end;
    /* the code that runs from SRAM: its own region, so the linker refuses a build that does not fit (it cannot reach the UI strings or the matrices) */
    .ramfn : { _rf_start = .; KEEP(*(.ramfn*)) KEEP(*(.ram_text)) . = ALIGN(4); _rf_end = .; } > RAMFN AT > XIP
    _rf_load = LOADADDR(.ramfn);
    .flashfn : { _ff_start = .; KEEP(*(.flashfn*)) . = ALIGN(4); _ff_end = .; } > FLASHFN AT > XIP
    _ff_load = LOADADDR(.flashfn);
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .blebss (NOLOAD) : { _bleb_start = .; *ble_radio.c.o(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); _bleb_end = .; } > BLERAM   /* the Bluetooth radio's zero-initialised state (this object's .bss); zeroed by pa_ble_radio_boot, entry.S only clears .bss. BEFORE .bss so it matches first */
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .pool (NOLOAD) : { _pool_start = .; *(.pool*) . = ALIGN(4); _pool_end = .; } > POOL
    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT
    _guard0 = 0x01C74000; _ustack_lo = ORIGIN(FLASHFN) + LENGTH(FLASHFN); _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}

/* The memory map must not overlap: the linker checks the region sizes, but not that the
 * regions are laid out next to each other in the right order.  These fail the LINK (not
 * the packer) if the map is ever changed carelessly. */
ASSERT(ORIGIN(RAMTEXT) + LENGTH(RAMTEXT) <= ORIGIN(RAM),
       "RAMTEXT overlaps RAM")
ASSERT(ORIGIN(RAM) + LENGTH(RAM) <= ORIGIN(POOL),
       "RAM overlaps POOL")
ASSERT(ORIGIN(POOL) + LENGTH(POOL) <= ORIGIN(BLERAM),
       "POOL overlaps the BLE state region")
ASSERT(ORIGIN(BLERAM) + LENGTH(BLERAM) <= ORIGIN(FLASHFN),
       "BLERAM overlaps the flash driver region")
ASSERT(_ustack_top - _ustack_lo >= 0x3000,
       "the user stack is smaller than 16 KB")
ASSERT(_ustack_top <= _sstack_lo,
       "the user stack overlaps the supervisor stack")
ASSERT(_sstack_top <= ORIGIN(NOINIT),
       "the supervisor stack overlaps NOINIT")
ASSERT(ORIGIN(NOINIT) + LENGTH(NOINIT) <= 0x01C80000,
       "NOINIT runs past the end of the SRAM")
