"""The Switch homebrew release (finding 144, NX4): the .nro and the files that go beside it on
the SD card, checked and zipped into build/release-switch/.

    python port/switch/package.py                       # build/host64-switch/dhd.nro
    python port/switch/package.py --nro PATH

The archive holds the SD card's own layout, so unzipping it onto the card puts everything in
sdmc:/switch/downhill-switch/ :

    switch/downhill-switch/dhd.nro                 the game
    switch/downhill-switch/dhd.ini                 the settings template (port/release/switch-dhd.ini)
    switch/downhill-switch/README.txt              the players' text (port/release/SWITCH-NOTES.txt)
    switch/downhill-switch/LICENSE.txt
    switch/downhill-switch/THIRD-PARTY-NOTICES.txt
    switch/downhill-switch/SWITCH-THIRD-PARTY-NOTICES.txt   what only the .nro links statically:
                                                    libnx, newlib, the GCC runtime, Mesa,
                                                    libdrm_nouveau, glad, libpng, zlib

The player's own disc image goes beside them and is never in the archive.

Why not package_release.py: that one publishes -- it writes latest.json, which the launchers and
the Android update notice read, and signs and checks an APK. The Switch build has no launcher and
no update check (finding 144), so there is nothing for it to publish into; what is worth sharing
is its checks, and those are imported from it rather than written again here.

The checks: the file really is an NRO, it was built from this version, it names no server but the
official one and no path of the machine that built it, and it carries no run of the retail
executable's bytes (that last one needs iso/SCUS_971.77; without it the scan is skipped and said
so, because a release must never be made that way).
"""
import argparse, hashlib, re, shutil, sys, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'port' / 'tools'))
import package_release as rel  # noqa: E402  (its checks, so there is one copy of them)

RELEASE = ROOT / 'port' / 'release'
OUT = ROOT / 'build' / 'release-switch'
FOLDER = 'switch/downhill-switch/'       # where it all lands on the SD card (DHD_SWITCH_DIR)


def version():
    """The game's own version (DHD_VERSION): the .nro must carry it, as every build does."""
    text = (ROOT / 'port' / 'include' / 'version.h').read_text(encoding='utf-8')
    found = re.search(r'#define\s+DHD_VERSION\s+"([^"]+)"', text)
    if not found:
        sys.exit('no DHD_VERSION in port/include/version.h')
    return found.group(1)


def switch_version():
    """The homebrew's own version (port/switch/switch_version.h), which names the release."""
    text = (ROOT / 'port' / 'switch' / 'switch_version.h').read_text(encoding='utf-8')
    found = re.search(r'#define\s+DHD_SWITCH_VERSION\s+"([^"]+)"', text)
    if not found:
        sys.exit('no DHD_SWITCH_VERSION in port/switch/switch_version.h')
    return found.group(1)


def nro_problems(nro, name, settings):
    """Everything wrong with the .nro, as a list of lines (empty: fit to publish). Each line says
    enough to tell a leak from a false positive without running the scan again by hand."""
    import scan_retail
    data = nro.read_bytes()
    problems = []
    # An NRO starts with an eight-byte branch, four reserved bytes, then "NRO0" at 0x10.
    if len(data) < 0x40 or data[0x10:0x14] != b'NRO0':
        problems.append('no NRO0 header: this is not a Switch homebrew file')
    if b'dhd-port %s ' % name.encode() not in data:
        problems.append('not built from version %s (its own version string is not in it)' % name)
    if switch_version().encode() not in data:
        problems.append('not built as Downhill Switch %s (switch_version.h changed since?)' % switch_version())
    # The strings, each with what is around it, so a match can be judged without running the scan
    # again by hand. The game's own paths that hold the project's name (its SD card folder, its
    # repository's address) are blanked first: rel.OWN_PATHS says why that cannot hide a leak.
    lowered = rel.without_own_paths(data)
    for s in rel.FORBIDDEN + sorted(rel.server_strings(settings)):
        at = lowered.find(s.lower())
        if at >= 0:
            around = data[max(0, at - 30):at + len(s) + 30]
            problems.append('names %r, in %r' % (s.decode('ascii', 'replace'),
                                                 around.decode('ascii', 'replace')))
    for host in rel.other_hosts(data):
        problems.append('names the server %r' % host.decode('ascii', 'replace'))
    elf_path = ROOT / 'iso' / 'SCUS_971.77'
    if not elf_path.is_file():
        problems.append('iso/SCUS_971.77 is not here, so the retail-bytes scan could not run; '
                        'a release must not be made without it')
    else:
        elf = elf_path.read_bytes()
        for at, e, n in rel.game_data_runs(data, elf)[:20]:
            problems.append('holds %d bytes of the retail executable at +0x%X, from %s. Either the '
                            'source holds it (then it belongs in scan_retail.SOURCE, with a note '
                            'naming the file, checked by hand) or it must not be shipped'
                            % (n, at, scan_retail.elf_place(elf, e)))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--nro', type=Path, default=ROOT / 'build' / 'host64-switch' / 'dhd.nro',
                    help='the built .nro (default build/host64-switch/dhd.nro)')
    ap.add_argument('--settings', type=Path, help='a dhd.ini whose server address is allowed too')
    ap.add_argument('--skip-checks', action='store_true',
                    help='package anyway: for looking at the layout, never for a release')
    a = ap.parse_args()
    if not a.nro.is_file():
        sys.exit('%s: no such file (build it with port/switch/build.sh)' % a.nro)
    name = version()
    problems = nro_problems(a.nro, name, a.settings)
    if problems and not a.skip_checks:
        sys.exit('%s is not fit to publish:\n  %s' % (a.nro, '\n  '.join(problems[:40])))
    for line in problems:
        print('IGNORED (--skip-checks): ' + line)

    OUT.mkdir(parents=True, exist_ok=True)
    release = switch_version()
    zip_path = OUT / ('DownhillSwitch-%s.zip' % release)
    files = [(FOLDER + 'dhd.nro', a.nro.read_bytes()),
             (FOLDER + 'dhd.ini', rel.lf(RELEASE / 'switch-dhd.ini')),
             (FOLDER + 'README.txt', rel.lf(RELEASE / 'SWITCH-NOTES.txt')),
             (FOLDER + 'LICENSE.txt', rel.lf(ROOT / 'LICENSE')),
             (FOLDER + 'THIRD-PARTY-NOTICES.txt', rel.lf(RELEASE / 'THIRD-PARTY-NOTICES.txt')),
             (FOLDER + 'SWITCH-THIRD-PARTY-NOTICES.txt', rel.lf(RELEASE / 'SWITCH-THIRD-PARTY-NOTICES.txt'))]
    for path, _ in files:
        if rel.GAME_DATA.search(path.rsplit('/', 1)[-1]):
            sys.exit('%s looks like game data' % path)
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path, content in files:
            z.writestr(path, content)

    # The folder beside the archive, so the layout can be looked at without unzipping.
    folder = OUT / ('DownhillSwitch-%s' % release)
    if folder.exists():
        shutil.rmtree(folder)
    for path, content in files:
        at = folder / path
        at.parent.mkdir(parents=True, exist_ok=True)
        at.write_bytes(content)

    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (OUT / 'SHA256SUMS').write_text('%s  %s\n' % (digest, zip_path.name), encoding='utf-8')
    print('%s (%d KB) sha256 %s' % (zip_path, zip_path.stat().st_size // 1024, digest))
    print('  unzipped beside it: %s' % folder)
    print('  the player puts their own disc image in %sgame.iso' % FOLDER)
    return 0


if __name__ == '__main__':
    sys.exit(main())
