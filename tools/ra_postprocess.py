#!/usr/bin/env python3
"""Post-process rust-project.json for prebuilt-RPM kernel trees.

Fedora's kernel-devel RPM ships $KDIR/rust as flat prebuilt .rmeta files with
no sources, so the kernel generator emits crates whose root_module does not
exist (core/alloc/std when rust-src is absent, or everything on an installed
tree). This script:

- drops crates whose root_module file is missing; RA resolves those names
  (core/alloc/std) against its own bundled sysroot instead.
- remaps dep crate indices after the drop (dropped targets become name-only
  extern edges).
- adds missing direct dep edges by scanning each module's top-level
  `use <name>::` imports (the generator derives deps from kbuild flags and
  misses e.g. a bare `use bindings::...`).

Usage: ra_postprocess.py <rust-project.json> [prebuilt_rust_dir]   (in place)
"""
import json
import os
import re
import sys

path = sys.argv[1]
d = json.load(open(path))
crates = d['crates']
keep = [i for i, c in enumerate(crates) if os.path.exists(c.get('root_module') or '')]
old2new = {i: n for n, i in enumerate(keep)}
dropped = [c['display_name'] for i, c in enumerate(crates) if i not in old2new]

final = []
for i, c in enumerate(crates):
    if i not in old2new:
        continue
    deps = []
    for dep in c.get('deps', []):
        idx = dep.get('crate')
        nd = {"name": dep["name"]} if (idx is None or idx not in old2new) else dict(dep, crate=old2new[idx])
        deps.append(nd)
    cc = {k: v for k, v in c.items() if k != 'deps'}
    cc['deps'] = deps
    final.append(cc)

SKIP = {'self', 'super', 'crate'}
use_re = re.compile(r'\s*use\s+([A-Za-z_][A-Za-z0-9_]*)\b')
for c in final:
    rm = c.get('root_module') or ''
    if not os.path.exists(rm):
        continue
    names = set()
    for line in open(rm, errors='ignore'):
        m = use_re.match(line)
        if m and m.group(1) not in SKIP:
            names.add(m.group(1))
    have = {x.get('name') for x in c['deps']}
    by_name = {cc['display_name']: i for i, cc in enumerate(final)}
    # Only add edges that resolve to a project crate; rust-analyzer's schema
    # requires the `crate` index on every dep, and unknown extern names are
    # resolved by RA itself (bundled sysroot / precompiled search).
    for n in sorted(names - have):
        if n in by_name:
            c['deps'].append({"name": n, "crate": by_name[n]})

# The generator records proc-macro dylibs at <objtree>/rust/lib<crate>.so; for
# an unbuilt source clone those paths do not exist. Rewrite them to prebuilt
# copies in the fallback dir (the installed $KDIR/rust) when available.
fallback = sys.argv[2] if len(sys.argv) > 2 else None
if fallback:
    for c in final:
        p = c.get('proc_macro_dylib_path')
        if p and not os.path.exists(p):
            alt = os.path.join(fallback, f"lib{c['display_name']}.so")
            if os.path.exists(alt):
                c['proc_macro_dylib_path'] = alt

d['crates'] = final
json.dump(d, open(path, 'w'), indent=1)
print(f"ra_postprocess: {len(final)} crates kept, dropped={dropped}")
