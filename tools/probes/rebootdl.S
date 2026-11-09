/*
 * rebootdl: S-Boot payload probe that does not depend on the screen.
 * Paints the framebuffer red, waits ~1 s, then asks S-Boot for download
 * mode (PMU INFORM3 = 0x12345671) and triggers a software reset
 * (PMU SWRESET = 1). If the phone comes back in download mode on its own,
 * S-Boot ran our code.
 */
	.text
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
	/* paint red */
	ldr	w2, =0xffff0000
	ldr	x1, =0x67000000
	ldr	x3, =(720 * 1280)
1:	str	w2, [x1], #4
	subs	x3, x3, #1
	b.ne	1b
	dsb	sy

	/* ~1 s */
	ldr	x5, =400000000
2:	subs	x5, x5, #1
	b.ne	2b

	/* request download mode, then software reset */
	ldr	x1, =0x1048080c		/* PMU INFORM3 */
	ldr	w2, =0x12345671
	str	w2, [x1]
	dsb	sy
	ldr	x1, =0x10480400		/* PMU SWRESET */
	mov	w2, #1
	str	w2, [x1]
	dsb	sy
3:	wfi
	b	3b

	.ltorg
	.balign	8
_end:
