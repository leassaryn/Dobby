#!/usr/bin/env python3
"""Build and package Dobby using canonical Android ABI names, from any directory."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
ALIASES = {"arm64": "arm64-v8a", "aarch64": "arm64-v8a", "armv7": "armeabi-v7a", "arm": "armeabi-v7a"}


def parse_abis(values):
    result = []
    for value in values or ["all"]:
        for name in value.split(","):
            names = ABIS if name == "all" else (ALIASES.get(name, name),)
            for abi in names:
                if abi not in ABIS:
                    raise ValueError(f"unknown ABI: {abi}")
                if abi not in result:
                    result.append(abi)
    return result


def run(command, dry_run=False):
    print("+ " + shlex.join(map(str, command)), flush=True)
    if not dry_run:
        subprocess.run(list(map(str, command)), check=True, cwd=ROOT)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi", action="append", help="ABI, comma-separated list, or all (default); aliases accepted")
    parser.add_argument("--ndk", type=Path, default=os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT"))
    parser.add_argument("--cmake", default=os.environ.get("CMAKE", "cmake"))
    parser.add_argument("--configuration", choices=("Debug", "Release", "RelWithDebInfo", "MinSizeRel"), default="Release")
    parser.add_argument("--library", choices=("static", "shared"), default="static")
    parser.add_argument("--api", type=int, default=21)
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--build-root", type=Path, default=ROOT / "build/android")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build-output")
    parser.add_argument("--tests", action="store_true", help="build device tests as well as the library")
    parser.add_argument("--prefab", action="store_true", help="generate a Prefab package in the variant output")
    parser.add_argument("--archive", type=Path, default=os.environ.get("DOBBY_BUILD_OUTPUT_NAME"), help="optional .tar.gz of selected ABI outputs")
    parser.add_argument("--clean-first", action="store_true", help="clean CMake targets before building")
    parser.add_argument("--dry-run", action="store_true", help="print commands without creating or changing files")
    args = parser.parse_args(argv)
    try:
        abis = parse_abis(args.abi)
    except ValueError as error:
        parser.error(str(error))
    if args.jobs < 1 or args.api < 21:
        parser.error("jobs must be positive and Android API must be >= 21")
    if not args.ndk:
        parser.error("set --ndk or ANDROID_NDK_HOME")
    ndk = args.ndk.expanduser().resolve()
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        parser.error(f"NDK toolchain missing: {toolchain}")
    variant = Path(args.configuration.lower()) / args.library
    build_root = args.build_root.expanduser().resolve() / variant
    output = args.output_root.expanduser().resolve() / variant
    library = "libdobby.a" if args.library == "static" else "libdobby.so"
    check_elf = runpy.run_path(str(ROOT / "scripts/check-elf-alignment.py"))["check"]
    packaged = []
    for abi in abis:
        build = build_root / abi
        destination = output / "android" / abi
        command = [args.cmake, "-S", ROOT, "-B", build,
                   f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DANDROID_ABI={abi}",
                   f"-DANDROID_PLATFORM=android-{args.api}", "-DANDROID_STL=c++_static",
                   f"-DCMAKE_BUILD_TYPE={args.configuration}",
                   f"-DDOBBY_GENERATE_SHARED={'ON' if args.library == 'shared' else 'OFF'}",
                   f"-DDOBBY_DEBUG={'ON' if args.configuration == 'Debug' else 'OFF'}",
                   f"-DDOBBY_BUILD_TESTS={'ON' if args.tests else 'OFF'}",
                   "-DDynamicBinaryInstrument=ON", "-DNearBranch=ON", "-DFullFloatingPointRegisterPack=ON",
                   "-DPlugin.SymbolResolver=ON", "-DPlugin.LinkerLoadCallback=ON",
                   "-DPlugin.Android.BionicLinkerRestriction=ON"]
        run(command, args.dry_run)
        command = [args.cmake, "--build", build, "--parallel", args.jobs]
        if args.clean_first:
            command.append("--clean-first")
        if not args.tests:
            command += ["--target", "dobby"]
        run(command, args.dry_run)
        if args.dry_run:
            print(f"Package: {destination / library}")
            continue
        if abi in ("arm64-v8a", "x86_64"):
            binaries = list(build.glob("*.so"))
            if args.tests:
                binaries += list(build.glob("dobby_*_test"))
            for binary in binaries:
                check_elf(binary.read_bytes())
                print(f"16 KB ELF alignment: {binary.name}", flush=True)
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / library, destination / library)
        shutil.copy2(ROOT / "include/dobby.h", destination / "dobby.h")
        metadata = {"abi": abi, "api": args.api, "configuration": args.configuration,
                    "library": args.library, "ndk": str(ndk), "tests_built": args.tests}
        (destination / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
        packaged += [destination / library, destination / "dobby.h", destination / "build.json"]
        if args.tests:
            for binary in sorted(build.glob("dobby_*_test")) + list(build.glob("libdobby_test_*.so")):
                shutil.copy2(binary, destination / binary.name)
                packaged.append(destination / binary.name)
        if args.prefab:
            prefab = output / "prefab"
            module = prefab / "modules/dobby"
            libs = module / "libs" / f"android.{abi}"
            libs.mkdir(parents=True, exist_ok=True)
            (module / "include").mkdir(exist_ok=True)
            shutil.copy2(ROOT / "include/dobby.h", module / "include/dobby.h")
            shutil.copy2(build / library, libs / library)
            properties = (ndk / "source.properties").read_text()
            revision = next(line.split("=", 1)[1].strip() for line in properties.splitlines() if line.startswith("Pkg.Revision"))
            (libs / "abi.json").write_text(json.dumps({"abi": abi, "api": args.api,
                "ndk": int(revision.split(".")[0]), "stl": "c++_static", "static": args.library == "static"}, indent=2) + "\n")
            (prefab / "prefab.json").write_text(json.dumps({"name": "dobby", "schema_version": 2, "dependencies": []}) + "\n")
            (module / "module.json").write_text(json.dumps({"export_libraries": ["-llog"], "android": {}}) + "\n")
            packaged += [libs / library, libs / "abi.json", module / "include/dobby.h", module / "module.json", prefab / "prefab.json"]
    if args.archive and not args.dry_run:
        archive = args.archive.expanduser().resolve()
        archive.parent.mkdir(parents=True, exist_ok=True)
        # Publish only a complete archive, preserving a previous archive on failure.
        if archive in packaged:
            raise ValueError("archive must not overwrite a packaged artifact")
        with tempfile.NamedTemporaryFile(dir=archive.parent, prefix=archive.name + ".", delete=False) as staging:
            temporary = Path(staging.name)
        try:
            with tarfile.open(temporary, "w:gz") as bundle:
                for path in dict.fromkeys(packaged):
                    bundle.add(path, arcname=str(variant / path.relative_to(output)))
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
        print(f"Archive: {archive}")
    print(f"Build root: {build_root}\nOutput: {output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        sys.exit(1)
