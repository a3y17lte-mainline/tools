/*
 * iio-events: print the events of an IIO device, like the kernel's tools/iio/iio_event_monitor,
 * for the phone's Alpine rootfs, which has no libc headers or IIO tools.
 *
 * Usage: iio-events /dev/iio:deviceN    (stop it with Ctrl-C or busybox timeout)
 * Prints one line per event: kernel timestamp in ns, channel type, direction, event type.
 *
 * Freestanding static aarch64 (no libc), like ssp-probe. Build: ./iio-events-build
 */
#include <linux/iio/events.h>
#include <linux/iio/types.h>

typedef unsigned long ul;
typedef unsigned char u8;
typedef unsigned long long u64;

static long sc(long n, long a, long b, long c)
{
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
	asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}
#define SYS_openat 56
#define SYS_read 63
#define SYS_write 64
#define SYS_ioctl 29
#define SYS_exit 93
#define AT_FDCWD -100
#define O_RDONLY 0

static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_write, 1, (long)s, slen(s)); }
static void dec(u64 v)
{
	char b[21]; int i = 20;
	b[i] = 0;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	out(b + i);
}
static void die(const char *m) { out(m); out("\n"); sc(SYS_exit, 1, 0, 0); }

static const char *dir_name(unsigned int d)
{
	switch (d) {
	case IIO_EV_DIR_EITHER: return "either";
	case IIO_EV_DIR_RISING: return "rising";
	case IIO_EV_DIR_FALLING: return "falling";
	default: return "other";
	}
}

__attribute__((used)) static void _start_c(long *sp)
{
	char **argv = (char **)(sp + 1);
	struct iio_event_data ev;
	int fd, efd;

	if (sp[0] != 2)
		die("usage: iio-events /dev/iio:deviceN");
	fd = sc(SYS_openat, AT_FDCWD, (long)argv[1], O_RDONLY);
	if (fd < 0)
		die("open failed");
	if (sc(SYS_ioctl, fd, IIO_GET_EVENT_FD_IOCTL, (long)&efd) < 0 || efd < 0)
		die("IIO_GET_EVENT_FD_IOCTL failed (no events on this device?)");

	for (;;) {
		long n = sc(SYS_read, efd, (long)&ev, sizeof(ev));

		if (n != sizeof(ev))
			die("read failed");
		dec(ev.timestamp);
		out(" type ");
		dec(IIO_EVENT_CODE_EXTRACT_CHAN_TYPE(ev.id));
		out(" ");
		out(dir_name(IIO_EVENT_CODE_EXTRACT_DIR(ev.id)));
		out(" evtype ");
		dec(IIO_EVENT_CODE_EXTRACT_TYPE(ev.id));
		out("\n");
	}
}

asm(".globl _start\n_start:\n\tmov x0, sp\n\tbl _start_c\n");
