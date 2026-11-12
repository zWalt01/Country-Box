"""Country-Box: the figures --probe-loader expects (runtime/loader.c expect_dkcr), measured on an extracted ExeFS with
the lifter's NSO parser (recomp/nso.py) instead of the runtime's loader, so the probe compares two implementations.

  python3 tools/measure_modules.py <exefs>        (needs lz4: tools/requirements.txt)

Per module: relocation kinds (RELATIVE incl. DT_RELR, ABS64, GLOB_DAT, JUMP_SLOT), symbolic relocations against
undefined / defined symbols, undefined named symbols and which of the loaded modules defines each (first in load
order), weak ones nobody defines, DT_INIT and DT_INIT_ARRAYSZ / 8. Prints counts and offsets only, never code or data.
"""
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'recomp'))
from nso import Nso  # noqa: E402

DT_INIT, DT_INIT_ARRAYSZ = 12, 27
REL_KINDS = (1027, 257, 1025, 1026)   # R_AARCH64_RELATIVE, ABS64, GLOB_DAT, JUMP_SLOT
STB_WEAK = 2


def main():
    exefs = Path(sys.argv[1])
    names = [n for n in ('main', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'sdk') if (exefs / n).exists()]
    mods = {n: Nso(exefs / n) for n in names}
    defined = {n: {s[0] for s in m.syms if s[0] and s[2] != 0} for n, m in mods.items()}
    for n in names:
        m = mods[n]
        kinds = Counter(r[1] for r in m.relocs)
        own = sum(1 for r in m.relocs if r[1] != 1027 and m.syms[r[2]][2] != 0)
        imp = sum(1 for r in m.relocs if r[1] != 1027 and m.syms[r[2]][2] == 0)
        to, weak, imports = Counter(), 0, 0
        for s in m.syms[1:]:
            if s[2] != 0 or not s[0]:
                continue
            imports += 1
            where = next((o for o in names if s[0] in defined[o]), None)
            if where:
                to[where] += 1
            elif s[1] >> 4 == STB_WEAK:
                weak += 1
        k = ', '.join(str(kinds[t]) for t in REL_KINDS)
        t = ', '.join(str(to[o]) for o in names)
        print(f'    {{"{n}", {{{k}}}, {len(m.relocs)}, {imp}, {own}, {imports}, {{{t}}}, {weak}, '
              f'{m.dyn.get(DT_INIT, 0):#x}, {m.dyn.get(DT_INIT_ARRAYSZ, 0) // 8}}},')


if __name__ == '__main__':
    main()
