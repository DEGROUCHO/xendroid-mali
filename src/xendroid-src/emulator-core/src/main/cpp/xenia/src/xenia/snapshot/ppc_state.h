#ifndef XENIA_SNAPSHOT_PPC_STATE_H_
#define XENIA_SNAPSHOT_PPC_STATE_H_
#include <bit>
#include "xenia/snapshot/state_file.h"
#include "xenia/cpu/ppc/ppc_context.h"
namespace xe::snapshot {
// Only guest architectural data. Host pointers, native stacks and JIT address
// maps belong to the new runtime and are never copied from a snapshot.
struct PpcState {
  uint32_t thread_id=0, pc=0;
  uint64_t lr=0,ctr=0,msr=0;
  std::array<uint64_t,32> gpr{},fpr_bits{};
  std::array<std::array<uint32_t,4>,128> vectors{};
  std::array<uint32_t,4> vscr_vector{};
  std::array<uint32_t,8> cr{};
  uint32_t fpscr=0,vrsave=0,xer_ca=0,xer_ov=0,xer_so=0,vscr_sat=0;
  static PpcState Capture(const cpu::ppc::PPCContext& c,uint32_t safe_pc) {
    PpcState s;s.thread_id=c.thread_id;s.pc=safe_pc;s.lr=c.lr;s.ctr=c.ctr;s.msr=c.msr;
    for(unsigned i=0;i<32;++i){s.gpr[i]=c.r[i];s.fpr_bits[i]=std::bit_cast<uint64_t>(c.f[i]);}
    for(unsigned i=0;i<128;++i)for(unsigned j=0;j<4;++j)s.vectors[i][j]=c.v[i].u32[j];
    for(unsigned j=0;j<4;++j)s.vscr_vector[j]=c.vscr_vec.u32[j];
    s.cr={c.cr0.value,c.cr1.value,c.cr2.value,c.cr3.value,c.cr4.value,c.cr5.value,c.cr6.value,c.cr7.value};
    s.fpscr=c.fpscr.value;s.vrsave=c.vrsave;s.xer_ca=c.xer_ca;s.xer_ov=c.xer_ov;s.xer_so=c.xer_so;s.vscr_sat=c.vscr_sat;return s;
  }
  bool Apply(cpu::ppc::PPCContext& c) const {
    if(c.thread_id!=thread_id||!pc||(pc&3))return false;
    c.lr=lr;c.ctr=ctr;c.msr=msr;
    for(unsigned i=0;i<32;++i){c.r[i]=gpr[i];c.f[i]=std::bit_cast<double>(fpr_bits[i]);}
    for(unsigned i=0;i<128;++i)for(unsigned j=0;j<4;++j)c.v[i].u32[j]=vectors[i][j];
    for(unsigned j=0;j<4;++j)c.vscr_vec.u32[j]=vscr_vector[j];
    c.cr0.value=cr[0];c.cr1.value=cr[1];c.cr2.value=cr[2];c.cr3.value=cr[3];
    c.cr4.value=cr[4];c.cr5.value=cr[5];c.cr6.value=cr[6];c.cr7.value=cr[7];
    c.fpscr.value=fpscr;c.vrsave=vrsave;c.xer_ca=uint8_t(xer_ca);c.xer_ov=uint8_t(xer_ov);c.xer_so=uint8_t(xer_so);c.vscr_sat=uint8_t(vscr_sat);
    c.preempt_requested=0;c.last_safepoint_pc=pc;return true;
  }
  std::vector<uint8_t> Encode() const {
    std::vector<uint8_t> out;Put32(out,1);Put32(out,thread_id);Put32(out,pc);Put64(out,lr);Put64(out,ctr);Put64(out,msr);
    for(auto v:gpr)Put64(out,v);for(auto v:fpr_bits)Put64(out,v);
    for(auto& v:vectors)for(auto lane:v)Put32(out,lane);for(auto lane:vscr_vector)Put32(out,lane);
    for(auto v:cr)Put32(out,v);for(auto v:{fpscr,vrsave,xer_ca,xer_ov,xer_so,vscr_sat})Put32(out,v);return out;
  }
  static bool Decode(std::span<const uint8_t> bytes,PpcState& out) {
    Reader r(bytes);PpcState s;uint32_t version;
    if(!r.U32(version)||version!=1||!r.U32(s.thread_id)||!r.U32(s.pc)||!s.pc||(s.pc&3)||!r.U64(s.lr)||!r.U64(s.ctr)||!r.U64(s.msr))return false;
    for(auto& v:s.gpr)if(!r.U64(v))return false;for(auto& v:s.fpr_bits)if(!r.U64(v))return false;
    for(auto& v:s.vectors)for(auto& lane:v)if(!r.U32(lane))return false;for(auto& lane:s.vscr_vector)if(!r.U32(lane))return false;
    for(auto& v:s.cr)if(!r.U32(v)||(v&0xFEFEFEFEu))return false;
    for(auto* v:{&s.fpscr,&s.vrsave,&s.xer_ca,&s.xer_ov,&s.xer_so,&s.vscr_sat})if(!r.U32(*v))return false;
    if(r.remaining()||s.xer_ca>1||s.xer_ov>1||s.xer_so>1||s.vscr_sat>1)return false;
    out=s;return true;
  }
};
}
#endif
