# Compatibility evidence

## Plugin update — 2026-10-24

NDK 29.0.14206865 / CMake 4.1.2, Android 14 API 34 with 4096-byte pages.

| Check | Observed result |
| --- | --- |
| Release static/shared, four ABIs | Eight builds passed without warnings; 64-bit ELF alignment checks passed |
| ARM64 and ARMv7 device tests | 22 executions passed: static/shared compatibility, C API, callbacks, modern API and plugin API, plus ARM64 safety |
| ARM64 Debug ASan/UBSan | All six CTests passed; function-type checking excluded for generated code and vptr excluded as described below |
| Symbol/loader/Bionic plugins disabled | ARM64 plugin and C API tests passed using manual deferred polling |
| Lifecycle | Early constructor calls, refused-patch retry, foreign ownership, cancellation and a destructor joining another Dobby-using thread passed |
| Parsers | Grouped REL/RELA, signed deltas, truncation/overflow/malformed ELF cases and 10,000 deterministic malformed APS2 streams passed |
| Bionic private ABI probe | Both ARM64 and ARMv7 report UNSUPPORTED_FORMAT on this device; disable/shutdown succeeds. Actual bypass availability is not claimed |
| Benchmark | ARM64: 200,000 iterations, inline-original 5.555 ns, instrumentation 63.490 ns, setup/commit/query/destroy 83.303 us; environment-specific, not a performance guarantee |

Evidence files: `build/plugin-static-runtime.json`, `build/plugin-shared-runtime.json`,
`build/plugin-sanitized-tests.log`, `build/plugin-benchmark.json` and the matching
build logs. These generated artifacts can be removed by cleanup.

The NDK sanitizer's vptr check also rejects a standalone program consisting only
of a virtual base constructor and derived object, without Dobby. This environment
therefore excludes vptr while retaining ASan and the remaining UBSan checks.
LeakSanitizer is unavailable. No x86/x64 Android runtime, true 16 KB kernel or
hardware BTI/PAC validation was performed. Hosted CI has not been executed here.
See [plugin contracts and limits](plugins.md).

## Earlier engine verification

Local verification on 2026-10-23, using Android NDK 29.0.14206865 and CMake 4.1.2.
Unmarked rows describe the engine changes. Rows marked baseline were checked at
commit 9161527 and were not rerun for this update. These results do not imply
hosted CI or an unavailable device matrix has run.

| Check | Result |
| --- | --- |
| Release static and shared builds, all four Android ABIs | Passed, no compiler/linker warnings |
| ARM64 Android 14 / API 34, 4096-byte pages | Compatibility, C API, safety, linker-callback and modern-API tests passed in static and shared configurations |
| ARM32 Android 14 / API 34, 4096-byte pages | Compatibility (ARM and Thumb), C API, linker-callback and modern-API tests passed in static and shared configurations |
| ARM64 core smoke linked to the Android archive (baseline 9161527) | 23 passed, 0 failed |
| ARM64 ASan + UBSan | Compatibility, C API, safety, linker-callback and modern-API tests passed; function-type checking excluded for generated code |
| ARM64 Debug (baseline 9161527) | Build and compatibility runtime passed |
| ARM64 minimal Release, DBI/near/symbol/linker plugins disabled (baseline 9161527) | Build and compatibility runtime passed |
| Installed CMake package (baseline 9161527) | Separate C consumer configured, linked and executed successfully |
| 64-bit shared ELF load-segment alignment (baseline 9161527) | 0x4000 (16 KB) verified for ARM64 and x86_64 |
| C/C++ formatting, workflow YAML syntax, diff whitespace | Passed |
| Capstone/Unicorn harness + Dobby with ASan/UBSan | 4,576 near-placement comparisons passed (256 generated cases + 30 fixtures, each with 16 states; packaged emulator/disassembler libraries uninstrumented) |
| Capstone 5.0.9 + Unicorn 2.1.4, ARM64 real relocation generator | 1,000 generated cases + 30 fixtures per placement, each with 16 states: 32,960 near/far comparisons passed; mutation controls detected corruption |
| x86/x64 production relocation generators under Unicorn | Fixed corpus and explicit rejection checks passed with and without ASan/UBSan; includes short/near/internal branches, RIP loads and 64-bit immediates |
| ARM/Thumb production relocator under Unicorn | Arithmetic, backward branches, calls, word/byte literals, rotated ARM ADR, Thumb ADR/IT, mapping-end reads and explicit rejection checks passed with ASan/UBSan |
| ARM64 assembler vs LLVM MC | 6,151 valid encodings matched; invalid operands and failed finalization rejected |
| Target analysis and patch coordinator | Analysis leaves registry/pools unchanged; stale bytes and destructive callback reentry rejected; enter refusal/leave pairing tested |
| GLOB_DAT, version selection and inspection | Real getppid GOT slot selected by LIBC version, changed/restored, and ownership/original inspected; missing version rejected |
| Fuzz failure replay (baseline 9161527) | Valid fixture passes; deliberate BRK failure saves and replays the identical artifact |
| Batch failure injection | Static ARM64/ARM32 commit failure restores previous hooks; rollback failure retains ownership for retry |
| Module/import lifecycle | Real shared fixture pinned/unpinned, callback ownership checked, getpid PLT changed/restored, RELRO permissions preserved |
| x86/x86_64 Android runtime | Not run: no matching device |
| Actual 16 KB page runtime | Not run: available device uses 4 KB |
| BTI enforcement / PAC hardware | Not run: hardware capabilities unavailable; instruction-hint paths tested only |
| LeakSanitizer | Unsupported on this Android environment; no heap-leak certification |

The local runner records logs and runtime values in
`build/engine-runtime.json` and `build/engine-shared-runtime.json`.
These generated files are not source-controlled. Reproduce the matrix with
[the build/device runner](build-documentation.md); the manual device workflow
uploads its JSON artifact. Required feature flags make unavailable hardware a
failure rather than a silently passing check.

Emulator commands and coverage are documented in [the relocation test guide](../tests/relocation/README.md).
These semantic checks exposed a W-literal load incorrectly emitted as a 64-bit
load; destination-width selection was fixed and the regression now passes.

Known limits: ELF import hooking covers standard REL/RELA JUMP_SLOT/GLOB_DAT and
Android APS2 GLOB_DAT streams; Thumb IT support is a documented subset;
x86 VEX/EVEX and LOOP/JCXZ relocation is rejected;
short-function checks are conservative; patching is not atomic against executing
threads; collection and complete pool release require application-proven
quiescence. See [API contracts](api.md).
