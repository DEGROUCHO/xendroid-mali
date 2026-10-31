#ifndef XENIA_SNAPSHOT_GPU_STATE_H_
#define XENIA_SNAPSHOT_GPU_STATE_H_
#include "xenia/snapshot/state_file.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
namespace xe::snapshot {
struct GpuState {
  std::array<uint32_t,gpu::RegisterFile::kRegisterCount> registers{};
  std::array<uint32_t,6> ring{};
  std::array<uint32_t,256> gamma{};
  std::array<std::array<uint32_t,3>,128> gamma_pwl{};
  uint32_t gamma_component=0,interrupt_callback=0,interrupt_data=0;
  uint32_t backend=1,scale_x=1,scale_y=1; // 1 = raw interlock EDRAM, 2 = canonical host RT RGBA8 raster
  uint64_t bin_select=~uint64_t{0},bin_mask=~uint64_t{0};
  uint32_t counter=0,vertex_ucode_address=0;
  std::vector<uint32_t> vertex_shader,pixel_shader;
  std::vector<uint8_t> edram;
  bool Validate() const {
    if ((backend != 1 && backend != 2) || scale_x != 1 || scale_y != 1 ||
        gamma_component > 2 || edram.size() != gpu::xenos::kEdramSizeBytes ||
        vertex_shader.size() > 65536 || pixel_shader.size() > 65536 ||
        (interrupt_callback & 3) || (vertex_ucode_address & 3)) return false;
    if (!ring[1]) return !ring[0] && !ring[2] && !ring[4] &&
                            (!ring[5] || ring[5] == 0xBAADF00D);
    const uint64_t physical_end = uint64_t(ring[0] & 0x1FFFFFFF) + ring[1];
    if ((ring[0] & 3) || ring[1] < 8 || (ring[1] & (ring[1] - 1)) ||
        physical_end > 0x20000000 || ring[2] >= ring[1] / 4 ||
        (ring[5] != 0xBAADF00D && ring[5] >= ring[1] / 4) ||
        (ring[4] & 3) || (ring[4] && (ring[4] & 0x1FFFFFFF) > 0x1FFFFFFC)) return false;
    return true;
  }
  std::vector<uint8_t> Encode() const {
    if (!Validate()) return {};
    std::vector<uint8_t> out;out.reserve(edram.size()+registers.size()*4+4096);
    for(auto v:{uint32_t(2),backend,scale_x,scale_y,interrupt_callback,interrupt_data,gamma_component})Put32(out,v);
    for(auto v:ring)Put32(out,v);for(auto v:registers)Put32(out,v);for(auto v:gamma)Put32(out,v);
    for(auto& v:gamma_pwl)for(auto channel:v)Put32(out,channel);
    Put64(out,bin_select);Put64(out,bin_mask);Put32(out,counter);Put32(out,vertex_ucode_address);
    for(const auto* shader:{&vertex_shader,&pixel_shader}){Put32(out,uint32_t(shader->size()));for(auto word:*shader)Put32(out,word);}
    Put64(out,edram.size());out.insert(out.end(),edram.begin(),edram.end());return out;
  }
  static bool Decode(std::span<const uint8_t> bytes,GpuState& out) {
    Reader r(bytes);GpuState s;uint32_t version;
    if(!r.U32(version)||version!=2||!r.U32(s.backend)||(s.backend!=1&&s.backend!=2)||!r.U32(s.scale_x)||s.scale_x!=1||!r.U32(s.scale_y)||s.scale_y!=1||!r.U32(s.interrupt_callback)||!r.U32(s.interrupt_data)||!r.U32(s.gamma_component)||s.gamma_component>2)return false;
    for(auto& v:s.ring)if(!r.U32(v))return false;for(auto& v:s.registers)if(!r.U32(v))return false;
    for(auto& v:s.gamma)if(!r.U32(v))return false;for(auto& v:s.gamma_pwl)for(auto& channel:v)if(!r.U32(channel))return false;
    if(!r.U64(s.bin_select)||!r.U64(s.bin_mask)||!r.U32(s.counter)||!r.U32(s.vertex_ucode_address))return false;
    for(auto* shader:{&s.vertex_shader,&s.pixel_shader}){uint32_t count;if(!r.U32(count)||count>65536||uint64_t(count)*4>r.remaining())return false;shader->resize(count);for(auto& word:*shader)if(!r.U32(word))return false;}
    uint64_t size;if(!r.U64(size)||size!=gpu::xenos::kEdramSizeBytes||size!=r.remaining())return false;
    s.edram.resize(size);if(!r.Bytes(s.edram)||!s.Validate())return false;out=std::move(s);return true;
  }
  void CopyRegistersTo(gpu::RegisterFile& target) const {
    std::copy(registers.begin(),registers.end(),std::begin(target.values));
  }
};
}
#endif
