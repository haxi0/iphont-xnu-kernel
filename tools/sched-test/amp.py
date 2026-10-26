#!/usr/bin/env python3
"""
tools/sched-test/amp.py FILE [FILE2]: the kernel's scheduler snapshots (kern.hackios.amp, written by `sched_test amp LABEL`
into a tools/t run's logs/t-*-amp.txt) made readable: what happened between each snapshot and the next -- energy by
cluster (the controller's model), busy time, where the Edge scheduler put threads (placed away, steals, pulls, moves),
scheduling latency percentiles by class, the controller's decisions -- and the busiest thread groups.
With FILE2 (e.g. the same tools/t check under sched=clutch): both side by side, interval by interval.
  --self-test   checks the parser on a built-in snapshot pair
"""
import re
import sys

BINS = [20, 50, 100, 250, 1000, 4000, 16000, 10 ** 9]


def parse(path_or_text, is_text=False):
    text = path_or_text if is_text else open(path_or_text, errors='replace').read()
    snaps, cur, last_label = [], None, None
    for line in text.splitlines():
        m = re.match(r't-amp (\S+): (.*)', line)
        if not m:
            continue
        label, body = m.groups()
        if body.startswith('amp: ') or cur is None or label != last_label:
            cur = {'label': label, 'clusters': {}, 'energy': [], 'lat': {}, 'groups': [], 'edge_from': {}}
            snaps.append(cur)
            last_label = label
        if body.startswith('amp: '):
            cur['head'] = body
        elif body.startswith('cluster '):
            f = body.split()
            c = int(f[1])
            d = dict(zip(f[3::2], f[4::2]))
            cur['clusters'][c] = {'type': f[2], 'busyms': int(re.search(r'busyms (\d+)', body).group(1)),
                                  'mhz': int(re.search(r' mhz (\d+)', body).group(1)), 'cap': int(d.get('cap', 0))}
        elif body.startswith('energy mj'):
            m2 = re.match(r'energy mj ([\d ]+) total (\d+)', body)
            if m2:
                cur['energy'] = [int(x) for x in m2.group(1).split()]
                cur['energy_total'] = int(m2.group(2))
        elif body.startswith('latency ') and not body.startswith('latency bins'):
            f = body.split()
            cur['lat'][(f[1], int(f[2]))] = [int(x) for x in f[3:]]
        elif body.startswith('edge from'):
            m2 = re.match(r'edge from (\d+): away ([\d ]+) steal ([\d ]+) pull ([\d ]+) moves ([\d ]+)', body)
            if m2:
                cur['edge_from'][int(m2.group(1))] = {k: [int(x) for x in m2.group(i + 2).split()] for i, k in
                                                      enumerate(('away', 'steal', 'pull', 'moves'))}
        elif body.startswith('edge placed'):
            m2 = re.match(r'edge placed ([\d ]+) rebalance_ipi ([\d ]+) amp_rebalance ([\d ]+) tg_migrated (\d+)', body)
            if m2:
                cur['placed'] = [int(x) for x in m2.group(1).split()]
                cur['rebalance'] = [int(x) for x in m2.group(2).split()]
                cur['amp_rebalance'] = [int(x) for x in m2.group(3).split()]
                cur['tg_migrated'] = int(m2.group(4))
        elif body.startswith('controller '):
            cur['controller'] = dict(re.findall(r'(\w+) (\d+)', body))
        elif body.startswith('group '):
            m2 = re.match(r'group (\d+) (\S+) flags (\S+) level (\S+)(?: \(prime\))? demand (\S+) .*workms (\d+) runms (\d+) moves (\d+)'
                          r'.*?(?:mj (\d+))?$', body)
            if m2:
                cur['groups'].append({'id': int(m2.group(1)), 'name': m2.group(2), 'level': m2.group(4), 'demand': m2.group(5),
                                      'workms': int(m2.group(6)), 'runms': int(m2.group(7)), 'moves': int(m2.group(8)),
                                      'mj': int(m2.group(9) or 0)})
    return snaps


def pct(hist, p):
    total = sum(hist)
    if not total:
        return None
    acc = 0
    for count, top in zip(hist, BINS):
        acc += count
        if acc >= total * p / 100:
            return top
    return BINS[-1]


