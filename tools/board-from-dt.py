#!/usr/bin/env python3
"""
The board_ addresses of a phone (bsd/sys/hk_board.h), read out of its own device tree.

usage: tools/board-from-dt.py DEVICE.dtb        prints  board_epss=0x... board_tsens0=0x... ...
       (common/new-device.sh puts that into the profile as BOARD_ARGS)

What is read, from the Qualcomm downstream device tree every Android phone of these SoCs carries:
  epss     qcom,cpufreq-epss / qcom,cpufreq-hw         "freq-domain0" (else its first reg)
  tsens0/1 the TSENS blocks ("tsens" in compatible)    "tsens_tm_physical" (else the second reg), in address order
  ufshc    the UFS host controller (qcom,ufshc)        "ufs_mem" (else the first reg)
  gcc      the global clock controller (qcom,gcc-...)  first reg
  dispcc   the display clock controller (dispcc)       first reg
  spmi_*   the PMIC arbiter (qcom,spmi-pmic-arb)       "core", "cnfg", "obsrvr"
  cmd_db   RPMh's command database (qcom,cmd-db)       first reg
A block that is not found is left out: the kernel keeps the brought-up phone's address for it, and the
driver will say so if that is wrong. Nothing here is guessed.
"""
import re, subprocess, sys


def nodes(dts):
    """(name, body) of every node, the body without its children"""
    out, stack = [], []
    for line in dts.split('\n'):
        m = re.match(r'^\s*([\w,.@+-]+) \{\s*$', line)
        if m:
            stack.append([m.group(1), []])
            continue
        if re.match(r'^\s*\};\s*$', line):
            if stack:
                name, body = stack.pop()
                out.append((name, '\n'.join(body)))
            continue
        if stack:
            stack[-1][1].append(line)
    return out


def prop(body, key):
    m = re.search(r'^\s*' + re.escape(key) + r' = (.*);\s*$', body, re.M)
    return m.group(1) if m else None


def regs(body):
    """[(name or None, address)] from reg (and reg-names): one or two cells an address, the same for a size"""
    r = prop(body, 'reg')
    if not r:
        return []
    cells = [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', r)]
    names = re.findall(r'"([^"]*)"', prop(body, 'reg-names') or '')
    n = len(names) or 1
    per = len(cells) // n if len(cells) % n == 0 else 2
    if not names:
        per = 4 if len(cells) % 4 == 0 and len(cells) >= 4 and cells[0] == 0 else 2
        n = len(cells) // per
    out = []
    for i in range(n):
        c = cells[i * per:(i + 1) * per]
        addr = c[0] if per == 2 else (c[0] << 32) | c[1]
        out.append((names[i] if i < len(names) else None, addr))
    return out


def pick(rs, want=None, index=0):
    for name, a in rs:
        if want and name == want:
            return a
    return rs[index][1] if len(rs) > index else None


def main():
    dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', sys.argv[1]], capture_output=True, text=True).stdout
    board, tsens = {}, []
    # enabled nodes first: a block's address is the same whether a device tree turns its node off or not (the
    # one a UEFI port carries does, for blocks the UEFI does not use), but an enabled node is the better witness
    every = nodes(dts)
    every = [n for n in every if prop(n[1], 'status') != '"disabled"'] + [n for n in every if prop(n[1], 'status') == '"disabled"']
    for name, body in every:
        comp = prop(body, 'compatible') or ''
        rs = regs(body)
        if not rs:
            continue
        if ('qcom,cpufreq-epss' in comp or 'qcom,cpufreq-hw' in comp) and 'epss' not in board:
            board['epss'] = pick(rs, 'freq-domain0')
        elif 'tsens' in comp:
            a = pick(rs, 'tsens_tm_physical', 1)
            if a not in tsens:
                tsens.append(a)
        elif 'qcom,ufshc' in comp and 'ufshc' not in board:
            board['ufshc'] = pick(rs, 'ufs_mem')
        elif re.search(r'"qcom,[a-z0-9]*-?gcc(-[a-z0-9]+)?"|"qcom,gcc-', comp) and 'gcc' not in board:
            board['gcc'] = pick(rs)
        elif 'dispcc' in comp and 'dispcc' not in board:
            board['dispcc'] = pick(rs)
        elif 'qcom,spmi-pmic-arb' in comp and 'spmi_core' not in board:
            for key, want in (('spmi_core', 'core'), ('spmi_cnfg', 'cnfg'), ('spmi_obsrvr', 'obsrvr')):
                a = pick(rs, want, -1)
                if a is not None and any(n == want for n, _ in rs):
                    board[key] = a
        elif 'qcom,cmd-db' in comp and 'cmd_db' not in board:
            board['cmd_db'] = pick(rs)
    for i, a in enumerate(sorted(a for a in tsens if a is not None)[:2]):
        board['tsens%d' % i] = a
    print(' '.join('board_%s=%#x' % (k, v) for k, v in sorted(board.items()) if v is not None))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main()
