"""
The run's log as data: where each line came from, how bad it is, and the records the system already writes
into it (init's checks, the tests' marks, user-space crashes, kernel panics).

A run's log (logs/HHMMSS-run.log, or `oem log` during a run) is the loader's own lines, then
"== KERNEL LOG ==" and everything the kernel and the programs wrote to the console, then on the way out
"--- kernel message buffer ---" and, after a panic, "--- panic buffer ---". Lines are "component: text"
by convention (`adreno: step 4: ...`, `ufs: ...`); the component is that prefix when it looks like one.
"""
import re

SECTION_MARKS = [
    (re.compile(r'^== XNULOADER'), 'loader'),
    (re.compile(r'^== KERNEL LOG(?: ==| \()'), 'kernel'),
    (re.compile(r'^=== init: '), 'init'),
    (re.compile(r'^=== /hackios/etc/rc ==='), 'rc'),
    (re.compile(r'^=== /hackios/etc/rc\.jobs ==='), 'jobs'),
    (re.compile(r'^--- kernel message buffer ---'), 'msgbuf'),
    (re.compile(r'^--- panic buffer ---'), 'panicbuf'),
]
COMPONENT = re.compile(r'^([A-Za-z_][\w.+-]{0,31}(?: [\w.+-]{1,16})?)(?:\[\d+\])?: ')
# severity by what the line says: the code base does not tag its lines, and these words are how it reports trouble
SEVERITY = [
    ('critical', re.compile(r'\bpanic\(|Kernel data abort|\bCrashDump\b|\bhung\b|kernel did not come back', re.I)),
    ('error', re.compile(r'\b(error|failed|fails|FAIL|cannot|could not|refused|refuses|timed? ?out|fault|no answer|'
                         r'not found|invalid|denied|abort(ed|s)?|crash(ed)?|died|exception)\b|\[FAIL\]', re.I)),
    ('warning', re.compile(r'\b(warn(ing)?|retry(ing)?|again|fallback|fell back|dropped|lost|late|stale|slow|'
                           r'unexpected|unknown|missing|not yet)\b', re.I)),
]
LEVELS = ['debug', 'info', 'warning', 'error', 'critical']


OSLOG_LEVEL = re.compile(r'^[\w.+-]+(?:\[\d+\])?: (debug|info|notice|default|error|fault):')
OSLOG_MAP = {'debug': 'debug', 'info': 'info', 'notice': 'info', 'default': 'info', 'error': 'error', 'fault': 'error'}


def severity(text):
    m = OSLOG_LEVEL.match(text)        # os_log's own level, where the line carries it ("CoreData: debug: ...")
    if m:
        return OSLOG_MAP[m.group(1)]
    for name, rx in SEVERITY:
        if rx.search(text):
            return name
    return 'info'


def lines(text):
    """Every line with its number (1-based), section, component and severity."""
    section = 'loader'
    out = []
    for n, line in enumerate(text.split('\n'), 1):
        for rx, name in SECTION_MARKS:
            if rx.match(line):
                section = name
                break
        m = COMPONENT.match(line)
        out.append({'n': n, 'section': section, 'component': m.group(1) if m else None,
                    'severity': severity(line), 'severity_source': 'os_log' if OSLOG_LEVEL.match(line) else 'inferred', 'text': line})
    return out


def select(records, component=None, min_severity=None, grep=None, section=None, since=None, exclude=('tl',)):
    """Filter line records. component and section may be comma lists; grep is a regular expression."""
    comps = set(component.split(',')) if component else None
    secs = set(section.split(',')) if section else None
    rank = LEVELS.index(min_severity) if min_severity else 0
    rx = re.compile(grep) if grep else None
    for r in records:
        if since is not None and r['n'] <= since:
            continue
        if comps is not None and r['component'] not in comps:
            continue
        if comps is None and exclude and r['component'] in exclude:
            continue                    # touch-latency lines (tl:) are tens of thousands: asked for by name only
        if secs is not None and r['section'] not in secs:
            continue
        if LEVELS.index(r['severity']) < rank:
            continue
        if rx is not None and not rx.search(r['text']):
            continue
        yield r


def components(records):
    counts = {}
    for r in records:
        c = r['component'] or '-'
        e = counts.setdefault(c, {'lines': 0, 'error': 0, 'warning': 0, 'critical': 0})
        e['lines'] += 1
        if r['severity'] in e:
            e[r['severity']] += 1
    return dict(sorted(counts.items(), key=lambda kv: -kv[1]['lines']))


def checks(text):
    """init's own checks ("[ ok ] what" / "[FAIL] what") and its summary line."""
    items = [{'ok': m.group(1) == ' ok ', 'what': m.group(2)} for m in re.finditer(r'^\[( ok |FAIL)\] (.*)$', text, re.M)]
    m = re.search(r'^=== (\d+) checks, (\d+) failed ===', text, re.M)
    return {'checks': items, 'total': int(m.group(1)) if m else len(items),
            'failed': int(m.group(2)) if m else sum(not i['ok'] for i in items), 'summary_seen': bool(m)}


def marks(text):
    """The tests' progress marks (`t-mark: NAME`) and verdict lines (`t-xxx: PASS|FAIL ...`)."""
    return {'marks': re.findall(r't-mark: (\S+)', text),
            'verdicts': [{'check': m.group(1), 'pass': m.group(2) == 'PASS', 'text': m.group(3).strip()}
                         for m in re.finditer(r'^t-([\w-]+): (PASS|FAIL)(.*)$', text, re.M)]}


