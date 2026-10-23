#include "dobby_internal.h"
#include "InstructionRelocation/InstructionPlan.h"
#include "UserMode/PlatformUtil/ProcessRuntimeUtility.h"
#if defined(TARGET_ARCH_IA32) || defined(TARGET_ARCH_X64)
#include "InstructionRelocation/x86/InstructionPlanX86.h"
#endif
#include <algorithm>

PUBLIC int DobbyAnalyzeTarget(void *target, void *replacement, const DobbyHookOptions *options,
                              DobbyTargetAnalysis *out) {
  DobbySetError(DOBBY_ERROR_NONE);
  if (!target || !replacement || !out || out->struct_size < sizeof(*out) || out->api_version != DOBBY_API_VERSION ||
      (options && (options->struct_size < sizeof(*options) || options->api_version != DOBBY_API_VERSION ||
                   options->near_policy < DOBBY_NEAR_AUTO || options->near_policy > DOBBY_NEAR_DISABLED)))
    return DobbyFailure(DOBBY_ERROR_INVALID_ARGUMENT);
  *out = {sizeof(*out), DOBBY_API_VERSION};
  auto fail = [&](DobbyError error) {
    out->reason = error;
    return DobbyFailure(error);
  };
  uintptr_t pc = reinterpret_cast<uintptr_t>(target);
  bool thumb = false;
#if defined(TARGET_ARCH_ARM)
  thumb = pc & 1;
  pc &= ~uintptr_t{1};
#endif
  size_t available = 0;
  for (const auto &r : ProcessRuntimeUtility::GetProcessMemoryLayout()) {
    uintptr_t start = reinterpret_cast<uintptr_t>(r.address);
    if (r.permission != kNoAccess && pc >= start && pc - start < r.length) {
      available = std::min(size_t{256}, r.length - (pc - start));
      break;
    }
  }
  if (!available)
    return fail(DOBBY_ERROR_INVALID_ARGUMENT);
  size_t required = 0, prefix = 0;
  out->route = DOBBY_ROUTE_DIRECT;
#if defined(TARGET_ARCH_ARM64)
  if ((pc & 3) || available < 4)
    return fail(DOBBY_ERROR_INVALID_ARGUMENT);
  uint32_t first;
  memcpy(&first, reinterpret_cast<void *>(pc), 4);
  if (first == 0xd503245fU || first == 0xd503249fU || first == 0xd50324dfU)
    prefix = 4;
  const bool pac = first == 0xd503233fU || first == 0xd503237fU;
  const int64_t distance = static_cast<int64_t>(reinterpret_cast<uintptr_t>(replacement)) -
                           static_cast<int64_t>(pc + prefix + (pac ? 4 : 0));
  if (!(distance & 3) && distance >= -(int64_t{1} << 27) && distance < (int64_t{1} << 27))
    required = 4;
  else {
#ifdef DOBBY_HAS_NEAR_BRANCH
    if (!options || options->near_policy != DOBBY_NEAR_DISABLED) {
      required = 4;
      out->route = DOBBY_ROUTE_RELAY_CANDIDATE;
    } else
#endif
    {
      required = 16;
      out->route = DOBBY_ROUTE_LONG;
    }
  }
#ifndef DOBBY_HAS_NEAR_BRANCH
  if (options && options->near_policy == DOBBY_NEAR_REQUIRED)
    return fail(DOBBY_ERROR_UNSUPPORTED_FORMAT);
#endif
  if (pac)
    required += 4;
#elif defined(TARGET_ARCH_ARM)
  required = thumb ? 8 + (pc % 4 ? 2 : 0) : 8;
  out->route = DOBBY_ROUTE_LONG;
#else
  required = 5;
#if defined(TARGET_ARCH_X64)
  out->route = DOBBY_ROUTE_RELAY_CANDIDATE;
#endif
#endif
#if !defined(TARGET_ARCH_ARM64)
  if (options && options->near_policy == DOBBY_NEAR_REQUIRED)
    return fail(DOBBY_ERROR_UNSUPPORTED_FORMAT);
#endif
  size_t consumed = prefix;
  while (consumed < prefix + required) {
    if (out->instruction_count == 64 || consumed >= available)
      return fail(DOBBY_ERROR_PATCH_LIMIT);
    dobby::Instruction i;
    const auto address = pc + consumed;
#if defined(TARGET_ARCH_ARM64)
    if (available - consumed < 4)
      return fail(DOBBY_ERROR_PATCH_LIMIT);
    uint32_t word;
    memcpy(&word, reinterpret_cast<void *>(address), 4);
    i = dobby::DecodeArm64(word, address);
#elif defined(TARGET_ARCH_ARM)
    if (available - consumed < 2)
      return fail(DOBBY_ERROR_PATCH_LIMIT);
    uint32_t word = 0;
    memcpy(&word, reinterpret_cast<void *>(address), 2);
    const size_t size = thumb ? (dobby::ThumbWide(word) ? 4 : 2) : 4;
    if (available - consumed < size)
      return fail(DOBBY_ERROR_PATCH_LIMIT);
    memcpy(&word, reinterpret_cast<void *>(address), size);
    if (thumb) {
      i = dobby::DecodeThumb(word, address);
      if ((word & 0xff00) == 0xbf00 && (word & 15)) {
        const auto block = dobby::ThumbItSize(reinterpret_cast<void *>(address), available - consumed);
        if (block) {
          i.kind = dobby::InstructionKind::ItBlock;
          i.size = block;
        }
      }
    } else
      i = dobby::DecodeArm(word, address);
#else
    x86_insn_decode_t decoded{};
    x86_options_t config{};
#if defined(TARGET_ARCH_X64)
    config.mode = 64;
#else
    config.mode = 32;
#endif
    x86_insn_decode_bounded(&decoded, reinterpret_cast<uint8_t *>(address), std::min(size_t{15}, available - consumed),
                            &config);
    i = dobby::ClassifyX86(decoded, reinterpret_cast<void *>(address), address);
    if (i.kind == dobby::InstructionKind::Unsupported)
      return fail(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
#endif
    auto &record = out->instructions[out->instruction_count++];
    record = {i.pc,   i.target,       i.reads,        i.writes, static_cast<uint32_t>(i.kind),
              i.size, i.literal_size, i.effects_known};
    if (i.kind == dobby::InstructionKind::Unsupported)
      return fail(DOBBY_ERROR_UNSUPPORTED_INSTRUCTION);
    consumed += i.size;
    if ((i.kind == dobby::InstructionKind::Terminal || i.kind == dobby::InstructionKind::Branch) &&
        consumed < prefix + required)
      return fail(DOBBY_ERROR_FUNCTION_TOO_SHORT);
  }
  out->patch_span = consumed;
  out->preserved_prefix = prefix;
  if (options && options->max_patch_bytes && consumed > options->max_patch_bytes)
    return fail(DOBBY_ERROR_PATCH_LIMIT);
  (void)thumb;
  return DobbySuccess();
}
