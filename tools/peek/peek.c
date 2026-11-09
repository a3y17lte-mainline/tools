/* peek ADDR [COUNT] | poke ADDR VAL | dump ADDR LEN (raw bytes to stdout) : /dev/mem, freestanding aarch64 */
typedef unsigned long ul; typedef unsigned int u32;
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c,
		x3 asm("x3") = d, x4 asm("x4") = e, x5 asm("x5") = f;
	asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
static void out(const char *s) { int n = 0; while (s[n]) n++; sc(64, 1, (long)s, n, 0, 0, 0); }
static ul num(const char *s)
{
	ul v = 0; int b = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { b = 16; s += 2; }
	for (; *s; s++) { int d = *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * b + d; }
	return v;
}
static void hex(ul v, int w) { char b[17]; for (int i = w - 1; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15]; b[w] = 0; out(b); }
static int run(int c, char **v)
{
	if (c < 2) return 2;
	int poke = 0, dump = 0;
	for (char *p = v[0]; *p; p++) { if (p[0] == 'p' && p[1] == 'o') poke = 1; if (p[0] == 'd' && p[1] == 'u') dump = 1; }
	ul a = num(v[1]), pg = a & ~0xfffUL;
	ul len = dump && c > 2 ? num(v[2]) : 4, maplen = dump ? (a - pg + len + 0xfff) & ~0xfffUL : 0x2000;
	long fd = sc(56, -100, (long)"/dev/mem", 02 | 04010000, 0, 0, 0);	/* openat O_RDWR|O_SYNC */
	if (fd < 0) { out("open /dev/mem failed\n"); return 1; }
	long m = sc(222, 0, maplen, 3, 1, fd, pg);				/* mmap shared rw */
	if (m < 0 && m > -4096) { out("mmap failed\n"); return 1; }
	volatile u32 *p = (volatile u32 *)(m + (a - pg));
	if (dump) {	/* copy word by word: device/uncached mappings dislike unaligned or wide accesses */
		static u32 buf[1024];
		for (ul off = 0; off < len; off += sizeof(buf)) {
			ul n = len - off < sizeof(buf) ? len - off : sizeof(buf);
			for (ul i = 0; i < (n + 3) / 4; i++) buf[i] = p[off / 4 + i];
			for (ul w = 0; w < n;) { long r = sc(64, 1, (long)buf + w, n - w, 0, 0, 0); if (r <= 0) return 1; w += r; }
		}
		return 0;
	}
	if (poke) { *p = (u32)num(v[2]); }
	int n = (!poke && c > 2) ? (int)num(v[2]) : 1;
	for (int i = 0; i < n; i++) { out("0x"); hex(a + 4 * i, 8); out(": 0x"); hex(p[i], 8); out("\n"); }
	return 0;
}
void __attribute__((used)) cmain(long *sp) { sc(93, run((int)sp[0], (char **)(sp + 1)), 0, 0, 0, 0, 0); }
asm(".globl _start\n_start: mov x0, sp\n bl cmain\n");
