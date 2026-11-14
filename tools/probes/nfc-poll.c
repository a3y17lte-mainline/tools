/*
 * nfc-poll: minimal Linux NFC netlink client for the phone's Alpine rootfs, which has no neard
 * (nfctool) and no libc headers. Talks to the kernel NFC core over generic netlink.
 *
 * Usage: nfc-poll up|down [IDX]       power the NFC device IDX (default 0) up or down
 *        nfc-poll poll SECONDS [IDX]  start a reader-mode poll, print each target found
 *                                     (protocols, SENS_RES/SEL_RES, NFCID1 = the tag's UID)
 * "poll" brings the device up first if needed and stops the poll at the end. Exit status 0 =
 * success / at least one target, 1 = error, 2 = no target before the timeout.
 *
 * Freestanding static aarch64 (no libc), like iio-events. Build: ./nfc-poll-build
 */
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nfc.h>

typedef unsigned long ul;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
	register long x3 asm("x3") = d, x4 asm("x4") = e, x5 asm("x5") = f;
	asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
		     : "memory");
	return x0;
}
#define SYS_write 64
#define SYS_exit 93
#define SYS_socket 198
#define SYS_setsockopt 208
#define SYS_sendto 206
#define SYS_recvfrom 207
#define AF_NETLINK 16
#define SOCK_RAW 3
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
#define SOL_NETLINK 270
#define EAGAIN 11
#define EALREADY 114

