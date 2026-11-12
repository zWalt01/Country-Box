#!/usr/bin/env python3
# Country-Box (Linux port): mechanical Win32 -> platform.h rewrites in runtime C files. Only idioms whose
# meaning is fixed: SRW locks (shared mode becomes exclusive), condition variables, Interlocked*, _aligned_malloc,
# _strdup/_stricmp, Sleep, GetTickCount64, GetCurrentThreadId, YieldProcessor, LONG/LONG64 counters. Everything else
# (threads, files, memory, handles) is left for a hand port. Prints what it changed and what Win32 names remain.
import re
import sys


def split_call(s, i):
    """s[i] is '('. Returns (args, end) with end the index after ')'; args split at top-level commas."""
    depth, j, start, args = 0, i, i + 1, []
    while j < len(s):
        c = s[j]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append(s[start:j].strip())
                return args, j + 1
        elif c == ',' and depth == 1:
            args.append(s[start:j].strip())
            start = j + 1
        elif c == '"':
            j += 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
        j += 1
    raise ValueError('unbalanced call')


def rewrite_calls(s, name, fn):
    out, i, n = [], 0, 0
    pat = re.compile(r'\b' + name + r'\s*\(')
    while True:
        m = pat.search(s, i)
        if not m:
            out.append(s[i:])
            break
        args, end = split_call(s, m.end() - 1)
        out.append(s[i:m.start()])
        out.append(fn(args))
        i = end
        n += 1
    return ''.join(out), n


def amp(a):   # "&x" -> "x" for macros that take the object; else "*(a)"
    return a[1:].strip() if a.startswith('&') else '*(' + a + ')'


CALLS = {
    'AcquireSRWLockExclusive': lambda a: f'plat_mutex_lock({a[0]})',
    'ReleaseSRWLockExclusive': lambda a: f'plat_mutex_unlock({a[0]})',
    'AcquireSRWLockShared': lambda a: f'plat_mutex_lock({a[0]})',
    'ReleaseSRWLockShared': lambda a: f'plat_mutex_unlock({a[0]})',
    'TryAcquireSRWLockExclusive': lambda a: f'plat_mutex_trylock({a[0]})',
    'InitializeSRWLock': lambda a: f'plat_mutex_init({a[0]})',
    'InitializeConditionVariable': lambda a: f'plat_cond_init({a[0]})',
    'WakeConditionVariable': lambda a: f'plat_cond_signal({a[0]})',
    'WakeAllConditionVariable': lambda a: f'plat_cond_broadcast({a[0]})',
    'SleepConditionVariableSRW': lambda a: (f'plat_cond_wait({a[0]}, {a[1]})' if a[2] == 'INFINITE'
                                            else f'plat_cond_wait_ns({a[0]}, {a[1]}, (uint64_t)({a[2]}) * 1000000ull)'),
    'InterlockedIncrement': lambda a: f'plat_atomic_inc({a[0]})',
    'InterlockedIncrement64': lambda a: f'plat_atomic_inc({a[0]})',
    'InterlockedDecrement': lambda a: f'plat_atomic_dec({a[0]})',
    'InterlockedDecrement64': lambda a: f'plat_atomic_dec({a[0]})',
    'InterlockedExchange': lambda a: f'plat_atomic_xchg({a[0]}, {a[1]})',
    'InterlockedExchange64': lambda a: f'plat_atomic_xchg({a[0]}, {a[1]})',
    'InterlockedExchangeAdd': lambda a: f'plat_atomic_add({a[0]}, {a[1]})',
    'InterlockedExchangeAdd64': lambda a: f'plat_atomic_add({a[0]}, {a[1]})',
    'InterlockedAdd64': lambda a: f'(plat_atomic_add({a[0]}, {a[1]}) + ({a[1]}))',
    'InterlockedCompareExchange': lambda a: f'plat_atomic_cas({a[0]}, {a[1]}, {a[2]})',
    'InterlockedCompareExchange64': lambda a: f'plat_atomic_cas({a[0]}, {a[1]}, {a[2]})',
    '_aligned_malloc': lambda a: f'aligned_alloc({a[1]}, (({a[0]}) + ({a[1]}) - 1) & ~(size_t)(({a[1]}) - 1))',
    '_aligned_free': lambda a: f'free({a[0]})',
    '_strdup': lambda a: f'strdup({a[0]})',
    '_stricmp': lambda a: f'strcasecmp({a[0]}, {a[1]})',
    '_strnicmp': lambda a: f'strncasecmp({a[0]}, {a[1]}, {a[2]})',
    'Sleep': lambda a: f'plat_sleep_ns((uint64_t)({a[0]}) * 1000000ull)',
    'GetTickCount64': lambda a: '(plat_now_ns() / 1000000ull)',
    'GetCurrentThreadId': lambda a: 'plat_thread_id()',
    'YieldProcessor': lambda a: '__builtin_ia32_pause()',
    'SwitchToThread': lambda a: 'sched_yield()',
}

