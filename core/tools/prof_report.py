#!/usr/bin/env python3
# Country-Box: reads a sampling profile (MP4_PROFILE=<path>, runtime/prof_linux.c) and names where the time went.
# Each sampled Rip is resolved against the executable's linker map (country-box.map, written by tools/build_linux.py):
# a lifted guest function f_<guest address> becomes main+<offset> and, when an IL2CPP symbol table is given
# (<work>/il2cpp/symbols.tsv: offset, class, method), its C# name; anything else is a runtime (host) function.
#
#   python3 tools/prof_report.py PROFILE --map build/country-box.map [--symbols symbols.tsv] [--top 40]
#
# Output: samples per thread, the share of time in guest code vs runtime, the top functions, and the top C# classes
# (guest functions grouped by class), each with its share of all samples.
import argparse
import bisect
import re
from collections import Counter
from pathlib import Path

MAIN_BASE = 0x7000000000   # where the loader places main (run.log: "loaded main at ...")


def read_map(path):
    syms = []
    for line in open(path, errors='replace'):
        parts = line.split(None, 4)
        if len(parts) != 5:
            continue
        name = parts[4].strip()
        if name.startswith('.') or ':(' in name or name.startswith('<') or '/' in name:
            continue
        try:
            syms.append((int(parts[0], 16), int(parts[2], 16), name))
        except ValueError:
            continue
    syms.sort()
    return syms


def read_symbols(path):
    out = {}
    if not path:
        return out
    for line in open(path, errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 3:
            try:
                out[int(f[0], 16)] = (f[1], f[2])
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--map', required=True)
    ap.add_argument('--symbols')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()

    base, total, rips, threads = 0, 0, Counter(), []
    for line in open(a.profile):
        f = line.split()
        if not f:
            continue
        if f[0] == 'base':
            base = int(f[1], 16)
        elif f[0] == 'samples':
            total = int(f[1])
        elif f[0] == 'thread':
            threads.append((int(f[2]), f[1], int(f[3]) if len(f) > 3 else 0))
        elif f[0] == 'rip':
            rips[int(f[1], 16)] += int(f[2])
    syms = read_map(a.map)
    starts = [s[0] for s in syms]
    il2 = read_symbols(a.symbols)
    il2_starts = sorted(il2)

    by_fn, by_class, guest, host, unknown = Counter(), Counter(), 0, 0, 0
    for rip, n in rips.items():
        off = rip - base
        i = bisect.bisect_right(starts, off) - 1
        if i < 0 or off >= syms[i][0] + max(syms[i][1], 1) + 0x100000:
            by_fn['(outside the executable: libraries, kernel, driver)'] += n
            unknown += n
            continue
        name = syms[i][2]
        m = re.fullmatch(r'f_([0-9a-f]+)', name)
        if m:
            guest += n
            ga = int(m.group(1), 16)
            mo = ga - MAIN_BASE
            j = bisect.bisect_right(il2_starts, mo) - 1
            if il2 and j >= 0 and mo - il2_starts[j] < 0x4000:
                cls, meth = il2[il2_starts[j]]
                label = f'{cls}::{meth}' if il2_starts[j] == mo else f'{cls}::{meth} (+{mo - il2_starts[j]:#x})'
                by_class[cls] += n
            else:
                label = f'main+{mo:#x}' if 0 <= mo < 0x10000000 else name
                by_class['(no IL2CPP name: runtime library, sdk, unnamed)'] += n
            by_fn[label] += n
        else:
            host += n
            by_fn['[runtime] ' + name] += n

    seen = sum(rips.values()) or 1
    print(f'samples {total} (top Rips cover {seen}); guest code {100 * guest / seen:.1f}%, runtime {100 * host / seen:.1f}%, '
          f'outside {100 * unknown / seen:.1f}%')
    for tid, s, ins in sorted(threads, key=lambda t: -t[0])[:8]:
        print(f'  thread {s}: {tid} samples, {ins} in the executable')
    print(f'\ntop {a.top} functions (share of sampled time)')
    for label, n in by_fn.most_common(a.top):
        print(f'  {100 * n / seen:5.1f}%  {label}')
    if by_class:
        print(f'\ntop {a.top // 2} C# classes')
        for cls, n in by_class.most_common(a.top // 2):
            print(f'  {100 * n / seen:5.1f}%  {cls}')


if __name__ == '__main__':
    main()
