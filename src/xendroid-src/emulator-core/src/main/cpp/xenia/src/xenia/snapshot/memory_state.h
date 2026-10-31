#ifndef XENIA_SNAPSHOT_MEMORY_STATE_H_
#define XENIA_SNAPSHOT_MEMORY_STATE_H_
#include "xenia/snapshot/state_file.h"
#include "xenia/memory.h"
namespace xe::snapshot {
struct HeapLayout {uint32_t base,page_size,page_count;};
inline bool ReadHeapLayout(Reader& r,const HeapLayout& expected) {
  uint32_t base,page_size,count;
  return r.U32(base)&&r.U32(page_size)&&r.U32(count)&&base==expected.base&&
      page_size==expected.page_size&&count==expected.page_count;
}
inline uint64_t EncodePage(PageEntry p) {
  return uint64_t(p.base_address)|(uint64_t(p.region_page_count)<<20)|
      (uint64_t(p.allocation_protect)<<40)|(uint64_t(p.current_protect)<<44)|
      (uint64_t(p.state)<<48)|(uint64_t(p.reserved)<<50);
}
inline PageEntry DecodePage(uint64_t wire) {
  PageEntry p{};p.base_address=wire&0xFFFFF;p.region_page_count=(wire>>20)&0xFFFFF;
  p.allocation_protect=(wire>>40)&15;p.current_protect=(wire>>44)&15;
  p.state=(wire>>48)&3;p.reserved=(wire>>50)&0x3FFF;return p;
}
inline bool ValidPage(PageEntry page,uint32_t index,uint32_t count) {
  if(page.reserved)return false;
  if(!page.state)return true;
  return page.region_page_count && page.base_address<=index &&
      uint64_t(page.base_address)+page.region_page_count<=count &&
      index<uint64_t(page.base_address)+page.region_page_count;
}
inline bool ValidateHeapBytes(Reader& r,const HeapLayout& expected, bool include_contents = true,
                              bool all_contents = false) {
  if(!expected.page_size||!ReadHeapLayout(r,expected))return false;
  for(uint32_t i=0;i<expected.page_count;++i){uint64_t wire;
    if(!r.U64(wire))return false;const PageEntry page=DecodePage(wire);
    if(!ValidPage(page,i,expected.page_count))return false;
    if(include_contents&&(all_contents||(page.state&kMemoryAllocationCommit))&&
       !r.Skip(expected.page_size))return false;
  }
  return true;
}
}
#endif
