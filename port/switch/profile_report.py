"""The Switch's sampling profile (port/switch/profile.c, DHD_SWITCH_PROFILE) as a report.

    python port/switch/profile_report.py profile.txt build/host64-switch/dhd.elf [--top 40]

profile.txt comes from the SD card, beside dhd.log; dhd.elf must be the one built with the
.nro that wrote it. Addresses are symbolized with devkitA64's addr2line (-f -C -i): run from the
build image, or name another one with DHD_ADDR2LINE. By default it runs addr2line through the
`dhd-switch-dev` Docker image, mounting the elf's folder.

For each thread: its samples, the share in kernel waits (the svc stubs a waiting thread sits
in), then the top functions by self samples (the innermost frame, inlined ones included) and by
the function the sample is in once inlining is undone (the outermost frame at that address).

Then each window (finding 145): a slow field the renderer logged with `[profile window N]`, what
every thread did over that field alone -- its top functions, and for a waiting sample the calls
it waits under. `--windows` prints only those.
"""
import argparse
import collections
import os
import subprocess
import sys
from pathlib import Path

WAITS = ('svcWait', 'svcArbitrate', 'svcSleepThread', 'svcReplyAndReceive', 'svcSendSyncRequest')


def load(path):
    threads, rows, pairs, waits, header = {}, [], [], [], ''
    windows = {}   # id -> (ms, samples, [(thread, pc, lr, up1, up2, count)])
    for line in Path(path).read_text().splitlines():
        if line.startswith('#'):
            header = line
            continue
        parts = line.split()
        if parts[0] == 'thread':
            threads[parts[1]] = (int(parts[2]), int(parts[3]))
        elif parts[0] == 'pair':   # thread, PC, LR, count (the current form)
            rows.append(('self', parts[1], int(parts[2], 16), int(parts[4])))
            rows.append(('caller', parts[1], int(parts[3], 16), int(parts[4])))
            pairs.append((parts[1], int(parts[2], 16), int(parts[3], 16), int(parts[4])))
        elif parts[0] == 'window':   # id, ms, samples
            windows[int(parts[1])] = (float(parts[2]), int(parts[3]), [])
        elif parts[0] == 'ws':   # window, thread, PC, LR, up1, up2, count
            windows[int(parts[1])][2].append((parts[2], *(int(x, 16) for x in parts[3:7]), int(parts[7])))
        elif parts[0] == 'wait':   # thread, count, return addresses (innermost first)
            waits.append((parts[1], int(parts[2]), [int(x, 16) for x in parts[3:]]))
        elif parts[0] in ('self', 'caller'):   # the first form: PC and LR counted apart
            rows.append((parts[0], parts[1], int(parts[2], 16), int(parts[3])))
    return header, threads, rows, pairs, waits, windows


