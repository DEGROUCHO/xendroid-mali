#ifndef XENIA_CPU_BACKEND_A64_A64_VECTOR_STORE_PARTIAL_H_
#define XENIA_CPU_BACKEND_A64_A64_VECTOR_STORE_PARTIAL_H_

#include <cstdint>

namespace xe::cpu::backend::a64 {

// Shared tails of STVL / STVR. Both store a run of 1..15 bytes of the
// byte-swapped vector image and must write no byte outside that run (another
// thread may own the rest of the 16-byte line), so the run is covered by two
// possibly overlapping power-of-two stores instead of a byte loop.
//
// In:  x0  = first source byte (in the stack stash), x16 = first destination
//      byte, w1 = length (1..15, upper half of x1 clear).
// Clobbers x1, x2, x3, x6. Flags are untouched.
template <typename Emitter, typename Label>
void EmitStoreVectorRun(Emitter& e, Label& l8, Label& l4, Label& l2,
                        Label& done) {
  using namespace Xbyak_aarch64;
  e.add(e.x2, e.x0, e.x1);   // one past the last source byte
  e.add(e.x3, e.x16, e.x1);  // one past the last destination byte
  e.tbnz(e.w1, 3, l8);
  e.tbnz(e.w1, 2, l4);
  e.tbnz(e.w1, 1, l2);
  e.ldrb(e.w1, ptr(e.x0));
  e.strb(e.w1, ptr(e.x16));
  e.b(done);
  e.L(l8);  // 8..15 bytes
  e.ldr(e.x1, ptr(e.x0));
  e.ldur(e.x6, ptr(e.x2, -8));
  e.str(e.x1, ptr(e.x16));
  e.stur(e.x6, ptr(e.x3, -8));
  e.b(done);
  e.L(l4);  // 4..7 bytes
  e.ldr(e.w1, ptr(e.x0));
  e.ldur(e.w6, ptr(e.x2, -4));
  e.str(e.w1, ptr(e.x16));
  e.stur(e.w6, ptr(e.x3, -4));
  e.b(done);
  e.L(l2);  // 2..3 bytes
  e.ldrh(e.w1, ptr(e.x0));
  e.ldurh(e.w6, ptr(e.x2, -2));
  e.strh(e.w1, ptr(e.x16));
  e.sturh(e.w6, ptr(e.x3, -2));
  e.L(done);
}

// v0 = byte-swapped vector image, x0 = host effective address.
// stash_offset: 16 bytes of scratch at [sp, #stash_offset].
template <typename Emitter, typename Label>
void EmitStoreVectorLeft(Emitter& e, uint32_t stash_offset, Label& unaligned,
                         Label& l8, Label& l4, Label& l2, Label& done) {
  using namespace Xbyak_aarch64;
  e.and_(e.w17, e.w0, 0xF);
  e.cbnz(e.w17, unaligned);
  // Aligned: the whole vector, one store.
  e.str(QReg(0), ptr(e.x0));
  e.b(done);
  e.L(unaligned);
  // mem[ea + k] = image[k], k = 0 .. 15 - offset.
  e.str(QReg(0), ptr(e.sp, stash_offset));
  e.mov(e.x16, e.x0);
  e.mov(e.w1, 16);
  e.sub(e.w1, e.w1, e.w17);
  e.add(e.x0, e.sp, stash_offset);
  EmitStoreVectorRun(e, l8, l4, l2, done);
}

template <typename Emitter, typename Label>
void EmitStoreVectorRight(Emitter& e, uint32_t stash_offset, Label& l8,
                          Label& l4, Label& l2, Label& done) {
  using namespace Xbyak_aarch64;
  e.and_(e.w17, e.w0, 0xF);
  // Aligned: nothing to store.
  e.cbz(e.w17, done);
  // mem[(ea & ~15) + k] = image[16 - offset + k], k = 0 .. offset - 1.
  e.str(QReg(0), ptr(e.sp, stash_offset));
  e.and_(e.x16, e.x0, ~0xFull);
  e.add(e.x0, e.sp, stash_offset + 16);
  e.sub(e.x0, e.x0, e.x17);
  e.mov(e.w1, e.w17);
  EmitStoreVectorRun(e, l8, l4, l2, done);
}

}  // namespace xe::cpu::backend::a64

#endif  // XENIA_CPU_BACKEND_A64_A64_VECTOR_STORE_PARTIAL_H_
