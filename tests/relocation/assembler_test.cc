#include "dobby_internal.h"
#include "core/modules/assembler/assembler-arm64.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <random>
using namespace zz::arm64;
template <class F> void Case(const std::string &text, F emit) {
  Assembler a(nullptr);
  emit(a);
  if (!a.valid() || a.GetCodeBuffer()->getSize() != 4)
    std::abort();
  uint32_t word;
  memcpy(&word, a.GetCodeBuffer()->getRawBuffer(), 4);
  std::printf("%08x|%s\n", word, text.c_str());
}
template <class F> void Reject(F emit) {
  Assembler a(nullptr);
  DobbySetError(DOBBY_ERROR_NONE);
  emit(a);
  if (a.valid() || a.GetCodeBuffer()->getSize() || DobbyGetLastError() != DOBBY_ERROR_INVALID_ENCODING ||
      AssemblyCodeBuilder::FinalizeFromTurboAssembler(&a))
    std::abort();
}
int main() {
  Reject([](auto &a) { a.b(2); });
  Reject([](auto &a) { a.b(int64_t{1} << 27); });
  Reject([](auto &a) { a.add(X(0), W(1), 1); });
  Reject([](auto &a) { a.add(X(0), X(1), 4096); });
  Reject([](auto &a) { a.str(W(0), MemOperand(X(1), 3)); });
  Reject([](auto &a) { a.movz(W(0), 1, 32); });
  Reject([](auto &a) { a.movk(X(0), 65536, 0); });
  Reject([](auto &a) { a.ldr(X(0), int64_t{1} << 20); });
  Reject([](auto &a) { a.adrp(X(0), 1); });
  Reject([](auto &a) { a.br(X(31)); });
  std::mt19937 rng(0xd0bb);
  for (unsigned n = 0; n < 512; ++n) {
    int r = rng() % 31, base = rng() % 31, imm = rng() % 4096;
    for (bool wide : {false, true}) {
      const auto rd = wide ? X(r) : W(r), rn = wide ? X(base) : W(base);
      const auto d = std::string(wide ? "x" : "w") + std::to_string(r);
      const auto b = std::string(wide ? "x" : "w") + std::to_string(base);
      Case("add " + d + ", " + b + ", #" + std::to_string(imm), [&](auto &a) { a.add(rd, rn, imm); });
      Case("subs " + d + ", " + b + ", #" + std::to_string(imm), [&](auto &a) { a.subs(rd, rn, imm); });
      const int off = imm * (wide ? 8 : 4);
      Case("str " + d + ", [x" + std::to_string(base) + ", #" + std::to_string(off) + "]",
           [&](auto &a) { a.str(rd, MemOperand(X(base), off)); });
      Case("ldr " + d + ", [x" + std::to_string(base) + ", #" + std::to_string(off) + "]",
           [&](auto &a) { a.ldr(rd, MemOperand(X(base), off)); });
      const int literal = (int(rng() % 524288) - 262144) * 4;
      Case("ldr " + d + ", #" + std::to_string(literal), [&](auto &a) { a.ldr(rd, literal); });
      const int shift = (rng() % (wide ? 4 : 2)) * 16, value = rng() % 65536;
      Case("movk " + d + ", #" + std::to_string(value) + ", lsl #" + std::to_string(shift),
           [&](auto &a) { a.movk(rd, value, shift); });
    }
  }
  for (int64_t offset : {-(int64_t{1} << 27), int64_t{-4}, int64_t{0}, (int64_t{1} << 27) - 4})
    Case("b #" + std::to_string(offset), [&](auto &a) { a.b(offset); });
  for (int64_t offset : {-(int64_t{1} << 32), int64_t{4096}, (int64_t{1} << 32) - 4096})
    Case("adrp x2, #" + std::to_string(offset), [&](auto &a) { a.adrp(X(2), offset); });
}
