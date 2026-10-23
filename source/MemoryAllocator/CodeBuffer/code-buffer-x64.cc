#include "platform_macro.h"
#if defined(TARGET_ARCH_X64)

#include "MemoryAllocator/CodeBuffer/code-buffer-x64.h"

#include <cstring>

void CodeBuffer::Emit32(int32_t data) {
  this->ensureCapacity(this->getSize() + sizeof(int32_t));
  memcpy(buffer_cursor, &data, sizeof(data));
  buffer_cursor += sizeof(int32_t);
  return;
}

void CodeBuffer::Emit64(int64_t data) {
  this->ensureCapacity(this->getSize() + sizeof(int64_t));
  memcpy(buffer_cursor, &data, sizeof(data));
  buffer_cursor += sizeof(int64_t);
  return;
}

void CodeBuffer::FixBindLabel(int offset, int32_t disp) {
  memcpy(buffer + offset, &disp, sizeof(disp));
  return;
}

#endif