/* Entry for C probes: arm64 Image header (stock A3 layout), stack, call main. */
	.section .head.text, "ax"
	.globl	_start
_start:
	b	code			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset */
	.quad	_end - _start		/* image_size */
	.quad	0			/* flags */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARMd"			/* magic */
	.long	0

code:
	ldr	x1, =0x50000000		/* stack in free DRAM, grows down */
	mov	sp, x1
	bl	main
1:	wfi
	b	1b

	.ltorg
