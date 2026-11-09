/*
 * decon_probe: find out how to get pixels onto the A3 2017 command-mode panel
 * after S-Boot. Watch the screen; each colour is one experiment:
 *
 *   RED    painted, nothing else touched        -> visible = panel refreshes by itself
 *   GREEN  painted after enabling HW (TE) auto-trigger
 *   BLUE   painted, then 40 software triggers over 2 s
 *   WHITE  painted, software trigger again (repeat check)
 *
 * Then it reboots into download mode, so no buttons are needed afterwards.
 */
typedef unsigned int u32;
typedef unsigned long u64;

#define DECON		0x14830000UL
#define VIDCON0		0x0000
#define  ENVID		(1 << 1)
#define  ENVID_F	(1 << 0)
#define WINCON(w)	(0x0050 + (w) * 4)
#define  ENWIN		(1 << 0)
#define VIDW_ADD0(w)	(0x0880 + (w) * 0x10)
#define TRIGCON		0x06B0
#define  TRIG_SAVE_DISABLE_SYNCMGR	(1 << 13)
#define  HWTRIG_AUTO_MASK		(1 << 6)
#define  HWTRIGMASK_DISPIF0		(1 << 4)
#define  HWTRIGEN_I80_RGB		(1 << 3)
#define  SWTRIGCMD_I80_RGB		(1 << 1)
#define  SWTRIGEN_I80_RGB		(1 << 0)
#define DECON_UPDATE	0x0710
#define  UPDATE_STANDALONE_F		(1 << 0)

#define PMU_INFORM3	0x1048080cUL
#define PMU_SWRESET	0x10480400UL

#define W 720
#define H 1280

static inline u32 rd(u64 a) { return *(volatile u32 *)a; }
static inline void wr(u64 a, u32 v) { *(volatile u32 *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
static inline u32 dr(u32 off) { return rd(DECON + off); }
static inline void dw(u32 off, u32 v) { wr(DECON + off, v); }

static u64 now(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void delay_ms(u64 ms)
{
	u64 end = now() + ms * 26000;	/* 26 MHz arch timer */
	while (now() < end)
		;
}

static void paint(u64 fb, u32 color)
{
	volatile u32 *p = (volatile u32 *)fb;
	for (u32 i = 0; i < W * H; i++)
		p[i] = color;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void update(void)
{
	dw(DECON_UPDATE, dr(DECON_UPDATE) | UPDATE_STANDALONE_F);
}

static void sw_trigger(void)
{
	dw(TRIGCON, dr(TRIGCON) | SWTRIGCMD_I80_RGB);
}

void main(void)
{
	u64 fb = 0x67000000;
	u32 t;

	/* Use the buffer of whatever window S-Boot left enabled. */
	for (int w = 0; w < 5; w++) {
		if (dr(WINCON(w)) & ENWIN) {
			u32 a = dr(VIDW_ADD0(w));
			if (a >= 0x40000000)
				fb = a;
			break;
		}
	}

	/* RED: untouched DECON state */
	paint(fb, 0xffff0000);
	delay_ms(3000);

	/* GREEN: hardware (TE) auto trigger, like the stock kernel */
	t = dr(TRIGCON);
	t &= ~SWTRIGEN_I80_RGB;
	t |= HWTRIGEN_I80_RGB | HWTRIG_AUTO_MASK | TRIG_SAVE_DISABLE_SYNCMGR | HWTRIGMASK_DISPIF0;
	dw(TRIGCON, t);
	dw(VIDCON0, dr(VIDCON0) | ENVID | ENVID_F);
	update();
	paint(fb, 0xff00ff00);
	delay_ms(3000);

	/* BLUE: software trigger */
	paint(fb, 0xff0000ff);
	t = dr(TRIGCON);
	t &= ~(HWTRIGEN_I80_RGB | HWTRIGMASK_DISPIF0);
	t |= SWTRIGEN_I80_RGB;
	dw(TRIGCON, t);
	update();
	for (int i = 0; i < 40; i++) {
		sw_trigger();
		delay_ms(50);
	}
	delay_ms(1000);

	/* WHITE: software trigger once more */
	paint(fb, 0xffffffff);
	sw_trigger();
	delay_ms(3000);

	/* back to download mode */
	wr(PMU_INFORM3, 0x12345671);
	wr(PMU_SWRESET, 1);
}
