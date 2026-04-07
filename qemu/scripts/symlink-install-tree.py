#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import shutil
import subprocess
import sys


def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))


def copy_fallback(source: str, dest: str) -> None:
    # Some install targets are generated later during the build.
    # Symlinks can point at missing targets, but copying cannot.
    if not os.path.exists(source):
        print(f"skipping missing source for {dest}: {source}", file=sys.stderr)
        return
    if os.path.isdir(source):
        shutil.copytree(source, dest, dirs_exist_ok=True)
    else:
        shutil.copy2(source, dest)


introspect = os.environ.get('MESONINTROSPECT')
out = subprocess.run([*shlex.split(introspect), '--installed'],
                     stdout=subprocess.PIPE, check=True).stdout
for source, dest in json.loads(out).items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except BaseException as e:
        print(f'error making directory {path}', file=sys.stderr)
        raise e
    try:
        os.symlink(source, bundle_dest)
    except BaseException as e:
        if isinstance(e, OSError) and e.errno == errno.EEXIST:
            continue

        can_copy_fallback = (
            isinstance(e, OSError)
            and os.name == 'nt'
            and getattr(e, 'winerror', None) == 1314
        )
        if can_copy_fallback:
            print('Symlink privilege unavailable, copying instead',
                  file=sys.stderr)
            try:
                copy_fallback(source, bundle_dest)
            except BaseException as copy_err:
                print(f'error copying {dest}', file=sys.stderr)
                raise copy_err
            continue

        if os.name == 'nt':
            print('Please enable Developer Mode to support soft link '
                  'without Administrator permission')
        print(f'error making symbolic link {dest}', file=sys.stderr)
        raise e