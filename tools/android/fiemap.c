/*
 * fiemap FILE: where FILE's blocks lie on its device (FS_IOC_FIEMAP), in 4 KB blocks from the start of
 * the partition -- to see whether a file that cannot be moved (a pinned swap file, say) sits in the part of
 * userdata that tools/userdata-carve.py would take. Reads only. Built like f2fs-resize (tools/android/build.sh).
 */
typedef unsigned long u64;
typedef unsigned int u32;

#define FS_IOC_FIEMAP 0xc020660bUL
#define AT_FDCWD (-100)

static long
sys(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
	__asm__ volatile ("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static void
out(const char *s)
{
	long n = 0;
	while (s[n]) n++;
	sys(64, 1, (long)s, n, 0);
}

static void
num(u64 v)
{
	char b[24];
	int i = 23;
	b[i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
	out(b + i);
}

struct extent { u64 logical, physical, length, r1, r2; u32 flags, r3[3]; };
struct fiemap { u64 start, length; u32 flags, mapped, count, reserved; struct extent e[256]; };

static struct fiemap fm;

void
_start_c(long *sp)
{
	char **argv = (char **)(sp + 1);
	long fd;
	u64 at = 0, total = 0;

	if (sp[0] < 2) { out("usage: fiemap FILE\n"); sys(93, 2, 0, 0, 0); }
	fd = sys(56, AT_FDCWD, (long)argv[1], 0, 0);
	if (fd < 0) { out("fiemap: cannot open\n"); sys(93, 1, 0, 0, 0); }
	for (;;) {
		fm.start = at; fm.length = ~0UL; fm.flags = 1; /* FIEMAP_FLAG_SYNC */ fm.count = 256; fm.mapped = 0;
		if (sys(29, fd, FS_IOC_FIEMAP, (long)&fm, 0) < 0) { out("fiemap: FS_IOC_FIEMAP failed\n"); sys(93, 1, 0, 0, 0); }
		if (fm.mapped == 0) break;
		for (u32 i = 0; i < fm.mapped; i++) {
			num(fm.e[i].physical / 4096); out(" "); num(fm.e[i].length / 4096); out("\n");
			total += fm.e[i].length / 4096;
			at = fm.e[i].logical + fm.e[i].length;
			if (fm.e[i].flags & 1) { out("total "); num(total); out("\n"); sys(93, 0, 0, 0, 0); } /* LAST */
		}
	}
	out("total "); num(total); out("\n");
	sys(93, 0, 0, 0, 0);
}

__asm__(".globl _start\n_start:\n mov x0, sp\n b _start_c\n");
