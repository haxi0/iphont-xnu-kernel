#!/usr/bin/env python3
"""
report.py RUN.log [MORE.log ...]: the frame-time distribution of the same gestures before and after sleeps, one line
per phase of a run of rc.jobs.wakeperf (boot-args touch_lat=1 cpuload=100), and what the kernel said about the
display and the sleeps on the way.

The phases are cut at gaps of PHASE_GAP_S seconds or more between one gesture and the next (a sleep, the screen off,
the screen left locked); a phase that begins with a button press (the side button, then the swipe that unlocks) leaves those two out. A
gesture's frames are counted as tools/frames.py counts them: its movement only (a gap of 100 ms or more is the end
of the movement, not a slow frame). A frame is "late" when it came more than 16.7 ms after the one before (at least
one refresh of the panel's 90 Hz missed) and "very late" over 33.3 ms. "Missed" is how many refreshes the late
ones lost between them: sum(round(gap / 11.11 ms) - 1).

    report.py --self-test
"""
import os
import re
import sys
from os.path import dirname, join

sys.path.insert(0, join(dirname(__file__), '..'))
import frames  # noqa: E402

PHASE_GAP_S = float(os.environ.get('PHASE_GAP_S', 8))   # a sleep and the unlock after it are 12 s or more between gestures
REFRESH_US = 11111
MIN_FRAMES = 300
LATE_US, VERY_LATE_US = 16700, 33300


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))] if ordered else 0


def phases_of(gestures):
    """[[gesture, ...], ...] cut at the long gaps; each gesture (start_us, kind, button, x, y)."""
    out, previous = [], None
    for g in gestures:
        if previous is None or g[0] - previous > PHASE_GAP_S * 1_000_000:
            out.append([])
        out[-1].append(g)
        previous = g[0]
    return out


def moving_gaps(gestures, index, timeline):
    start = gestures[index][0]
    end = min(gestures[index + 1][0] if index + 1 < len(gestures) else start + frames.SEGMENT_US, start + frames.SEGMENT_US)
    chosen = [f for f in timeline if start <= f['done'] < end]
    gaps = [b['done'] - a['done'] for a, b in zip(chosen, chosen[1:])]
    return [g for g in gaps if g < 100000] if len(chosen) >= 3 else []


def summarize(gaps):
    if not gaps:
        return None
    span = sum(gaps)
    late = [g for g in gaps if g > LATE_US]
    return dict(frames=len(gaps), fps=len(gaps) * 1e6 / span, p50=percentile(gaps, .5) / 1000,
                p95=percentile(gaps, .95) / 1000, p99=percentile(gaps, .99) / 1000, worst=max(gaps) / 1000,
                late=100 * len(late) / len(gaps), very=100 * sum(g > VERY_LATE_US for g in gaps) / len(gaps),
                missed=sum(max(0, round(g / REFRESH_US) - 1) for g in late))


def marks_of(paths):
    """The microsecond times of the `rwtouch mark phase` lines (t-mark: phase at US): each begins a phase."""
    found = set()
    for path in paths:
        with open(path, 'rb') as f:
            for m in re.finditer(rb't-mark: phase at (\d+)', f.read()):
                found.add(int(m.group(1)))
    return sorted(found)


def phases_marked(gestures, marks):
    """Gestures cut at the marks (those before the first mark belong to no phase and are dropped)."""
    out = [[] for _ in marks]
    for g in gestures:
        k = sum(1 for m in marks if m <= g[0]) - 1
        if k >= 0:
            out[k].append(g)
    return [p for p in out if p]


def report(paths):
    gestures, timeline, waits, loads = frames.parse(paths)
    marks = marks_of(paths)
    phases = phases_marked(gestures, marks) if marks else phases_of(gestures)
    result = []
    for number, phase in enumerate(phases):
        all_gestures = [g for p in phases for g in p]
        base = all_gestures.index(phase[0])
        gaps = []
        for k in range(2 if not marks and phase[0][1] == 4 else 0, len(phase)):
            gaps += moving_gaps(all_gestures, base + k, timeline)
        result.append((number, len(phase), summarize(gaps)))
    return result


