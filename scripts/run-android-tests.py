#!/usr/bin/env python3
"""Run built Android tests locally or over adb and save observed runtime evidence."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
TESTS = ("dobby_api_compatibility_test", "dobby_public_api_c_test", "dobby_hook_safety_test", "dobby_linker_callback_test", "dobby_modern_api_test", "dobby_plugin_api_test")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True, help="directory containing one build directory per ABI")
    parser.add_argument("--output", type=Path, default=Path("build/android-test-results.json"))
    parser.add_argument("--local", action="store_true", help="execute on the current Android host")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial")
    parser.add_argument("--abi", action="append", choices=ABIS)
    parser.add_argument("--require-16k", action="store_true")
    parser.add_argument("--require-bti", action="store_true")
    parser.add_argument("--require-pac", action="store_true")
    args = parser.parse_args()
    adb = [args.adb] + (["-s", args.serial] if args.serial else [])

    def run(command, **kwargs):
        return subprocess.run(command, text=True, capture_output=True, timeout=90, **kwargs)

    def device(command):
        return run(command if args.local else adb + ["shell", shlex.join(command)])

    supported = device(["getprop", "ro.product.cpu.abilist"])
    if supported.returncode:
        parser.error("cannot query Android device: " + supported.stderr)
    abis = set(supported.stdout.strip().split(","))
    api = device(["getprop", "ro.build.version.sdk"]).stdout.strip()
    report = {"api_level": api, "device_abis": sorted(abis), "local": args.local, "results": []}
    requested = args.abi or list(ABIS)
    required = [flag for flag in ("--require-16k", "--require-bti", "--require-pac")
                if getattr(args, flag[2:].replace("-", "_"))]
    failed = False
    executed = 0
    for abi in requested:
        if abi not in abis:
            report["results"].append({"abi": abi, "status": "skipped", "reason": "device does not support ABI"})
            if args.abi:
                failed = True
            continue
        directory = args.build_root.resolve() / abi
        required_binary = directory / TESTS[0]
        if not required_binary.is_file():
            report["results"].append({"abi": abi, "status": "failed", "reason": "compatibility executable missing"})
            failed = True
            continue
        remote = f"/data/local/tmp/dobby-tests-{os.getpid()}-{time.time_ns()}-{abi}"
        if not args.local:
            result = device(["mkdir", "-p", remote])
            if result.returncode:
                raise RuntimeError(result.stderr)
            for library in ("libdobby.so", "libdobby_test_importer.so", "libdobby_test_packed.so"):
                shared = directory / library
                if shared.is_file():
                    run(adb + ["push", str(shared), remote + "/"], check=True)
        for name in TESTS:
            binary = directory / name
            if not binary.is_file():
                report["results"].append({"abi": abi, "test": name, "status": "skipped", "reason": "target not built"})
                continue
            flags = required if name == TESTS[0] else []
            try:
                if args.local:
                    env = dict(os.environ)
                    env["LD_LIBRARY_PATH"] = str(directory) + ":" + env.get("LD_LIBRARY_PATH", "")
                    # Invoke Android's matching linker explicitly. Termux exec
                    # wrappers can drop library paths for a foreign (32-bit) ABI.
                    env.pop("LD_PRELOAD", None)
                    linker = "/system/bin/linker64" if abi in ("arm64-v8a", "x86_64") else "/system/bin/linker"
                    result = run([linker, str(binary), *flags], env=env)
                else:
                    run(adb + ["push", str(binary), remote + "/"], check=True)
                    device(["chmod", "700", remote + "/" + name])
                    result = device(["env", "LD_LIBRARY_PATH=" + remote, remote + "/" + name, *flags])
                entry = {"abi": abi, "test": name, "status": "passed" if result.returncode == 0 else "failed",
                         "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
                for line in result.stdout.splitlines():
                    if line.startswith("DOBBY_RESULT "):
                        entry["runtime"] = json.loads(line[len("DOBBY_RESULT "):])
                failed |= result.returncode != 0
                executed += 1
            except (subprocess.SubprocessError, OSError) as error:
                failed = True
                entry = {"abi": abi, "test": name, "status": "failed", "reason": str(error)}
            report["results"].append(entry)
            print(f"{abi}/{name}: {entry['status']}", flush=True)
        # Keep device artifacts for diagnosis; report their exact directory.
        if not args.local:
            report.setdefault("device_artifacts", []).append(remote)
    report["executed"] = executed
    report["success"] = not failed and executed > 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