def crashes(text):
    """
    User-space crashes as the kernel already reports them (osfmk/arm64/user_exception_trace.c, boot-arg exctrace=1
    or systrace=1): "usertrap: NAME[PID] exception E code A B: pc .. lr .. sp .., shared cache slide S" followed by
    "usertrap:   x0 -> "text"" and "usertrap:   called from ADDR" lines; and abort() calls ("userabort: ...").
    Also programs that init or launchd saw end by a signal.
    """
    out, cur = [], None
    for line in text.split('\n'):
        m = re.match(r'usertrap: (.+?)\[(-?\d+)\] exception (\d+) code (0x[0-9a-f]+) (0x[0-9a-f]+): pc (0x[0-9a-f]+) '
                     r'lr (0x[0-9a-f]+) sp (0x[0-9a-f]+), shared cache slide (0x[0-9a-f]+)', line)
        if m:
            cur = {'kind': 'exception', 'process': m.group(1), 'pid': int(m.group(2)), 'exception': int(m.group(3)),
                   'exception_name': EXCEPTIONS.get(int(m.group(3)), '?'), 'code': [m.group(4), m.group(5)],
                   'pc': m.group(6), 'lr': m.group(7), 'sp': m.group(8), 'slide': m.group(9), 'messages': [],
                   'frames': []}
            out.append(cur)
            continue
        if cur is not None:
            m = re.match(r'usertrap:   x(\d+) -> "(.*)"$', line)
            if m:
                cur['messages'].append({'register': 'x' + m.group(1), 'text': m.group(2)})
                continue
            m = re.match(r'usertrap:   called from (0x[0-9a-f]+)', line)
            if m:
                cur['frames'].append(m.group(1))
                continue
            cur = None
        m = re.match(r'userabort: (.+?)\[(-?\d+)\] aborts, shared cache slide (0x[0-9a-f]+); called from:((?: 0x[0-9a-f]+)*)', line)
        if m:
            out.append({'kind': 'abort', 'process': m.group(1), 'pid': int(m.group(2)), 'slide': m.group(3),
                        'frames': m.group(4).split()})
            continue
        m = re.search(r'Failed to send exception EXC_CORPSE_NOTIFY\. error code: \d+ for pid (\d+)', line)
        if m:
            out.append({'kind': 'corpse', 'pid': int(m.group(1)), 'process': None,
                        'note': 'a process crashed (no crash reporter took its corpse); boot-arg exctrace=1 prints where'})
            continue
        m = re.match(r'\[killed by signal (\d+)\]', line)
        if m:
            out.append({'kind': 'signal', 'signal': int(m.group(1)), 'process': None})
    return out


EXCEPTIONS = {1: 'EXC_BAD_ACCESS', 2: 'EXC_BAD_INSTRUCTION', 3: 'EXC_ARITHMETIC', 6: 'EXC_BREAKPOINT',
              10: 'EXC_CRASH', 11: 'EXC_RESOURCE', 12: 'EXC_GUARD'}


def panics(text):
    """Kernel panics: the message, registers, backtrace addresses and the identity lines XNU prints."""
    out = []
    for m in re.finditer(r'^panic\(cpu (\d+) caller (0x[0-9a-f]+)\): (.*)$', text, re.M):
        block = text[m.start():m.start() + 20000]
        p = {'cpu': int(m.group(1)), 'caller': m.group(2), 'message': m.group(3).strip(), 'registers': {},
             'backtrace': []}
        for rm in re.finditer(r'\b(x\d+|fp|lr|sp|pc|cpsr|esr|far):\s+(0x[0-9a-f]+)', block[:4000]):
            p['registers'].setdefault(rm.group(1), rm.group(2))
        for key, rx in (('kernel_version', r'^Kernel version: (.*)$'), ('kernel_uuid', r'^Kernel UUID: (.*)$'),
                        ('text_base', r'^Kernel text base:\s+(0x[0-9a-f]+)'), ('boot_args', r'^Boot args: (.*)$'),
                        ('mach_absolute_time', r'^mach_absolute_time: (0x[0-9a-f]+)')):
            km = re.search(rx, block, re.M)
            if km:
                p[key] = km.group(1).strip()
        p['backtrace'] = re.findall(r'lr: (0x[0-9a-f]+)\s+fp: 0x[0-9a-f]+', block)
        pm = re.search(r'^Panicked task [^\n]*\n(?:[^\n]*\n){0,2}', block, re.M)
        if pm:
            p['task'] = pm.group(0).strip()
        out.append(p)
    return out


def summary(text):
    recs = lines(text)
    sev = {}
    for r in recs:
        if r['component'] != 'tl':
            sev[r['severity']] = sev.get(r['severity'], 0) + 1
    end = None
    for rx, why in ((r'--- the host asked for the end of the run', 'stopped by the host'),
                    (r'--- \d+ fingers held', 'five-finger exit'),
                    (r'^panic\(cpu', 'panic'), (r'loader_return_after|time is up', 'timed return')):
        if re.search(rx, text, re.M):
            end = why
            break
    return {'lines': len(recs), 'bytes': len(text.encode('utf-8')), 'characters': len(text), 'severity': sev, 'sections': sorted({r['section'] for r in recs}),
            'panics': len(panics(text)), 'crashes': len(crashes(text)), 'init': checks(text), 'marks': marks(text),
            'end': end}