static ul slen(const char *s) { ul n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_write, 1, (long)s, slen(s), 0, 0, 0); }
static void die(const char *s) { out(s); out("\n"); sc(SYS_exit, 1, 0, 0, 0, 0, 0); }
static void outn(long v)
{
	char b[24]; int i = 23; int neg = v < 0;
	b[i] = 0; if (neg) v = -v;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	if (neg) b[--i] = '-';
	out(b + i);
}
static void outhex(const u8 *p, int n)
{
	static const char h[] = "0123456789abcdef";
	char b[3] = { 0, 0, 0 };
	for (int i = 0; i < n; i++) { b[0] = h[p[i] >> 4]; b[1] = h[p[i] & 15]; out(b); }
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static long atoi_(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return v; }

static int fd, fam, mcast = -1;
static u32 seq = 1;
static u8 buf[8192] __attribute__((aligned(4)));

/* message under construction */
static u8 msg[512] __attribute__((aligned(4)));
static u32 mlen;

static void put_attr(u16 type, const void *data, u16 len)
{
	struct nlattr *a = (struct nlattr *)(msg + mlen);
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	for (int i = 0; i < len; i++) ((u8 *)a + NLA_HDRLEN)[i] = ((const u8 *)data)[i];
	mlen += NLA_ALIGN(a->nla_len);
}

static void start(u16 type, u16 flags, u8 cmd, u8 ver)
{
	struct nlmsghdr *h = (struct nlmsghdr *)msg;
	struct genlmsghdr *g = (struct genlmsghdr *)(msg + NLMSG_HDRLEN);
	for (int i = 0; i < (int)sizeof(msg); i++) msg[i] = 0;
	h->nlmsg_type = type;
	h->nlmsg_flags = NLM_F_REQUEST | flags;
	h->nlmsg_seq = ++seq;
	g->cmd = cmd;
	g->version = ver;
	mlen = NLMSG_HDRLEN + GENL_HDRLEN;
}

static void send_msg(void)
{
	struct sockaddr_nl { u16 family; u16 pad; u32 pid; u32 groups; } sa = { AF_NETLINK, 0, 0, 0 };
	((struct nlmsghdr *)msg)->nlmsg_len = mlen;
	if (sc(SYS_sendto, fd, (long)msg, mlen, 0, (long)&sa, sizeof(sa)) < 0)
		die("sendto failed");
}

/* walk the attributes of one genl message, calling fn for each */
static void attrs(struct nlmsghdr *h, void (*fn)(struct nlattr *a))
{
	u8 *p = (u8 *)h + NLMSG_HDRLEN + GENL_HDRLEN;
	u8 *end = (u8 *)h + h->nlmsg_len;
	while (p + NLA_HDRLEN <= end) {
		struct nlattr *a = (struct nlattr *)p;
		if (a->nla_len < NLA_HDRLEN) break;
		fn(a);
		p += NLA_ALIGN(a->nla_len);
	}
}
#define ADATA(a) ((u8 *)(a) + NLA_HDRLEN)
#define ALEN(a) ((a)->nla_len - NLA_HDRLEN)

static void fam_attr(struct nlattr *a)
{
	if (a->nla_type == CTRL_ATTR_FAMILY_ID)
		fam = *(u16 *)ADATA(a);
	if (a->nla_type == CTRL_ATTR_MCAST_GROUPS) {
		u8 *g = ADATA(a), *gend = ADATA(a) + ALEN(a);
		while (g + NLA_HDRLEN <= gend) {
			struct nlattr *grp = (struct nlattr *)g;
			u8 *q = ADATA(grp), *qend = ADATA(grp) + ALEN(grp);
			int id = -1, ev = 0;
			while (q + NLA_HDRLEN <= qend) {
				struct nlattr *x = (struct nlattr *)q;
				if (x->nla_type == CTRL_ATTR_MCAST_GRP_ID) id = *(u32 *)ADATA(x);
				if (x->nla_type == CTRL_ATTR_MCAST_GRP_NAME)
					ev = streq((char *)ADATA(x), NFC_GENL_MCAST_EVENT_NAME);
				q += NLA_ALIGN(x->nla_len);
			}
			if (ev) mcast = id;
			g += NLA_ALIGN(grp->nla_len);
		}
	}
}

static void tgt_attr(struct nlattr *a)
{
	switch (a->nla_type) {
	case NFC_ATTR_TARGET_INDEX: out(" target "); outn(*(u32 *)ADATA(a)); break;
	case NFC_ATTR_PROTOCOLS: out(" protocols 0x"); outhex(ADATA(a) + 3, 1); outhex(ADATA(a) + 2, 1);
		outhex(ADATA(a) + 1, 1); outhex(ADATA(a), 1); break;
	case NFC_ATTR_TARGET_SENS_RES: out(" sens_res 0x"); outhex(ADATA(a) + 1, 1); outhex(ADATA(a), 1); break;
	case NFC_ATTR_TARGET_SEL_RES: out(" sel_res 0x"); outhex(ADATA(a), 1); break;
	case NFC_ATTR_TARGET_NFCID1: out(" nfcid1 "); outhex(ADATA(a), ALEN(a)); break;
	case NFC_ATTR_TARGET_SENSB_RES: out(" sensb_res "); outhex(ADATA(a), ALEN(a)); break;
	case NFC_ATTR_TARGET_SENSF_RES: out(" sensf_res "); outhex(ADATA(a), ALEN(a)); break;
	case NFC_ATTR_TARGET_ISO15693_UID: out(" iso15693_uid "); outhex(ADATA(a), ALEN(a)); break;
	}
}

/*
 * Receive until the reply to the last request: returns the netlink error (0 = ack) for
 * plain requests; for dumps calls fn on each message and returns 0 at NLMSG_DONE.
 * Multicast events seen meanwhile set *event to their command. Returns -EAGAIN on timeout.
 */
static int recv_reply(void (*fn)(struct nlmsghdr *h), int *event)
{
	for (;;) {
		long n = sc(SYS_recvfrom, fd, (long)buf, sizeof(buf), 0, 0, 0);
		if (n < 0) return n;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_seq == 0 && h->nlmsg_type == fam) {
				if (event) *event = ((struct genlmsghdr *)NLMSG_DATA(h))->cmd;
				continue;
			}
			if (h->nlmsg_seq != seq) continue;
			if (h->nlmsg_type == NLMSG_ERROR) return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
			if (h->nlmsg_type == NLMSG_DONE) return 0;
			if (fn) fn(h);
		}
	}
}

static void fam_msg(struct nlmsghdr *h) { attrs(h, fam_attr); }
static void tgt_msg(struct nlmsghdr *h) { out("found:"); attrs(h, tgt_attr); out("\n"); }

static int simple(u8 cmd, u32 idx, u32 protos)
{
	start(fam, NLM_F_ACK, cmd, NFC_GENL_VERSION);
	put_attr(NFC_ATTR_DEVICE_INDEX, &idx, 4);
	if (protos) put_attr(NFC_ATTR_PROTOCOLS, &protos, 4);
	send_msg();
	return recv_reply(0, 0);
}

