# Compatibility evidence

Local verification on 2026-10-23, using Android NDK 29.0.14206865 and CMake 4.1.2.
These results describe this working tree; they do not imply hosted CI or an
unavailable device matrix has run.

| Check | Result |
| --- | --- |
| Release static and shared builds, all four Android ABIs | Passed, no compiler/linker warnings |
| ARM64 Android 14 / API 34, 4096-byte pages | Compatibility, C API, safety, linker-callback and modern-API tests passed in static and shared configurations |
| ARM32 Android 14 / API 34, 4096-byte pages | Compatibility (ARM and Thumb), C API, linker-callback and modern-API tests passed in static and shared configurations |
| ARM64 core smoke linked to the Android archive | 23 passed, 0 failed |
| ARM64 ASan + UBSan | Compatibility, C API, safety, linker-callback and modern-API tests passed; function-type checking excluded for generated code |
| ARM64 Debug | Build and compatibility runtime passed |
| ARM64 minimal Release, DBI/near/symbol/linker plugins disabled | Build and compatibility runtime passed |
| Installed CMake package | Separate C consumer configured, linked and executed successfully |
| 64-bit shared ELF load-segment alignment | 0x4000 (16 KB) verified for ARM64 and x86_64 |
| C/C++ formatting, workflow YAML syntax, diff whitespace | Passed |
| Capstone/Unicorn harness + Dobby with ASan/UBSan | 4,560 near-placement comparisons passed (256 generated cases + 29 fixtures, each with 16 states; packaged emulator/disassembler libraries uninstrumented) |
| Capstone 5.0.9 + Unicorn 2.1.4, ARM64 real relocation generator | 1,000 generated cases + 29 fixtures per placement, each with 16 states: 32,928 near/far comparisons passed; mutation controls detected corruption |
| x86/x64 production relocation generators under Unicorn | Fixed corpus and explicit rejection checks passed with and without ASan/UBSan; includes short/near/internal branches, RIP loads and 64-bit immediates |
| Fuzz failure replay | Valid fixture passes; deliberate BRK failure saves and replays the identical artifact |
| Batch failure injection | Static ARM64/ARM32 commit failure restores previous hooks; rollback failure retains ownership for retry |
| Module/import lifecycle | Real shared fixture pinned/unpinned, callback ownership checked, getpid PLT changed/restored, RELRO permissions preserved |
| x86/x86_64 Android runtime | Not run: no matching device |
| Actual 16 KB page runtime | Not run: available device uses 4 KB |
| BTI enforcement / PAC hardware | Not run: hardware capabilities unavailable; instruction-hint paths tested only |
| LeakSanitizer | Unsupported on this Android environment; no heap-leak certification |

The local runner records logs and runtime values in
`build/seven-runtime-final.json` and `build/seven-shared-final.json`.
These generated files are not source-controlled. Reproduce the matrix with
[the build/device runner](build-documentation.md); the manual device workflow
uploads its JSON artifact. Required feature flags make unavailable hardware a
failure rather than a silently passing check.

Emulator commands and coverage are documented in [the relocation test guide](../tests/relocation/README.md).
These semantic checks exposed a W-literal load incorrectly emitted as a 64-bit
load; destination-width selection was fixed and the regression now passes.

Known limits: ELF import hooking covers standard REL/RELA PLT JUMP_SLOT entries;
x86 VEX/EVEX and LOOP/JCXZ relocation is rejected;
short-function checks are conservative; patching is not atomic against executing
threads; collection and complete pool release require application-proven
quiescence. See [API contracts](api.md).
