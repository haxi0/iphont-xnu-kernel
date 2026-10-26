#!/usr/bin/env python3
"""
kprof.py RUN.log [KERNEL.unstripped]: the kernel's own hot spots from a run that sampled with `power_test kprof` (kern.hackios.kprof): the
"t-kprof:" line's 32-byte places named by the kernel's symbols, summed by function, most often first.
The kernel: xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped (the default).
"""
import bisect
import re
import subprocess
import sys
from collections import Counter

KERNEL = 'xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped'


def symbols(path):
    out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
    table = [(int(a, 16), n) for a, t, n in (l.split() for l in out.splitlines() if len(l.split()) == 3) if t not in 'Uu']
    return [a for a, _ in table], [n for _, n in table]


def main():
    log = sys.argv[1]
    addrs, names = symbols(sys.argv[2] if len(sys.argv) > 2 else KERNEL)
    for line in dict.fromkeys(l for l in open(log, errors='replace').read().splitlines() if 't-kprof:' in l):
        head, _, rest = line.partition('t-kprof: ')
        label, _, body = rest.partition(' ')
        m = re.match(r'(\d+) in programs of (\d+):(.*)', body)
        if not m:
            continue
        program, total, places = int(m.group(1)), int(m.group(2)), m.group(3).split()
        by = Counter()
        for p in places:
            a, c = p.split(':')
            a = int(a, 16)
            i = bisect.bisect_right(addrs, a) - 1
            by[names[i] if i >= 0 else hex(a)] += int(c)
        idle = sum(c for n, c in by.items() if 'idle' in n.lower() or n.endswith('wfi'))
        kernel = total - program - idle
        print(f'{label}: {total} samples: {program} in programs, {idle} idle (WFI), {kernel} busy in the kernel; the hottest functions:')
        for n, c in by.most_common(40):
            if 'idle' not in n.lower() and c >= 1:
                print(f'  {c:5d} {100 * c / max(kernel, 1):5.1f}% of busy kernel samples  {n}')
                if sum(1 for _ in range(1)) and c < 0.01 * kernel:
                    break


if __name__ == '__main__':
    main()
