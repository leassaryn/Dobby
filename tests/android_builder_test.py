"""Exercise builder routing, packaging, dry runs and failure handling without an NDK build."""
import contextlib
import io
from pathlib import Path
import runpy
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

builder = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'scripts/build-android.py'))
main = builder['main']


class BuilderTest(unittest.TestCase):
    def test_aliases_and_all(self):
        self.assertEqual(builder['parse_abis'](['arm64,armv7', 'arm64']), ['arm64-v8a', 'armeabi-v7a'])
        self.assertEqual(builder['parse_abis'](['all']), list(builder['ABIS']))
        with self.assertRaises(ValueError):
            builder['parse_abis'](['mips'])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dobby builder ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        ndk = self.root / 'ndk'
        (ndk / 'build/cmake').mkdir(parents=True)
        (ndk / 'build/cmake/android.toolchain.cmake').touch()
        (ndk / 'source.properties').write_text('Pkg.Revision = 29.0.1\n')
        self.args = ['--ndk', str(ndk), '--build-root', str(self.root / 'build'),
                     '--output-root', str(self.root / 'output')]

    def invoke(self, extra):
        with contextlib.redirect_stdout(io.StringIO()):
            return main(self.args + extra)

    def fake_run(self, command, dry_run=False):
        if '--build' in command and not dry_run:
            directory = Path(command[command.index('--build') + 1])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / 'libdobby.a').write_bytes(b'archive fixture')

    def test_dry_run_no_writes(self):
        self.invoke(['--abi', 'all', '--dry-run', '--prefab'])
        self.assertFalse((self.root / 'build').exists())
        self.assertFalse((self.root / 'output').exists())

    def test_canonical_package_and_archive(self):
        archive = self.root / 'bundle.tar.gz'
        with patch.dict(main.__globals__, run=self.fake_run):
            self.invoke(['--abi', 'arm64,armv7', '--prefab', '--archive', str(archive)])
        output = self.root / 'output/release/static'
        for abi in ('arm64-v8a', 'armeabi-v7a'):
            self.assertTrue((output / 'android' / abi / 'libdobby.a').is_file())
            self.assertTrue((output / 'prefab/modules/dobby/libs' / ('android.' + abi) / 'abi.json').is_file())
        self.assertFalse((output / 'android/arm64').exists())
        with tarfile.open(archive) as bundle:
            self.assertIn('release/static/android/arm64-v8a/libdobby.a', bundle.getnames())
        with patch.dict(main.__globals__, run=self.fake_run):
            self.invoke(['--abi', 'armv7', '--archive', str(archive)])
        with tarfile.open(archive) as bundle:
            self.assertFalse(any('arm64-v8a' in name for name in bundle.getnames()))

    def test_archive_failure_preserves_previous(self):
        archive = self.root / 'bundle.tar.gz'
        archive.write_bytes(b'previous archive')
        with patch.dict(main.__globals__, run=self.fake_run), \
                patch('tarfile.open', side_effect=OSError('write failed')):
            with self.assertRaises(OSError):
                self.invoke(['--abi', 'armv7', '--archive', str(archive)])
        self.assertEqual(archive.read_bytes(), b'previous archive')
        self.assertEqual(list(self.root.glob('bundle.tar.gz.*')), [])

    def test_failure_does_not_package(self):
        def fail(*args):
            raise subprocess.CalledProcessError(1, ['cmake'])
        with patch.dict(main.__globals__, run=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                self.invoke(['--abi', 'arm64'])
        self.assertFalse((self.root / 'output').exists())


class CleanerTest(unittest.TestCase):
    def test_preserves_tracked_and_symlink_target(self):
        cleaner = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'scripts/clean-build.py'))['main']
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'build').mkdir()
            (root / 'outside').mkdir()
            (root / 'outside/keep').write_text('external')
            (root / 'build/tracked').write_text('tracked')
            (root / 'build/generated').touch()
            (root / 'build/link').symlink_to(root / 'outside', target_is_directory=True)
            with patch.dict(cleaner.__globals__, ROOT=root), patch('sys.argv', ['clean', '--dry-run']), \
                    patch('subprocess.check_output', return_value=b'build/tracked\0'), \
                    contextlib.redirect_stdout(io.StringIO()):
                cleaner()
                self.assertTrue((root / 'build/generated').exists())
                with patch('sys.argv', ['clean']):
                    cleaner()
            self.assertTrue((root / 'build/tracked').exists())
            self.assertTrue((root / 'outside/keep').exists())
            self.assertFalse((root / 'build/generated').exists())
            self.assertFalse((root / 'build/link').is_symlink())


if __name__ == '__main__':
    unittest.main()
