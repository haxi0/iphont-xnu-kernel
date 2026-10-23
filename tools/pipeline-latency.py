#!/usr/bin/env python3
"""Summarize touch_lat=1 stages from ONE run (including its live-log snapshots).

With fb_push (instantnoodle), "push" stages end when PINGPONG_0 has sent the frame to the panel's RAM;
the panel shows it on its next scan. Photons are still not measured.
No touch-to-frame pairing is inferred: a frame after a touch may contain unrelated content.
"""
import math
from pathlib import Path
import re
import statistics
import sys


PATTERNS = {
    "input": r"tl: i (\d+) posted (\d+) read (\d+) sent (\d+)",
    "touch": r"tl: t irq (\d+) worker (\d+) done (\d+) events (\d+) error (\d+)",
    "bus": r"tl: b (\S+) begin (\d+) locked (\d+) done (\d+) bytes (\d+) error (\d+)",
    "swap": r"tl: s (\d+) at (\d+)",
    "dequeue": r"tl: d (\d+) began (\d+) ready (\d+)",
    "copy": r"tl: v (\d+) ready (\d+) shown (\d+) done (\d+)",
    "push": r"tl: k (\d+) kick (\d+) done (\d+)",
    "poll": r"tl: c before (\d+) seen (\d+) read (\d+)",
}


def stages(lines):
    values, swaps, dequeued, copies, pushes = {}, {}, {}, {}, {}
    missed, errors = 0, 0

    def add(name, delta):
        if delta < 0:
            raise ValueError(f"{name}: negative duration {delta} us; check for mixed runs or incomplete lines")
        values.setdefault(name, []).append(delta)

    for line in set(lines):
        for kind, pattern in PATTERNS.items():
            match = re.search(pattern, line)
            if not match:
                continue
            if kind == "bus":
                bus = match[1]
                begin, locked, done, count, error = map(int, match.groups()[1:])
                add(bus + " lock wait", locked - begin)
                add(bus + " transfer", done - locked)
                errors += bool(error)
                continue
            nums = tuple(map(int, match.groups()))
            if kind == "input":
                _, posted, read, sent = nums
                add("kernel post -> HID read", read - posted)
                add("HID read -> dispatch return", sent - read)
            elif kind == "touch":
                irq, worker, done, count, error = nums
                if irq:
                    add("touch IRQ -> worker", worker - irq)
                elif count:
                    missed += 1
                add("touch worker -> reports posted", done - worker)
            elif kind == "swap":
                swaps[nums[0]] = nums[1]
            elif kind == "dequeue":
                dequeued[nums[0]] = nums[1:]
            elif kind == "copy":
                copies[nums[0]] = nums[1:]
            elif kind == "push":
                pushes[nums[0]] = nums[1:]
            elif kind == "poll" and nums[0]:
                add("touch poll period (bounds detection delay)", nums[1] - nums[0])
    for sid, (ready, shown, done) in copies.items():
        if sid in dequeued:
            began, gpu_ready = dequeued[sid]
            if sid in swaps:
                add("SwapEnd -> presenter (queue)", began - swaps[sid])
            add("presenter GPU wait", gpu_ready - began)
        add("pacing wait (beat or previous push)", shown - ready)
        add("CPU scaling/copy", done - shown)
        if sid in swaps:
            add("SwapEnd -> CPU copy complete", done - swaps[sid])
    for sid, (kick, done) in pushes.items():
        add("push kick -> panel RAM written (TE + transfer)", done - kick)
        if sid in swaps:
            add("SwapEnd -> panel RAM written", done - swaps[sid])
    return values, missed, errors


def records(text):
    """The tl: records of a log. A record another one starts inside was cut short: dropped."""
    out = []
    # a live snapshot can stop in the middle of its last line: never a sample
    for line in text.splitlines()[:None if text.endswith("\n") else -1]:
        at = line.rfind("tl: ")
        if at >= 0:
            out.append(line[at:])
    return out


def self_test():
    lines = [
        "tl: b se13 begin 10 locked 20 done 100 bytes 8 error 0",
        "tl: t irq 5 worker 10 done 105 events 1 error 0",
        "tl: i 1 posted 100 read 110 sent 120",
        "tl: s 1 at 130", "tl: d 1 began 140 ready 150",
        "tl: v 1 ready 150 shown 160 done 170",
        "tl: k 1 kick 171 done 200",
        "tl: c before 300 seen 310 read 320",
        "tl: t irq 0 worker 200 done 250 events 1 error 0",
    ]
    values, missed, errors = stages(lines + lines)
    assert values["se13 lock wait"] == [10]
    assert values["touch IRQ -> worker"] == [5]
    assert values["kernel post -> HID read"] == [10]
    assert values["SwapEnd -> presenter (queue)"] == [10]
    assert values["presenter GPU wait"] == [10]
    assert values["pacing wait (beat or previous push)"] == [10]
    assert values["push kick -> panel RAM written (TE + transfer)"] == [29]
    assert values["SwapEnd -> panel RAM written"] == [70]
    assert values["touch poll period (bounds detection delay)"] == [10]
    assert values["CPU scaling/copy"] == [10]
    assert values["SwapEnd -> CPU copy complete"] == [40]
    assert missed == 1 and errors == 0
    assert stages([]) == ({}, 0, 0)
    spliced = "tl: i 2 posted 10 read 20 sent 1tl: v 9 ready 1 shown 2 done 3\n322219683\ntl: s 4 at 5"
    assert records(spliced) == ["tl: v 9 ready 1 shown 2 done 3"]
    try:
        stages(["tl: i 1 posted 100 read 90 sent 120"])
    except ValueError:
        pass
    else:
        raise AssertionError("negative timing was silently accepted")
    print("PASS: stage attribution, duplicate snapshots, missing IRQ timestamps, empty logs")


def main():
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lines = []
    for name in sys.argv[1:]:
        lines += records(Path(name).read_text(errors="replace"))
    values, missed, errors = stages(lines)
    if not values:
        sys.exit("No timing samples. Capture a run with touch_lat=1; existing aggregate GPU logs are insufficient.")
    print("Stage                                      n   median      p95      max (ms)")
    for name, samples in sorted(values.items()):
        ordered = sorted(samples)
        p95 = ordered[math.ceil(len(ordered) * 0.95) - 1]
        print(f"{name:40s} {len(samples):5d} {statistics.median(samples)/1000:8.3f} "
              f"{p95/1000:8.3f} {max(samples)/1000:8.3f}")
    print(f"Touch batches without IRQ timestamps: {missed} (polling or watchdog); I2C read errors: {errors}.")
    print("Panel scan-out after its RAM is written, and input-to-photon latency, remain unmeasured.")


if __name__ == "__main__":
    main()