def interval(a, b):
    out = {'label': f'{a["label"]} -> {b["label"]}'}
    if a.get('energy') and b.get('energy'):
        out['energy'] = [y - x for x, y in zip(a['energy'], b['energy'])]
        out['energy_total'] = b['energy_total'] - a['energy_total']
    out['busy'] = {c: b['clusters'][c]['busyms'] - a['clusters'].get(c, {}).get('busyms', 0) for c in b['clusters']}
    out['type'] = {c: b['clusters'][c]['type'] for c in b['clusters']}
    lat = {}
    for key, hist in b['lat'].items():
        d = [y - x for x, y in zip(a['lat'].get(key, [0] * len(hist)), hist)]
        cls = key[0]
        lat.setdefault(cls, [0] * len(d))
        lat[cls] = [x + y for x, y in zip(lat[cls], d)]
    out['lat'] = {k: (sum(v), pct(v, 50), pct(v, 99)) for k, v in lat.items()}
    if a.get('placed') and b.get('placed'):
        out['placed'] = [y - x for x, y in zip(a['placed'], b['placed'])]
        out['away'] = {s: [y - x for x, y in zip(a['edge_from'][s]['away'], b['edge_from'][s]['away'])] for s in b['edge_from']}
        out['steal'] = sum(sum(b['edge_from'][s]['steal']) - sum(a['edge_from'][s]['steal']) for s in b['edge_from'])
        out['moves'] = sum(sum(b['edge_from'][s]['moves']) - sum(a['edge_from'][s]['moves']) for s in b['edge_from'])
    ga = {g['id']: g for g in a['groups']}
    gd = []
    for g in b['groups']:
        prev = ga.get(g['id'], {'workms': 0, 'mj': 0, 'runms': 0})
        gd.append((g['runms'] - prev['runms'], g['mj'] - prev['mj'], g['name'], g['level']))
    out['groups'] = sorted(gd, reverse=True)[:6]
    return out


def show(iv):
    print(f'== {iv["label"]}')
    if 'energy' in iv:
        print(f'   energy {iv["energy_total"]} mJ (model): ' + ', '.join(f'c{c} {iv["type"].get(c, "?")} {e}' for c, e in enumerate(iv['energy'])))
    print('   busy ms: ' + ', '.join(f'c{c} {iv["type"][c]} {v}' for c, v in sorted(iv['busy'].items())))
    if 'placed' in iv:
        away = ', '.join(f'{s}>' + '/'.join(str(x) for x in v) for s, v in sorted(iv['away'].items()))
        print(f'   placed by preferred cluster {iv["placed"]}; put elsewhere {away}; steals {iv["steal"]}; moves {iv["moves"]}')
    print('   latency (wakes, p50/p99 us bin): ' + ', '.join(f'{k} {n} {p50}/{p99}' for k, (n, p50, p99) in sorted(iv['lat'].items()) if n))
    for run, mj, name, level in iv['groups']:
        print(f'   group {name:24} {run:6} ms {mj:5} mJ level {level}')


def main(args):
    if args == ['--self-test']:
        a = ('t-amp x: amp: scheduler edge, controller on, calibrated 1, ticks 1, screen on\n'
             't-amp x: cluster 0 P cpus 0x1 domain 2 A77 l2 512 cap 1024 dpc 642 maxmhz 2841 mhz 844 util 1 avail 1 runq 0 foreign 0 busyms 10 load 0 0 0 0 0 0 exec 0 0 0 0 0 0\n'
             't-amp x: energy mj 5 total 5 mw 100\n'
             't-amp x: latency fg 0 1 1 0 0 0 0 0 0\n')
        b = a.replace('t-amp x', 't-amp y').replace('busyms 10', 'busyms 30').replace('mj 5 total 5', 'mj 9 total 9').replace(
            'latency fg 0 1 1 0', 'latency fg 0 1 9 0')
        s = parse(a + b, True)
        iv = interval(s[0], s[1])
        ok = len(s) == 2 and iv['energy_total'] == 4 and iv['busy'][0] == 20 and iv['lat']['fg'] == (8, 50, 50)
        print('amp.py self-test:', 'PASS' if ok else f'FAIL {iv}')
        return 0 if ok else 1
    if not args:
        print(__doc__)
        return 2
    runs = [parse(p) for p in args[:2]]
    for i in range(max(len(r) for r in runs) - 1):
        for path, snaps in zip(args, runs):
            if i + 1 < len(snaps):
                print(f'[{path}]')
                show(interval(snaps[i], snaps[i + 1]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
