# Assembler and relocation engine

## Checked encoding

Assembler instances carry a sticky validity flag. Operand rejection reports
`DOBBY_ERROR_INVALID_ENCODING`; a failed assembler cannot be finalized into
executable code. ARM64 checks branch/literal/ADRP reach and alignment, immediate
width, GPR width, memory-offset scaling, pair offsets and wide-move shifts.
ARM/Thumb branch emitters also check alignment/range. This is not a new general
purpose assembler: unsupported operand forms are rejected, and raw instruction
emission remains an internal escape hatch.

Literal offsets are byte offsets. ARM64 literal-load encoding now scales them by
four correctly; W-register stores select the 32-bit opcode. ADD/SUB flag-setting
variants are implemented. Logical-immediate construction remains unsupported
rather than accepting an ambiguous raw bitfield. Existing trampoline/relocation
callers retain their specialized emitters.

`dobby_assembler_test` generates 6,151 valid encodings plus invalid-operand checks.
`check_llvm_encoding.py` compares the output with
[LLVM MC](https://www.llvm.org/docs/CommandGuide/llvm-mc.html). LLVM is a test-only
dependency. This oracle covers ARM64 encoders, not every assembler backend.

## Shared instruction records

`source/InstructionRelocation/InstructionPlan.h` defines instruction kind, length,
source/target addresses, literal width and GPR effects. The x86 adapter shares
classification with the existing bounded decoder. ARM64 and Thumb/ARM classifiers
are shared by the relocation engine and target analysis. Records explicitly mark
unknown register effects; a zero mask must not be interpreted as proof of no
register use. Flags/SIMD effects are not represented by the GPR masks. ARM64's
existing X16/X17 scratch-register contract remains in force.

The representation is deliberately partial. It identifies the PC-relative and
control-flow families implemented by each backend; it is not an exhaustive ISA
validator. Unsupported forms detected by the backend fail before publication.

## Layout, fixups and literals

Relocation emits expansion slots, builds an original-instruction-to-output map,
then resolves branch targets once the final allocation and pool layout are known.
ARM64 retains fixed-size expansion slots when replacing an indirect branch with
a direct branch plus NOP. Consequently, late shortening does not invalidate other
offsets or require a branch-relaxation loop. No iterative size-minimization pass
is provided.

ARM64 literals fully inside the displaced span are snapshotted into an aligned
pool after the branch literals. Their address-materialization sites are fixed
up after allocation. A literal straddling the displaced boundary is rejected.
This protects relocated reads from the subsequently patched source bytes.
ARM/Thumb overlapping literals remain conservatively unsupported.

ARM and Thumb backward branch immediates are sign-extended, internal targets are
remapped, and targets in the middle of relocated instructions fail. ARM word/byte
literal widths and rotated ADR immediates are preserved. Conditional ARM literal
loads/ADR and BLX-immediate remain unsupported. ARM branch
labels are distinguished from data/address labels. Thumb ADR includes PC
alignment and immediate scaling.

Supported Thumb IT blocks are copied as indivisible units, including members
past the minimum patch span. The supported members are 16-bit immediate
MOV/CMP/ADD/SUB, immediate shifts, low-register ADD/SUB and data-processing
operations, SP-relative word loads/stores, and ADD of SP plus an immediate into
a low register. PC operands remain excluded. No alignment NOP or
expanded instruction is inserted inside the block. Other IT members are rejected;
this is not complete Thumb-2 IT rewriting. Callers must supply a real instruction
entry, not an address inside an existing IT block.

The copy predicate retains ITSTATE and the original instruction encodings,
including flag behavior described by [Arm's conditional execution documentation](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/condition-codes-3-conditional-execution-in-thumb-2).
The emulator checks 22 added member encodings in two- and four-member blocks,
with taken/skipped predicates, register/flag results and stack writes compared
against the original code. Truncated blocks and PC-relative/control-flow members
remain rejected. This extends existing binary compatibility; it does not promote
multi-instruction IT blocks for newly generated Armv8 code.

## Read-only target analysis

```c
DobbyTargetAnalysis analysis = {
    .struct_size = sizeof(analysis), .api_version = DOBBY_API_VERSION,
};
int result = DobbyAnalyzeTarget(target, replacement, NULL, &analysis);
```

The result contains an estimated patch span, preserved prefix, direct/long/relay
candidate route, instruction records and a failure reason. It allocates no
executable pages, changes no target bytes, and registers no hook. An IT-block
record includes the whole supported block and is marked `DOBBY_INSN_IT_BLOCK`.
Analysis uses bounded reads within the current mapping; the caller must keep the
module mapped and its code stable while analyzing.

A relay candidate does not promise near memory availability. Analysis does not
execute the relocator or establish that every instruction can be relocated at
the eventual output address. `DobbyPrepareEx` remains the definitive preparation
step; its byte-limit check includes whole displaced instructions/IT blocks.

## Application-owned patch coordination

`DobbySetPatchCoordinator` installs versioned `enter`/`leave` callbacks and userdata.
The engine invokes them around live inline/DBI install, removal and import-slot
updates, while holding its management mutex. It does not invoke them for writes
to newly allocated staging/trampoline memory.

`enter` returns `RS_SUCCESS` only after the application has established quiescence.
A refusal returns `DOBBY_ERROR_BUSY`, writes nothing, and does not call `leave`.
After successful entry, `leave` runs on both success and failure; `applied` says
whether the bytes were written. Last-error diagnostics survive `leave`.
Callbacks must not throw, wait for another thread to enter Dobby management APIs,
or call management APIs recursively. Destructive reentry is rejected as busy.
Read-only queries are allowed. Passing NULL clears the coordinator; callback code
and userdata must stay valid until then. Module-release checks include registered
coordinator functions.

The engine compares the prepared prologue with current bytes after entering the
safe point. A mismatch makes commit fail with `DOBBY_ERROR_CONFLICT`. Removal
likewise refuses to overwrite a different installed patch and retains ownership
for recovery. Import writes compare their expected slot value inside the safe
point. These checks do not replace quiescence or stop arbitrary external writers.
Callbacks run per patch; batch installation is still not atomic publication.

## Extended import backend

`DobbyHookImportEx` accepts `DobbyImportOptions`: `symbol_version` selects an
imported symbol's `DT_VERNEED` version, and flags select standard PLT `JUMP_SLOT`,
standard dynamic-table `GLOB_DAT`, or both. Null options preserve the legacy
PLT-only behavior. `GLOB_DAT` can refer to data as well as functions; the caller
must use a replacement of the appropriate type. Android APS2 packed GLOB_DAT
streams are supported; symbol-definition version lookup remains unimplemented.
See [plugin contracts](plugins.md) for parser bounds and lifecycle details.

`DobbyInspectImport` supports a count query followed by caller-provided slot
storage. It reports slot/current/original addresses, Dobby owner handle,
relocation type, version index and conflicts with the expected installed value.
Only Dobby-owned slots have a known original/owner. Other engines cannot be
identified from an arbitrary pointer value. Retain a module handle while using
returned slot addresses. Inspection is a snapshot, not a reservation.

The backend follows standard ELF tables used by
[Bionic's relocation implementation](https://android.googlesource.com/platform/bionic/+/4b4fb6f43/linker/linker_relocate.cpp).
Existing RELRO restoration, module pinning and retryable failed rollback rules
remain applicable.
