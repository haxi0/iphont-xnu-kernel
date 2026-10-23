/*
 * mkpinned FILE BLOCKS: make FILE a file of BLOCKS 4 KB blocks that f2fs never moves -- pinned
 * (F2FS_IOC_SET_PIN_FILE: the garbage collector leaves its blocks where they are, as Android's own swap
 * files and Dynamic System Update images are kept) and allocated in full (fallocate) -- so that where its
 * blocks lie on the flash stays true for as long as the file is there. This is how HackiOS keeps darwin in
 * a file of userdata instead of a partition of its own (common/flash/darwinfile.sh): nothing of the
 * partition table or of the filesystem's size changes. FILE must not exist yet.
 */
typedef unsigned long u64;
typedef unsigned int u32;

#define AT_FDCWD (-100)
#define O_WRONLY_CREAT_EXCL (01 | 0100 | 0200)
#define F2FS_IOC_SET_PIN_FILE 0x4004f50dUL     /* _IOW(0xf5, 13, __u32) */

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
num(long v)
{
	char b[24];
	int i = 23;
	int neg = v < 0;
	u64 u = neg ? (u64)-v : (u64)v;
	b[i] = 0;
	do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
	if (neg) b[--i] = '-';
	out(b + i);
}

static u64
parse(const char *s)
{
	u64 v = 0;
	for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (u64)(*s - '0');
	return v;
}

static void
fail(const char *what, long r)
{
	out("mkpinned: "); out(what); out(": errno "); num(-r); out("\n");
	sys(93, 1, 0, 0, 0);
}

void
_start_c(long *sp)
{
	char **argv = (char **)(sp + 1);
	u32 pin = 1;
	long fd, r;
	u64 blocks;

	if (sp[0] != 3) { out("usage: mkpinned FILE BLOCKS\n"); sys(93, 2, 0, 0, 0); }
	blocks = parse(argv[2]);
	fd = sys(56, AT_FDCWD, (long)argv[1], O_WRONLY_CREAT_EXCL, 0600);
	if (fd < 0) fail("create (it must not exist yet)", fd);
	/* pinned while it is empty: f2fs pins only a file with no blocks yet, and allocates it for that */
	r = sys(29, fd, F2FS_IOC_SET_PIN_FILE, (long)&pin, 0);
	if (r < 0) fail("F2FS_IOC_SET_PIN_FILE", r);
	r = sys(47, fd, 0, 0, (long)(blocks * 4096));          /* fallocate(fd, 0, 0, size) */
	if (r < 0) fail("fallocate", r);
	r = sys(82, fd, 0, 0, 0);                               /* fsync */
	if (r < 0) fail("fsync", r);
	out("mkpinned: "); out(argv[1]); out(": "); num((long)blocks); out(" blocks, pinned and allocated\n");
	sys(93, 0, 0, 0, 0);
}

__asm__(".globl _start\n_start:\n mov x0, sp\n b _start_c\n");
