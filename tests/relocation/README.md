# Differential relocation tests

This test links the **actual Dobby relocation generator**. Capstone decodes every
executed source/generated instruction; Unicorn executes original and relocated
code independently from identical initial machine states. This is not a test of
a separate reimplementation of the relocator.

Requirements: a native ARM64 compiler with C++23, CMake 3.22+, pkg-config,
Capstone 5+ and Unicorn 2+. Dependency discovery is opt-in and test-only; Android
release libraries acquire no Capstone/Unicorn dependency. The native ARM64 test build also compiles the production x86/x64 generators as
separate test executables using test-only architecture overrides. Unicorn runs
their 32/64-bit guest code; this is not native Android x86 runtime coverage.
A native static build is required to access the private ARM64 generator.

```sh
pkg-config --modversion capstone unicorn
pkg-config --cflags --libs capstone unicorn

cmake -S . -B build/relocation-emulator \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DDOBBY_GENERATE_SHARED=OFF \
  -DDOBBY_BUILD_RELOCATION_EMULATOR_TESTS=ON \
  -DPlugin.LinkerLoadCallback=OFF
cmake --build build/relocation-emulator --parallel 4
ctest --test-dir build/relocation-emulator --output-on-failure
```

Use a separate build directory and add `-DNearBranch=OFF` to exercise far
relocation. In that configuration the harness supplies a synthetic guest source
PC (0x1000000), while real generated storage is outside the ARM64 B range; it
asserts that distance before executing. This forces long return/external-branch
sequences. With NearBranch enabled, source addresses are actual native mappings.

On the current Termux workspace, the functioning CMake/CTest binaries are under
`$HOME/android-sdk/cmake/4.1.2/bin/`; use those absolute paths if the system CMake
cannot start. This native development build is separate from NDK release builds.

## What is checked

30 ARM64 fixtures each run with 16 initial states (480 comparisons per placement):

- Arithmetic and writes; SP and stack-pair save/restore.
- Internal forward/backward branches, B/BL, CBZ/CBNZ, TBZ/TBNZ and conditional
  branches, including taken/fallthrough and external-target paths.
- ADR/ADRP and W/X/LDRSW/S/D/Q literal loads, including a negative 32-bit literal.
- The IL2CPP `ldr s0, [...] ; ret` short-method sequence, SIMD loads/stores,
  and a BTI instruction as an emulated hint (not enforcement certification).

The comparison checks X0–X30 except documented scratch registers X16/X17, all
32 Q registers, SP/NZCV/FPCR/FPSR, the entire data page and the entire stack page.
Both executions must reach the same stop PC. Instruction/time limits make loops
and bad targets fail. Executed instructions must decode successfully in Capstone;
inline literal data is not mistaken for executable instructions.

The relocated run poisons the displaced original bytes with BRK, detecting
incorrect branches back into the overwritten prologue. A negative control
replaces a generated arithmetic instruction with NOP and requires a mismatch.
The W-literal fixture exposed and guards a real bug where the assembler emitted
an X-register (64-bit) load for a W-register (32-bit) destination.

## Sanitizers and limits

Use a separate directory with the ASan/UBSan flags from
[the build guide](../../docs/build-documentation.md#sanitizers). On Termux, put
`$PREFIX/lib/clang/21/lib/linux:$PREFIX/lib` on `LD_LIBRARY_PATH` for the installed
Clang 21 sanitizer runtime. Run with `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`. Packaged Capstone/Unicorn shared libraries are not
rebuilt with instrumentation, so the sanitizer claim covers Dobby/the harness.

The 4096-byte Unicorn mapping granularity in this harness is not a measurement of
Android's kernel page size. These tests do not validate real mprotect behavior,
SELinux policy, instruction-cache coherence, concurrent live patching, 16 KB
Android deployment, or hardware BTI/PAC enforcement. Native device tests remain
necessary. Passing fixtures establish coverage of those sequences, not every
possible ARM64 instruction or arbitrary function.

## x86 and x64

The additional CTest executables check arithmetic, short/near jumps, conditional
branches and internal targets with 16 initial states. x64 also covers RIP-relative
loads and 64-bit immediate decoding. They compare integer registers, flags and
stack, poison overwritten source bytes with INT3, and check explicit rejection
of LOOP/JCXZ and VEX instructions. The decoder conservatively rejects VEX/EVEX
and unsupported extended opcode maps; this is not AVX or exhaustive ISA support.
These tests found incorrect short-branch next-IP adjustments and missing internal
branch remapping, now fixed in both production generators.

## Seeded fuzzing and replay

```sh
build/relocation-emulator/dobby_relocation_emulator_test \
  --cases 1000 --seed 855 --failure build/relocation-failure.txt
build/relocation-emulator/dobby_relocation_emulator_test \
  --replay build/relocation-failure.txt --failure build/replayed-failure.txt
```

The bounded generator combines integer arithmetic, flags, data loads/stores and
forward conditional branches. Each case executes with 16 states, in addition to
the fixed corpus. Repeat with NearBranch disabled. This is constrained semantic
fuzzing, not exhaustive arbitrary-byte fuzzing. A failing fixture is saved as a
versioned text artifact containing displaced size, argument mode, failing state
seed and instruction words. Replay validates the artifact and reruns all 16
states (including the recorded failing state); use the same near/far build.
Instruction/time budgets prevent generated loops from hanging the run. The
artifact is emitted when a fixture fails; setup/argument errors have no fixture.


## ARM/Thumb and LLVM assembler oracle

CTest also builds the actual ARM relocator with a test-only architecture override.
Its allocator uses a reserved low-address test mapping so 32-bit guest pointers
remain representable. Capstone checks executed instructions and Unicorn compares
GPRs, CPSR and stack for ARM/Thumb arithmetic, literal loads, backward branches,
Thumb ADR and whole supported IT blocks. Overwritten bytes are poisoned, and
unsupported IT members are explicitly rejected. This does not emulate every ARM
extension or replace native ARM32 device tests.

`dobby_assembler_llvm` compares 6,151 ARM64 encodings against `llvm-mc` and checks
invalid operands/finalization failure. Install LLVM's `llvm-mc` alongside the
existing test dependencies. No LLVM code is linked into the production library.
See [the engine guide](../../docs/engine.md) for exact coverage and contracts.
