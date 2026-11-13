/*
 * ssp-probe: talk to the Galaxy A3 2017 sensor hub (STM32F410, Samsung "SSP" firmware) from
 * userspace through spidev + the GPIO character device, using READ commands only:
 *   WHOAMI (0x0f, 1 byte, expect 0x55), FIRMWARE_REV (0xf0, u32), SENSOR_SCANNING (0xf4, u64).
 * Nothing on the hub changes state. Without -r, MCU reset and BOOT0 are never requested
 * (requesting them, even as inputs, would change their direction): read their levels with
 * ssp-pins.sh instead.
 *
 * Usage: ssp-probe [-n] [-r] [-d /dev/spidevX.Y] [-s HZ] [-m MODE] [whoami|rev|scan|all]
 *   -n  dry run: print the bytes that would be sent, touch no device.
 *   -r  release the MCU from reset first: BOOT0 (gpd4-3) := 0, nRST (gpd2-3) := 1, wait 500 ms,
 *       as the vendor kernel does at boot (S-Boot leaves nRST low). The lines stay that way.
 * Lines (by gpiochip label): AP_INT gpd4-2 (output, idle high), MCU_INT2 gpd3-7 (input),
 * MCU_INT1 gpa0-7 (input, low = hub has a message).
 *
 * Freestanding static aarch64 (no libc), like tools/peek. Build: ./ssp-probe-build
 */
#include <linux/gpio.h>
#include <linux/spi/spidev.h>

typedef unsigned long ul;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c,
		x3 asm("x3") = d, x4 asm("x4") = e, x5 asm("x5") = f;
	asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
#define SYS_openat 56
#define SYS_close 57
#define SYS_write 64
#define SYS_ioctl 29
#define SYS_nanosleep 101
#define SYS_exit 93
#define AT_FDCWD -100
#define O_RDWR 02
#define O_CLOEXEC 02000000

