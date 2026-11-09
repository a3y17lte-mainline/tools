ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { *(.head.text) *(.text.startup*) *(.text*) *(.rodata*) }
  .data : { *(.data*) *(.bss*) *(COMMON) }
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