def symbolize(elf, addresses):
    """address -> list of (function, file:line) frames, innermost first."""
    addresses = sorted(addresses)
    tool = os.environ.get('DHD_ADDR2LINE')
    elf = Path(elf).resolve()
    flags = ['-a', '-f', '-C', '-i']   # -a: each address's own line first, then its frames
    if tool:
        cmd = [tool, *flags, '-e', str(elf)]
    else:
        cmd = ['docker', 'run', '--rm', '-i', '-v', f'{elf.parent}:/elf', 'dhd-switch-dev',
               '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line', *flags, '-e', f'/elf/{elf.name}']
    text = '\n'.join(f'0x{a:x}' for a in addresses) + '\n'
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    out = subprocess.run(cmd, input=text, capture_output=True, text=True, env=env, check=True).stdout.splitlines()
    result, current, i = {}, None, 0
    while i < len(out):
        line = out[i]
        if line.startswith('0x'):   # a function name never starts with 0x
            current = int(line, 16)
            result[current] = []
            i += 1
            continue
        where = out[i + 1] if i + 1 < len(out) else '??'
        result[current].append((line, where.split(' (')[0]))
        i += 2
    return {a: f or [('??', '??')] for a, f in result.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profile')
    ap.add_argument('elf')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--callers', type=int, default=20, help='top self functions to list callers for')
    ap.add_argument('--windows', action='store_true', help='only the slow-field windows')
    args = ap.parse_args()
    header, threads, rows, pairs, waits, windows = load(args.profile)
    frames = symbolize(args.elf, {a for _, _, a, _ in rows if a < 0xFFFFFFF} |
                       {a for _, _, stack in waits for a in stack if 0 < a < 0xFFFFFFF} |
                       {a for _, _, entries in windows.values() for e in entries for a in e[1:5] if 0 < a < 0xFFFFFFF})
    print(header)
    if not args.windows:
        report_threads(args, threads, rows, pairs, waits, frames)
    report_windows(windows, frames)


def name_of(frames, a, outer=False):
    f = frames.get(a, [('?? (outside the module)', '??')])
    return f[-1][0] if outer else f[0][0]


def report_windows(windows, frames, top=6):
    """Each slow field's window: per thread, where its samples were."""
    for wid, (ms, samples, entries) in sorted(windows.items()):
        print(f'\n== window {wid}: {ms:.1f} ms, {samples} samples')
        by_thread = collections.defaultdict(list)
        for t, pc, lr, up1, up2, c in entries:
            by_thread[t].append((pc, lr, up1, up2, c))
        for t, items in sorted(by_thread.items(), key=lambda kv: -sum(i[4] for i in kv[1])):
            total = sum(i[4] for i in items) or 1
            work, waiting = collections.Counter(), collections.Counter()
            for pc, lr, up1, up2, c in items:
                name = name_of(frames, pc)
                if any(name.startswith(w) for w in WAITS):
                    chain = ' <- '.join(name_of(frames, a, True) for a in (lr, up1, up2) if a)
                    waiting[f'{name} <- {chain}'] += c
                else:
                    work[f'{name} <- {name_of(frames, lr, True)}'] += c
            print(f'-- {t}: {total} samples, {100 * sum(waiting.values()) / total:.0f}% waiting')
            for label, counter in (('run', work), ('wait', waiting)):
                for what, c in counter.most_common(top):
                    print(f'   {label} {100 * c / total:5.1f}%  {what}')


def report_threads(args, threads, rows, pairs, waits, frames):
    for thread, (samples, failed) in threads.items():
        mine = [(k, a, c) for k, t, a, c in rows if t == thread]
        self_rows = [(a, c) for k, a, c in mine if k == 'self']
        total = sum(c for _, c in self_rows) or 1
        inner, outer, lines, callers = (collections.Counter() for _ in range(4))
        waiting = 0
        for a, c in self_rows:
            f = frames.get(a, [('?? (outside the module)', '??')])
            inner[f[0][0]] += c
            outer[f[-1][0]] += c
            lines[f'{f[0][0]}  {f[0][1]}'] += c
            if any(f[0][0].startswith(w) for w in WAITS):
                waiting += c
        for k, a, c in mine:
            if k == 'caller':
                f = frames.get(a, [('??', '??')])
                callers[f[-1][0]] += c
        print(f'\n== thread {thread}: {samples} samples ({failed} failed), '
              f'{100 * waiting / total:.1f}% in kernel waits')
        for title, counter in (('self (innermost frame)', inner), ('in function (inlining undone)', outer),
                               ('hot lines', lines), ('called from (LR, outermost)', callers)):
            print(f'-- {title}')
            for name, c in counter.most_common(args.top):
                print(f'{100 * c / total:6.2f}%  {c:7d}  {name}')
        mine_waits = collections.Counter()
        for t, c, stack in waits:
            if t == thread:
                names = [frames[a][-1][0] for a in stack if a in frames]
                mine_waits[' <- '.join(names)] += c
        if mine_waits:
            print('-- where it waits (kernel wait or lock, a few calls up)')
            for chain, c in mine_waits.most_common(args.callers):
                print(f'{100 * c / total:6.2f}%  {chain}')
        if pairs:   # who calls each of the top self functions (the LR's function)
            by = collections.defaultdict(collections.Counter)
            for t, pc, lr, c in pairs:
                if t == thread:
                    name = frames.get(pc, [('?? (outside the module)', '??')])[0][0]
                    by[name][frames.get(lr, [('??', '??')])[-1][0]] += c
            print('-- callers of the top self functions')
            for name, c in inner.most_common(args.callers):
                calls = ', '.join(f'{caller} {100 * n / c:.0f}%' for caller, n in by[name].most_common(4))
                print(f'{100 * c / total:6.2f}%  {name}  <-  {calls}')


if __name__ == '__main__':
    sys.exit(main())