void *memset(void *d, int c, ul n) { u8 *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, ul n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0, 0, 0); }
static void hex(u64 v, int w)
{
	char b[17];
	for (int i = w - 1; i >= 0; i--, v >>= 4)
		b[i] = "0123456789abcdef"[v & 15];
	b[w] = 0;
	out(b);
}
static void dec(u64 v)
{
	char b[21]; int i = 20;
	b[i] = 0;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	out(b + i);
}
static ul num(const char *s)
{
	ul v = 0; int b = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { b = 16; s += 2; }
	for (; *s; s++) { int d = *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * b + d; }
	return v;
}
static void sleep_us(long us)
{
	long ts[2] = { us / 1000000, (us % 1000000) * 1000 };
	sc(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}
static void dump(const u8 *p, int n)
{
	for (int i = 0; i < n; i++) { hex(p[i], 2); out(i + 1 < n ? " " : "\n"); }
	if (!n) out("\n");
}

static int dry, spifd = -1;
static u32 speed = 4800000;
static u8 mode = SPI_MODE_1;

/* --- GPIO lines ------------------------------------------------------------------------- */
struct line { const char *bank; u32 off; int out; int fd; const char *name; };
static struct line ap_int = { "gpd4", 2, 1, -1, "AP_INT" };
static struct line mcu_int2 = { "gpd3", 7, 0, -1, "MCU_INT2" };
static struct line mcu_int1 = { "gpa0", 7, 0, -1, "MCU_INT1" };
static struct line boot0 = { "gpd4", 3, 1, -1, "BOOT0" };	/* requested low */
static struct line nrst = { "gpd2", 3, 1, -1, "nRST" };	/* requested high */

static int chip_by_label(const char *label)
{
	char path[] = "/dev/gpiochip\0\0\0";
	struct gpiochip_info info;

	for (int i = 0; i < 64; i++) {
		int n = 13;
		if (i >= 10) path[n++] = '0' + i / 10;
		path[n++] = '0' + i % 10;
		path[n] = 0;
		long fd = sc(SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_CLOEXEC, 0, 0, 0);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		if (sc(SYS_ioctl, fd, GPIO_GET_CHIPINFO_IOCTL, (long)&info, 0, 0, 0) == 0 &&
		    streq(info.label, label))
			return fd;
		sc(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	return -1;
}

static int line_request(struct line *l)
{
	struct gpio_v2_line_request req;
	long chip;

	if (dry)
		return 0;
	chip = chip_by_label(l->bank);
	if (chip < 0) { out("no gpiochip labelled "); out(l->bank); out("\n"); return -1; }
	memset(&req, 0, sizeof(req));
	req.offsets[0] = l->off;
	req.num_lines = 1;
	memcpy(req.consumer, "ssp-probe", 10);
	if (l->out) {
		req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
		req.config.num_attrs = 1;
		req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		req.config.attrs[0].attr.values = l == &boot0 ? 0 : 1;	/* BOOT0 low, others high */
		req.config.attrs[0].mask = 1;
	} else {
		req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
	}
	long r = sc(SYS_ioctl, chip, GPIO_V2_GET_LINE_IOCTL, (long)&req, 0, 0, 0);
	sc(SYS_close, chip, 0, 0, 0, 0, 0);
	if (r < 0) { out("cannot request "); out(l->name); out(" (in use by a driver?)\n"); return -1; }
	l->fd = req.fd;
	return 0;
}

static int line_get(struct line *l)
{
	struct gpio_v2_line_values v = { .bits = 0, .mask = 1 };

	if (dry)
		return 1;
	if (sc(SYS_ioctl, l->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, (long)&v, 0, 0, 0) < 0)
		return -1;
	return v.bits & 1;
}

static int line_set(struct line *l, int val)
{
	struct gpio_v2_line_values v = { .bits = (u64)!!val, .mask = 1 };

	if (dry) { out("  "); out(l->name); out(val ? " := 1\n" : " := 0\n"); return 0; }
	return sc(SYS_ioctl, l->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, (long)&v, 0, 0, 0) < 0 ? -1 : 0;
}

/* Poll a line every 3 ms, give up after ~1.5 s (the kernel driver's limits). */
static int wait_line(struct line *l, int val)
{
	if (dry) { out("  wait "); out(l->name); out(val ? " == 1\n" : " == 0\n"); return 0; }
	for (int i = 0; i <= 500; i++) {
		int v = line_get(l);
		if (v == val)
			return 0;
		if (v < 0)
			return -1;
		sleep_us(3000);
	}
	out("timeout waiting for "); out(l->name); out(val ? " high\n" : " low\n");
	return -1;
}

/* --- SPI -------------------------------------------------------------------------------- */
static int spi_xfer(const u8 *tx, u8 *rx, u32 len)
{
	struct spi_ioc_transfer t;

	if (dry) {
		if (tx) { out("  spi write: "); dump(tx, len); }
		else { out("  spi read "); dec(len); out(" bytes\n"); memset(rx, 0, len); }
		return 0;
	}
	memset(&t, 0, sizeof(t));
	t.tx_buf = (ul)tx;
	t.rx_buf = (ul)rx;
	t.len = len;
	t.speed_hz = speed;
	t.bits_per_word = 8;
	return sc(SYS_ioctl, spifd, SPI_IOC_MESSAGE(1), (long)&t, 0, 0, 0) < 0 ? -1 : 0;
}

static u8 frame[4096];

/*
 * Service one message the hub announced by pulling MCU_INT1 low: read the 4-byte header
 * (u16 options, u16 length). If it is the reply to our READ, copy the payload to buf.
 * HUB2AP frames (sensor data, debug strings) are read and printed. Returns 1 when our reply
 * was received, 0 for anything else, -1 on errors.
 */
static int service(u8 *buf, u16 want_len)
{
	u8 hdr[4];
	u16 opt, len;

	if (spi_xfer(0, hdr, 4) < 0) { out("spi read failed\n"); return -1; }
	opt = hdr[0] | hdr[1] << 8;
	len = hdr[2] | hdr[3] << 8;
	if (dry) { opt = 0; len = want_len; }
	out("  hub header: options 0x"); hex(opt, 4); out(" length "); dec(len); out("\n");

	if ((opt & 3) == 0 && len == want_len) {	/* AP2HUB_READ: our reply */
		if (spi_xfer(0, buf, len) < 0) return -1;
		if (dry && len == 1) buf[0] = 0x55;	/* let a dry run show the whole sequence */
		out("  payload: "); dump(buf, len);
		return 1;
	}
	if ((opt & 3) == 2 && len && len <= sizeof(frame)) {	/* HUB2AP_WRITE: a data frame */
		if (spi_xfer(0, frame, len) < 0) return -1;
		out("  unsolicited frame: "); dump(frame, len < 64 ? len : 64);
		return 0;
	}
	out("  unexpected header, stopping (no further reads)\n");
	return -1;
}

static int ssp_read(u8 cmd, u16 len, u8 *buf)
{
	u8 h[9] = { cmd, (u8)len, (u8)(len >> 8), 0, 0, 0, 0, 0, 0 };	/* options 0 = AP2HUB_READ */
	int r;

	out("cmd 0x"); hex(cmd, 2); out(", read "); dec(len); out(" bytes\n");

	/* drain anything the hub already queued */
	while (line_get(&mcu_int1) == 0) {
		out("  MCU_INT1 already low: draining\n");
		if (service(0, 0xffff) < 0) return -1;
		sleep_us(1000);
	}

	if (line_set(&ap_int, 0) < 0 || wait_line(&mcu_int2, 0) < 0) goto fail;
	if (spi_xfer(h, 0, 9) < 0) { out("spi write failed\n"); goto fail; }
	if (line_set(&ap_int, 1) < 0 || wait_line(&mcu_int2, 1) < 0) return -1;

	for (int tries = 0; tries < 8; tries++) {
		if (wait_line(&mcu_int1, 0) < 0) return -1;
		r = service(buf, len);
		if (r)
			return r < 0 ? -1 : 0;
		sleep_us(1000);
	}
	out("  no reply\n");
	return -1;
fail:
	line_set(&ap_int, 1);
	return -1;
}

static const char *const types[64] = {
	[1] = "accel", [2] = "mag", [3] = "orientation", [4] = "gyro", [5] = "light",
	[6] = "pressure", [8] = "prox", [11] = "rotation vector", [14] = "mag uncal",
	[15] = "game rotation vector", [16] = "gyro uncal", [17] = "sig motion",
	[18] = "step detector", [19] = "step counter", [20] = "geomag rotation vector",
	[22] = "tilt", [25] = "pickup", [28] = "accel int", [29] = "prox raw",
	[30] = "geomag power", [31] = "interrupt gyro", [32] = "scontext", [33] = "mobeam",
	[34] = "light cct",
};

static int run(int argc, char **argv)
{
	const char *dev = "/dev/spidev4.0", *what = "all";
	int release = 0;
	u8 b[8];

	for (int i = 1; i < argc; i++) {
		if (streq(argv[i], "-n")) dry = 1;
		else if (streq(argv[i], "-r")) release = 1;
		else if (streq(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
		else if (streq(argv[i], "-s") && i + 1 < argc) speed = num(argv[++i]);
		else if (streq(argv[i], "-m") && i + 1 < argc) mode = num(argv[++i]) & 3;
		else if (streq(argv[i], "whoami") || streq(argv[i], "rev") || streq(argv[i], "scan") ||
			 streq(argv[i], "all")) what = argv[i];
		else { out("usage: ssp-probe [-n] [-r] [-d DEV] [-s HZ] [-m MODE] [whoami|rev|scan|all]\n"); return 2; }
	}

	if (!dry) {
		spifd = sc(SYS_openat, AT_FDCWD, (long)dev, O_RDWR | O_CLOEXEC, 0, 0, 0);
		if (spifd < 0) { out("cannot open "); out(dev); out("\n"); return 1; }
		u8 bits = 8;
		if (sc(SYS_ioctl, spifd, SPI_IOC_WR_MODE, (long)&mode, 0, 0, 0) < 0 ||
		    sc(SYS_ioctl, spifd, SPI_IOC_WR_BITS_PER_WORD, (long)&bits, 0, 0, 0) < 0 ||
		    sc(SYS_ioctl, spifd, SPI_IOC_WR_MAX_SPEED_HZ, (long)&speed, 0, 0, 0) < 0) {
			out("spidev setup failed\n"); return 1;
		}
	}
	if (line_request(&mcu_int1) || line_request(&mcu_int2) || line_request(&ap_int))
		return 1;
	if (release) {
		/* BOOT0 first: the MCU samples it when it leaves reset */
		out("releasing the MCU from reset (BOOT0 low, nRST high), waiting 500 ms\n");
		if (line_request(&boot0) || line_request(&nrst))
			return 1;
		if (!dry)
			sleep_us(500000);
	}

	out("spi mode "); dec(mode); out(", "); dec(speed); out(" Hz; MCU_INT1 ");
	dec(line_get(&mcu_int1)); out(", MCU_INT2 "); dec(line_get(&mcu_int2)); out("\n");

	int all = streq(what, "all"), fail = 0;

	if (all || streq(what, "whoami")) {
		int ok = 0;
		for (int i = 0; i < 3 && !ok; i++) {	/* the first transfer after reset may fail */
			if (ssp_read(0x0f, 1, b) == 0 && b[0] == 0x55) ok = 1;
			else sleep_us(5000);
		}
		out(ok ? "WHOAMI 0x55: ok\n" : "WHOAMI: FAILED\n");
		if (!ok) return 1;
	}
	if (all || streq(what, "rev")) {
		if (ssp_read(0xf0, 4, b) == 0) {
			u32 rev = b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24;
			out("FIRMWARE_REV "); dec(rev);
			out(rev == 99999 || rev == 0xffffff ? " (invalid: no application firmware)\n" : "\n");
		} else fail = 1;
	}
	if (all || streq(what, "scan")) {
		if (ssp_read(0xf4, 8, b) == 0) {
			u64 m = 0;
			for (int i = 7; i >= 0; i--) m = m << 8 | b[i];
			out("SENSOR_SCANNING 0x"); hex(m, 16); out("\n");
			for (int i = 0; i < 64; i++)
				if (m >> i & 1) {
					out("  "); dec(i); out(" "); out(types[i] ? types[i] : "?"); out("\n");
				}
		} else fail = 1;
	}
	return fail;
}

__attribute__((used)) static void cmain(long *sp)
{
	sc(SYS_exit, run((int)sp[0], (char **)(sp + 1)), 0, 0, 0, 0, 0);
}

asm(".globl _start\n_start:\n\tmov x0, sp\n\tbl cmain\n");
