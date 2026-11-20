#!/usr/bin/env python3
"""Copy the Switch build and your game files to the console over FTP.

  scripts/deploy-switch-ftp.py --host <switch ip> --user <name> [--env env.txt] [--game-dir ../SSX3]

Needs an FTP server running on the Switch (ftpd, port 5000 by default).
Credentials come from SSX3_FTP_PASSWORD or a hidden prompt; nothing is
stored. Layout: /switch/ssx3/ssx3_recomp.nro, env.txt, game/. The NRO and
env.txt are replaced when they differ (with SHA-256 readback); game files
are never overwritten when they differ.
From the SSX Tricky Switch port's deploy script. Completed uploads are size-checked;
runtime and default.xbe also receive SHA256 readback checks. A local manifest
allows a interrupted run to reuse its verified completed files. Partial .ssx-
upload files can resume only if the server accepts REST; otherwise restart
that temporary file. No console settings or existing saves are modified.
"""
import argparse
from ftplib import FTP, error_perm
import getpass
import hashlib
import json
import os
from pathlib import Path
import time

REMOTE = '/switch/ssx3'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def remote_size(ftp, name):
    try:
        return ftp.size(name)
    except error_perm as exc:
        if not str(exc).startswith('550'):
            raise
        return None


def upload(ftp, source, target, progress):
    """Use an active socket and send data before reading the server's 150.

    Some Switch FTP servers otherwise wait indefinitely for data. Only the
    temporary name is written; a failed transfer cannot truncate the final.
    """
    temporary = target + '.ssx-upload'
    size = source.stat().st_size
    offset = remote_size(ftp, temporary) or 0
    if offset > size:
        raise RuntimeError('Temporary file is larger than source: ' + temporary)
    if offset:
        # A partial temporary upload is accepted only if its bytes match this
        # source's prefix. Never splice an unrelated file into a game asset.
        actual = hashlib.sha256()
        ftp.retrbinary('RETR ' + temporary, actual.update)
        expected = hashlib.sha256()
        with source.open('rb') as stream:
            remaining = offset
            while remaining:
                data = stream.read(min(1024 * 1024, remaining))
                if not data:
                    raise RuntimeError('Source changed while checking partial upload')
                expected.update(data)
                remaining -= len(data)
        if actual.digest() != expected.digest():
            raise RuntimeError('Partial upload content differs: ' + temporary)
    if offset < size:
        ftp.voidcmd('TYPE I')
        if offset:
            try:
                response = ftp.sendcmd('REST ' + str(offset))
                if not response.startswith('350'):
                    offset = 0
            except error_perm:
                offset = 0
        print(f'Uploading {source.name} from {offset:,} of {size:,} bytes', flush=True)
        with ftp.makeport() as listener:
            ftp.putcmd('STOR ' + temporary)
            connection, _ = listener.accept()
            with connection, source.open('rb') as stream:
                stream.seek(offset)
                while data := stream.read(256 * 1024):
                    connection.sendall(data)
                    offset += len(data)
                    progress(offset, size, source.name)
        response = ftp.getresp()
        if response.startswith('1'):
            response = ftp.getresp()
        if not response.startswith('2'):
            raise RuntimeError('Upload failed: ' + response)
    if remote_size(ftp, temporary) != size:
        raise RuntimeError('Upload size mismatch: ' + target)
    return temporary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', required=True)
    p.add_argument('--port', type=int, default=5000)
    p.add_argument('--user', required=True)
    p.add_argument('--runtime', type=Path, default=Path('build-switch/ssx3_recomp.nro'))
    p.add_argument('--game-dir', type=Path, default=Path('../SSX3'))
    p.add_argument('--env', type=Path, help='a RECOMP_* settings file to install as env.txt')
    p.add_argument('--no-game', action='store_true', help='only the NRO (and env.txt)')
    p.add_argument('--manifest', type=Path, default=Path('build-switch/game-transfer.json'))
    p.add_argument('--dry-run', action='store_true')
    a = p.parse_args()
    if not a.runtime.is_file() or (not a.no_game and not (a.game_dir / 'default.xbe').is_file()):
        p.error('Build the NRO (scripts/build-switch.sh) and give the rip (--game-dir) with default.xbe')
    files = [(a.runtime, REMOTE + '/ssx3_recomp.nro')]
    replaceable = {REMOTE + '/ssx3_recomp.nro', REMOTE + '/env.txt'}
    if a.env:
        files.append((a.env, REMOTE + '/env.txt'))
    for source in ([] if a.no_game else sorted(a.game_dir.rglob('*'))):
        if not source.is_file():
            continue
        relative = source.relative_to(a.game_dir).as_posix()
        if source.is_symlink() or any(c in relative for c in '\r\n'):
            p.error('Unsafe input path: ' + relative)
        files.append((source, REMOTE + '/game/' + relative))
    print(f'Stage {len(files)} files, {sum(s.stat().st_size for s, _ in files):,} bytes', flush=True)
    if a.dry_run:
        return
    password = os.environ.get('SSX3_FTP_PASSWORD') or getpass.getpass('FTP password: ')
    old = json.loads(a.manifest.read_text()) if a.manifest.exists() else {}
    verified = {r['path']: r for r in old.get('files', [])}
    rows = []
    ftp = FTP()
    ftp.connect(a.host, a.port, timeout=30)
    ftp.login(a.user, password)
    ftp.set_pasv(False)
    ftp.voidcmd('TYPE I')
    created = set()
    last = time.monotonic()

    def progress(done, total, name):
        nonlocal last
        now = time.monotonic()
        if now - last >= 15:
            print(f'{name}: {done / 1e6:.0f}/{total / 1e6:.0f} MB', flush=True)
            last = now

    try:
        for source, target in files:
            directory = ''
            for part in target.rsplit('/', 1)[0].strip('/').split('/'):
                directory += '/' + part
                if directory not in created:
                    try:
                        ftp.mkd(directory)
                    except error_perm:
                        ftp.cwd(directory)
                    created.add(directory)
            size = source.stat().st_size
            sha = digest(source)
            existing = remote_size(ftp, target)
            old_row = verified.get(target)
            if existing is not None and target in replaceable:
                h = hashlib.sha256()
                if existing == size:
                    ftp.retrbinary('RETR ' + target, h.update)
                if existing == size and h.hexdigest() == sha:
                    print('Up to date:', source.name, flush=True)
                else:
                    temporary = upload(ftp, source, target, progress)
                    h = hashlib.sha256()
                    ftp.retrbinary('RETR ' + temporary, h.update)
                    if h.hexdigest() != sha:
                        raise RuntimeError('Hash mismatch: ' + target)
                    ftp.delete(target)
                    ftp.rename(temporary, target)
                    print('Replaced:', source.name, flush=True)
            elif existing is not None:
                if existing != size:
                    raise RuntimeError('Refusing to overwrite differing destination: ' + target)
                # Accept the manifest only for the identical source, size and
                # target. Other existing data must pass a full hash readback.
                if not (old_row and old_row['bytes'] == size and old_row['source_sha256'] == sha):
                    h = hashlib.sha256()
                    ftp.retrbinary('RETR ' + target, h.update)
                    if h.hexdigest() != sha:
                        raise RuntimeError('Existing content differs: ' + target)
                print('Verified existing:', source.name, flush=True)
            else:
                temporary = upload(ftp, source, target, progress)
                if source.suffix == '.nro' or source.name in ('default.xbe', 'env.txt'):
                    h = hashlib.sha256()
                    ftp.retrbinary('RETR ' + temporary, h.update)
                    if h.hexdigest() != sha:
                        raise RuntimeError('Hash mismatch: ' + target)
                ftp.rename(temporary, target)
                if remote_size(ftp, target) != size:
                    raise RuntimeError('Final size mismatch: ' + target)
                print('Installed:', source.name, flush=True)
            rows.append({'path': target, 'bytes': size, 'source_sha256': sha,
                         'remote_check': 'sha256' if source.suffix == '.nro' or source.name == 'default.xbe' else 'size'})
            # Keep the old verified entries during a retry, not just the ones
            # already revisited before the next network interruption.
            verified[target] = rows[-1]
            a.manifest.parent.mkdir(parents=True, exist_ok=True)
            a.manifest.write_text(json.dumps({'completed': len(verified), 'total_files': len(files),
                                             'files': list(verified.values())}, indent=2) + '\n')
        print('Staging complete; runtime/XBE hashes and other file sizes verified.', flush=True)
    finally:
        ftp.close()


if __name__ == '__main__':
    main()