static void set_timeout(long sec)
{
	long tv[2] = { sec, 0 };
	sc(SYS_setsockopt, fd, SOL_SOCKET, SO_RCVTIMEO, (long)tv, sizeof(tv), 0);
}

static void report(const char *what, int err)
{
	out(what); out(": "); outn(err); out(err ? " (errno)\n" : " ok\n");
}

int main(int argc, char **argv)
{
	u32 idx;
	int err;

	if (argc < 2) die("usage: nfc-poll up|down [IDX] | poll SECONDS [IDX]");
	fd = sc(SYS_socket, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC, 0, 0, 0);
	if (fd < 0) die("socket failed");
	set_timeout(5);

	start(GENL_ID_CTRL, NLM_F_ACK, CTRL_CMD_GETFAMILY, 1);
	put_attr(CTRL_ATTR_FAMILY_NAME, NFC_GENL_NAME, sizeof(NFC_GENL_NAME));
	send_msg();
	{
		int f = -1;

		for (;;) {
			long n = sc(SYS_recvfrom, fd, (long)buf, sizeof(buf), 0, 0, 0);
			if (n < 0) die("no reply from genl ctrl");
			struct nlmsghdr *h = (struct nlmsghdr *)buf;
			if (h->nlmsg_type == NLMSG_ERROR) {
				if (((struct nlmsgerr *)NLMSG_DATA(h))->error) die("nfc family not found (module loaded?)");
				break;
			}
			fam = -1; fam_msg(h); f = fam;
		}
		fam = f;
	}
	if (fam < 0) die("nfc family not found");

	if (streq(argv[1], "up") || streq(argv[1], "down")) {
		idx = argc > 2 ? atoi_(argv[2]) : 0;
		err = simple(streq(argv[1], "up") ? NFC_CMD_DEV_UP : NFC_CMD_DEV_DOWN, idx, 0);
		report(argv[1], err);
		return err != 0;
	}
	if (!streq(argv[1], "poll") || argc < 3) die("usage: nfc-poll up|down [IDX] | poll SECONDS [IDX]");

	long secs = atoi_(argv[2]);
	idx = argc > 3 ? atoi_(argv[3]) : 0;
	if (mcast < 0) die("no nfc events multicast group");
	if (sc(SYS_setsockopt, fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, (long)&mcast, 4, 0) < 0)
		die("cannot join the nfc events group");

	err = simple(NFC_CMD_DEV_UP, idx, 0);
	if (err && err != -EALREADY) { report("up", err); return 1; }

	u32 protos = NFC_PROTO_JEWEL_MASK | NFC_PROTO_MIFARE_MASK | NFC_PROTO_FELICA_MASK |
		     NFC_PROTO_ISO14443_MASK | NFC_PROTO_ISO14443_B_MASK | NFC_PROTO_ISO15693_MASK;
	err = simple(NFC_CMD_START_POLL, idx, protos);
	report("start_poll", err);
	if (err) return 1;

	int ev = 0, found = 0;
	set_timeout(1);
	for (long t = 0; t < secs && !found; t++) {
		for (;;) {
			long n = sc(SYS_recvfrom, fd, (long)buf, sizeof(buf), 0, 0, 0);
			if (n < 0) break;
			for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n))
				if (h->nlmsg_type == fam)
					ev = ((struct genlmsghdr *)NLMSG_DATA(h))->cmd;
			if (ev == NFC_EVENT_TARGETS_FOUND) { found = 1; break; }
		}
	}
	set_timeout(5);
	if (found) {
		start(fam, NLM_F_DUMP, NFC_CMD_GET_TARGET, NFC_GENL_VERSION);
		put_attr(NFC_ATTR_DEVICE_INDEX, &idx, 4);
		send_msg();
		err = recv_reply(tgt_msg, 0);
		if (err) report("get_target", err);
	} else {
		out("no target\n");
	}
	/* the core ends the poll itself once a target is activated */
	err = simple(NFC_CMD_STOP_POLL, idx, 0);
	if (err && !found) report("stop_poll", err);
	return found ? 0 : 2;
}

void __attribute__((naked, noreturn)) _start(void)
{
	asm volatile(
		"ldr x0, [sp]\n"
		"add x1, sp, #8\n"
		"bl main\n"
		"mov x8, #93\n"
		"svc 0\n");
}
