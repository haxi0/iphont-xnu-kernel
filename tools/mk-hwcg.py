#!/usr/bin/env python3
"""mk-hwcg.py A6XX_GPU_C A6XX_XML_H OUT_H: the A650's hardware clock gating table (bsd/dev/hk_a650_hwcg.h) from Linux's
a6xx_gpu.c (a650_hwcg[], git.kernel.org .../plain/drivers/gpu/drm/msm/adreno/a6xx_gpu.c?h=v6.6) and Mesa's generated a6xx.xml.h
(register names to dword offsets). Check: 49 rows, none missing."""
import re
import sys

src, xml, out = (open(sys.argv[1]).read(), open(sys.argv[2]).read(), sys.argv[3])
a = src.index('const struct adreno_reglist a650_hwcg[]')
rows = re.findall(r'\{\s*(REG_A6XX_\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}', src[a:src.index('};', a)])
off = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'#define (REG_A6XX_\w+)\s+(0x[0-9a-fA-F]+)', xml)}
missing = [n for n, _ in rows if n not in off]
assert not missing, missing
lines = ["/*", " * The A650 (SM8250) hardware clock gating table: Linux drivers/gpu/drm/msm/adreno/a6xx_gpu.c (v6.6) a650_hwcg[], registers",
         " * as dword offsets from Mesa's a6xx.xml.h. Written by bsd/dev/hk_adreno.c hw_init (a6xx_set_hwcg). Generated: tools/mk-hwcg.py.",
         " */", "static const struct { uint16_t reg; uint32_t value; } a650_hwcg[] = {"]
lines += ['\t{ 0x%04x, %s },\t/* %s */' % (off[n], v, n[len('REG_A6XX_'):]) for n, v in rows]
open(out, 'w').write('\n'.join(lines + ['};', '']))
print(len(rows), 'rows')
