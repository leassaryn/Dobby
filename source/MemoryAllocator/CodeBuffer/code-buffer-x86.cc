#include "platform_macro.h"
#if defined(TARGET_ARCH_IA32)

#include "MemoryAllocator/CodeBuffer/code-buffer-x86.h"

#include <cstring>

void CodeBuffer::Emit32(int32_t data) {
  ensureCapacity(getSize() + sizeof(int32_t));
  memcpy(getCursor(), &data, sizeof(data));
  buffer_cursor += sizeof(int32_t);
  return;
}

void CodeBuffer::FixBindLabel(int offset, int32_t disp) {
  memcpy(buffer + offset, &disp, sizeof(disp));
  return;
}

#endif