TOKENS = [
    (r'\bstatic\s+SRWLOCK\s+(\w+)\s*=\s*SRWLOCK_INIT\b', r'static PlatMutex \1 = PLAT_MUTEX_INIT'),
    (r'\bSRWLOCK_INIT\b', 'PLAT_MUTEX_INIT'),
    (r'\bCONDITION_VARIABLE_INIT\b', 'PLAT_COND_INIT'),
    (r'\bSRWLOCK\b', 'PlatMutex'),
    (r'\bCONDITION_VARIABLE\b', 'PlatCond'),
    (r'\bvolatile\s+LONG64\b', 'volatile int64_t'),
    (r'\bvolatile\s+LONG\b', 'volatile int32_t'),
    (r'\bLONG64\b', 'int64_t'),
    (r'\bINIT_ONCE_STATIC_INIT\b', 'PLAT_ONCE_INIT'),
]

WIN32 = re.compile(r'\b(HANDLE|DWORD|BOOL|WINAPI|CreateThread|CreateFile\w*|ReadFile|WriteFile|CloseHandle|Virtual\w+|'
                   r'GetLastError|MultiByteToWideChar|WideCharToMultiByte|wchar_t|MAX_PATH|INVALID_HANDLE_VALUE|'
                   r'WaitForSingleObject|LARGE_INTEGER|QueryPerformance\w+|InitOnceExecuteOnce|INIT_ONCE|LONG|'
                   r'SetEnvironmentVariable\w*|GetModuleHandle\w*|Find\w+File\w*|_wfopen|LPVOID|LPCWSTR|HMODULE)\b')

for path in sys.argv[1:]:
    s = open(path).read()
    if '#include <windows.h>' in s:
        s = s.replace('#include <windows.h>\n', '')
    changes = []
    for name, fn in CALLS.items():
        s, n = rewrite_calls(s, name, fn)
        if n:
            changes.append(f'{name} x{n}')
    for pat, rep in TOKENS:
        s, n = re.subn(pat, rep, s)
        if n:
            changes.append(f'{pat.split(chr(92))[1] if chr(92) in pat else pat} x{n}')
    if 'plat_' in s and '"platform/platform.h"' not in s and '"platform.h"' not in s:
        s = re.sub(r'(#include "[^"]+"\n)', r'\1#include "platform/platform.h"\n', s, count=1)
    if ('strcasecmp' in s or 'strncasecmp' in s) and '<strings.h>' not in s:
        s = s.replace('#include <string.h>\n', '#include <string.h>\n#include <strings.h>\n', 1)
    if 'sched_yield' in s and '<sched.h>' not in s:
        s = s.replace('#include <stdio.h>\n', '#include <sched.h>\n#include <stdio.h>\n', 1)
    open(path, 'w').write(s)
    left = sorted(set(m.group(1) for m in WIN32.finditer(s)))
    print(f'{path}: {", ".join(changes) or "nothing"}' + (f'\n    left for a hand port: {" ".join(left)}' if left else ''))
