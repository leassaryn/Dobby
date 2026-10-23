#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>

namespace dobby {
// Android APS2: bounded SLEB128 decoding, independent of pointer width.
class PackedReader {
public:
  PackedReader(const uint8_t *data, size_t size) : data_(data), size_(size) {
  }
  bool Read(int64_t &result) {
    uint64_t value = 0;
    for (unsigned n = 0; n < 10; ++n) {
      if (cursor_ == size_)
        return false;
      const uint8_t byte = data_[cursor_++];
      if (n == 9 && byte != 0 && byte != 0x7f)
        return false;
      value |= uint64_t(byte & 0x7f) << (n * 7);
      if (!(byte & 0x80)) {
        if (n < 9 && (byte & 0x40))
          value |= ~uint64_t{0} << ((n + 1) * 7);
        memcpy(&result, &value, sizeof(result));
        return true;
      }
    }
    return false;
  }
  bool Done() const {
    return cursor_ == size_;
  }

private:
  const uint8_t *data_;
  size_t size_, cursor_ = 0;
};
inline bool AddSigned(int64_t &value, int64_t delta) {
  if ((delta > 0 && value > INT64_MAX - delta) || (delta < 0 && value < INT64_MIN - delta))
    return false;
  value += delta;
  return true;
}
template <typename Visitor> bool VisitPacked(const void *bytes, size_t size, bool rela, unsigned bits, Visitor visit) {
  if (!bytes || size < 4 || memcmp(bytes, "APS2", 4) || (bits != 32 && bits != 64))
    return false;
  PackedReader reader(static_cast<const uint8_t *>(bytes) + 4, size - 4);
  int64_t count, offset;
  // Bound work independently of compressed input size (group compression can be extreme).
  if (!reader.Read(count) || count < 0 || count > 1048576 || !reader.Read(offset) || offset < 0)
    return false;
  int64_t addend = 0;
  for (int64_t index = 0; index < count;) {
    int64_t group, flags, delta = 0, info = 0, add = 0;
    if (!reader.Read(group) || group <= 0 || group > count - index || !reader.Read(flags) || flags < 0 || flags > 15 ||
        ((flags & 4) && !(flags & 8)) || (!rela && (flags & 8)))
      return false;
    if ((flags & 2) && !reader.Read(delta))
      return false;
    if ((flags & 1) && (!reader.Read(info) || info < 0))
      return false;
    if (!(flags & 8))
      addend = 0;
    else if (flags & 4) {
      if (!reader.Read(add) || !AddSigned(addend, add))
        return false;
    }
    for (int64_t member = 0; member < group; ++member, ++index) {
      if (!(flags & 2) && !reader.Read(delta))
        return false;
      if (!AddSigned(offset, delta) || offset < 0)
        return false;
      if (!(flags & 1) && (!reader.Read(info) || info < 0))
        return false;
      if ((flags & 8) && !(flags & 4) && (!reader.Read(add) || !AddSigned(addend, add)))
        return false;
      if (bits == 32 &&
          (uint64_t(offset) > UINT32_MAX || uint64_t(info) > UINT32_MAX || addend < INT32_MIN || addend > INT32_MAX))
        return false;
      if (!visit(uint64_t(offset), uint64_t(info), addend))
        return false;
    }
  }
  return reader.Done();
}
} // namespace dobby
