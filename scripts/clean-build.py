#!/usr/bin/env python3
"""Remove generated build files while preserving every Git-tracked file."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TREES = ('build', 'build-output', 'auto-build-output', 'auto-build-workspace', 'prefab',
         'tests/build', 'build-workspace/auto-build-workspace')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dry-run', '-n', action='store_true')
    args = parser.parse_args()
    tracked = set(subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0'))

    def clean(path, libraries_only=False):
        relative = path.relative_to(ROOT).as_posix()
        if relative in tracked:
            return
        if path.is_symlink() or path.is_file():
            if libraries_only and path.suffix not in ('.a', '.so'):
                return
            print(('Would remove: ' if args.dry_run else 'Removing: ') + relative)
            if not args.dry_run:
                path.unlink()
        elif path.is_dir():
            for child in path.iterdir():
                clean(child, libraries_only)
            if not args.dry_run and not any(path.iterdir()):
                path.rmdir()

    for tree in TREES:
        clean(ROOT / tree)
    clean(ROOT / 'build-workspace/prefab', libraries_only=True)


if __name__ == '__main__':
    main()
