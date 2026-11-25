/* OPEN MPK4 application linker script: AT32F405 (256 KiB flash, 96 KiB SRAM) behind the Akai bootloader.
 * 0x08000000-0x08005FFF  Akai bootloader            (copied verbatim by mkimage, never written by us)
 * 0x08006000-0x0800BFFF  staged bootloader copy      (copied verbatim by mkimage)
 * 0x0800C000-0x0802BFFF  OPEN MPK4 application       (this script; the bootloader jumps to *(0x0800C004))
 * 0x0802C000-0x080387FF  STORAGE: our slots (storage.c): patterns, settings, scratch. Inside the range the bootloader
 *                        erases on every update (C1: 0x6000-0x3EFFF) and writes only with the image's own bytes
 *                        (erased: mkimage ships it 0xFF), so an update starts the slots empty
 * 0x08038800-0x08038FFF  sector holding the app-valid bytes 0x38FFE/F (mkimage copies the packet from stock)
 * 0x08039000-0x0803EFFF  stock app-data pages, unused (erased)
 * 0x0803F000-0x0803FFFF  settings backup, settings page + trailer (serials, wheel calibration): never written by us
 * Where the line between the application and the storage sits is a choice, made here only: every update erases the
 * storage anyway, so moving it costs nothing more than any update does. 128 KiB of application leaves it ~50 % to
 * grow; 25 sectors of storage hold the settings and song, all 8 patterns at up to 2 sectors (~660 events) each at the
 * same time (any one of them up to the full 2000-event pool while the others are smaller), the spare sectors a full
 * pattern rewrite needs (the old copy stays until the new one is complete) and the 2-sector storage check.
 * The app is ordinary: .data/.bss/stack in SRAM, vector table in flash at 0x0800C000 (VTOR set by startup.c).
 * .ramfunc (code that runs while the flash is busy erasing or programming) is copied to SRAM with .data. */
MEMORY
{
  FLASH   (rx) : ORIGIN = 0x0800C000, LENGTH = 0x0802C000 - 0x0800C000
  STORAGE (r)  : ORIGIN = 0x0802C000, LENGTH = 0x08038800 - 0x0802C000
  RAM    (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
_stack_size = 0x1000;
PROVIDE(end = _ebss);

/* the only flash storage.c may erase or program; flash_op() refuses everything outside [start, end) */
__storage_start = ORIGIN(STORAGE);
__storage_end = ORIGIN(STORAGE) + LENGTH(STORAGE);
/* The rules the storage region obeys (tools/mpk4_layout.py storage_errors() checks the same ones on the image):
 * hardware facts from docs/hardware/update-protocol.md and the hardware map, section 9 */
__sector = 0x800;                                  /* erase unit */
__boot_end = 0x0800C000;                           /* bootloader + staged copy */
__valid_sector = 0x08038800;                       /* the sector holding the app-valid bytes 0x38FFE/F */
__protected = 0x0803F000;                          /* settings backup + settings page, to the end of flash */
__erase_lo = 0x08006000;                           /* C1 erases [0x6000, 0x3F000) on every update */
__erase_hi = 0x0803F000;
ASSERT(__storage_start % __sector == 0 && __storage_end % __sector == 0, "storage must be whole 2 KiB sectors")
ASSERT(__storage_end > __storage_start, "storage is empty")
ASSERT(__storage_start >= __boot_end, "storage overlaps the bootloader or its staged copy (0x0000-0xBFFF)")
ASSERT(__storage_end <= __valid_sector || __storage_start >= __valid_sector + __sector,
       "storage overlaps the sector holding the app-valid bytes 0x38FFE/F")
ASSERT(__storage_end <= __protected, "storage overlaps the settings backup / settings page 0x3F000-0x3FFFF")
ASSERT(__storage_start >= __erase_lo && __storage_end <= __erase_hi,
       "storage outside the range every update erases (C1, 0x6000-0x3EFFF)")
ASSERT(__storage_start >= ORIGIN(FLASH) + LENGTH(FLASH) || __storage_end <= ORIGIN(FLASH),
       "storage overlaps the application")
ASSERT(ORIGIN(FLASH) + LENGTH(FLASH) <= __valid_sector, "the application must end below the app-valid sector")

ENTRY(Reset_Handler)
SECTIONS
{
  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); } > FLASH
  .preinit_array : {
    . = ALIGN(4);
    __preinit_array_start = .;
    KEEP(*(.preinit_array))
    __preinit_array_end = .;
  } > FLASH
  .init_array : {
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : {
    . = ALIGN(4); _sdata = .;
    __ramfunc_start = .; *(.ramfunc*) . = ALIGN(4); __ramfunc_end = .;
    *(.data*) . = ALIGN(4); _edata = .;
  } > RAM AT > FLASH
  .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  ._stack (NOLOAD) : { . = ALIGN(8); . = . + _stack_size; } > RAM
  ASSERT(ADDR(.isr_vector) == 0x0800C000, "vector table must be at 0x0800C000")
  ASSERT(_ebss + _stack_size <= _estack, "RAM overflow")
  __app_end = LOADADDR(.data) + SIZEOF(.data);
  ASSERT(__app_end <= __storage_start || ORIGIN(FLASH) >= __storage_end, "the application's bytes reach the storage")
}