# After a sleep the same gestures may not be slower than before it, by more than the noise of ~1000 frames: the frame
# rate not under 85% of the first phase's, late frames not 8 points more, the 95th percentile not 60% + 4 ms more (the same build twice
# differs by 1-2 points; a wake's widget reloads and the sleeps' own work run beside the gestures and add to it).
# (The first phase of a session is, if anything, the slower one: tools/frames.py's order effect.)
def verdict(phases):
    """[(ok, text)] for each phase after the first, against the first. phases: [(number, gestures, summary|None)]."""
    good = [p for p in phases if p[2] is not None and p[2]['frames'] >= MIN_FRAMES]   # (the unlock swipe after a sleep is no gesture set)
    if len(good) < 2:
        return [(False, 'fewer than two phases with frames: nothing to compare')]
    first, out = good[0][2], []
    for number, count, s in good[1:]:
        bad = []
        if s['fps'] < 0.85 * first['fps']:
            bad.append(f'{s["fps"]:.1f} fps against {first["fps"]:.1f}')
        if s['late'] > first['late'] + 8:
            bad.append(f'{s["late"]:.1f}% late against {first["late"]:.1f}%')
        if s['p95'] > first['p95'] * 1.6 + 4:
            bad.append(f'p95 {s["p95"]:.1f} ms against {first["p95"]:.1f}')
        out.append((not bad, f'phase {number}: ' + ('; '.join(bad) if bad else
                    f'{s["fps"]:.1f} fps, {s["late"]:.1f}% late, p95 {s["p95"]:.1f} ms (first: {first["fps"]:.1f}, {first["late"]:.1f}%, {first["p95"]:.1f})')))
    return out


def kernel_lines(paths):
    wanted = re.compile(r'autorefresh back|interrupt \(\d+\) did not come|the panel (dark|lit)|sleep: awake again|'
                        r'power_test:|NOT ASLEEP|cpufreq: over the last|HKGPU: .*(held|follows|does not take)')
    seen, out = set(), []
    for path in paths:
        with open(path, 'rb') as f:
            for raw in f:
                text = raw.decode('utf-8', 'replace').rstrip()
                if wanted.search(text) and text not in seen:
                    seen.add(text)
                    out.append(text)
    return out


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--self-test':
        return self_test()
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    letters = 'ABCDEFGHIJ'
    print(f'{"phase":6} {"gestures":>8} {"frames":>7} {"fps":>5} {"p50":>6} {"p95":>6} {"p99":>6} {"worst":>7} '
          f'{"late%":>6} {"v.late%":>7} {"missed":>6}')
    for number, count, s in report(sys.argv[1:]):
        name = letters[number] if number < len(letters) else str(number)
        if s is None:
            print(f'{name:6} {count:8} no frames')
            continue
        print(f'{name:6} {count:8} {s["frames"]:7} {s["fps"]:5.1f} {s["p50"]:6.1f} {s["p95"]:6.1f} {s["p99"]:6.1f} '
              f'{s["worst"]:7.1f} {s["late"]:6.1f} {s["very"]:7.1f} {s["missed"]:6}')
    for ok, text in verdict(report(sys.argv[1:])):
        print(f't-frames: {"PASS" if ok else "FAIL"} {text}')
    print('kernel:')
    for line in kernel_lines(sys.argv[1:]):
        print('  ' + line[:200])


def self_test():
    # two phases of 4 gestures, 30 s apart; the second one slower
    def gesture(t):
        return (t, 1, 0, 0, 0)
    gestures = [gesture(t) for t in (0, 3_000_000, 6_000_000, 9_000_000, 60_000_000, 63_000_000, 66_000_000, 69_000_000)]
    assert [len(p) for p in phases_of(gestures)] == [4, 4]
    # 3 frames each at 11.1 ms in phase 0, at 25 ms in phase 1 (the first gesture of phase 1 is left out)
    timeline = []
    for start, step in ((0, 11111), (3_000_000, 11111), (6_000_000, 11111), (9_000_000, 11111),
                        (63_000_000, 25000), (66_000_000, 25000), (69_000_000, 25000)):
        timeline += [dict(done=start + i * step) for i in range(40)]
    timeline.sort(key=lambda f: f['done'])
    gaps0 = sum((moving_gaps(gestures, i, timeline) for i in range(4)), [])
    gaps1 = sum((moving_gaps(gestures, i, timeline) for i in range(6, 8)), [])
    s0, s1 = summarize(gaps0), summarize(gaps1)
    s0['frames'] = 800
    assert s0['late'] == 0 and abs(s0['fps'] - 90) < 1, s0
    assert s1['late'] == 100 and s1['missed'] == len(gaps1), s1      # 25 ms: one refresh lost each
    assert all(ok for ok, _ in verdict([(0, 4, s0), (1, 4, dict(s0, frames=800, fps=88.0, late=2.0, p95=12.0))]))
    assert not verdict([(0, 4, s0), (1, 4, dict(s0, frames=800, fps=60.0))])[0][0]
    assert not verdict([(0, 4, s0), (1, 4, dict(s0, frames=800, late=20.0))])[0][0]
    print('report.py: self-test ok')


if __name__ == '__main__':
    main()
