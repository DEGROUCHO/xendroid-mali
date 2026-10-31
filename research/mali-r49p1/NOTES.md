# Mali r49p1 (POCO X7 Pro, libGLES_mali.so 52 MB) — reverse-engineering notes

Tools: Ghidra 12.1.2 headless (`support/analyzeHeadless`), project `proj/`, scripts `ghidra-scripts/DecompAt.java` (args "name=0xVADDR,..." outdir depth; works with -noanalysis, creates callees). Full analysis crashes ("Can't checkpoint with locked buffers"): always -noanalysis + targeted decompile.

## Vulkan entry table (in .data.rel.ro, RELR in-place pointers)
64-byte records: {name_ptr, name_len, ext_name_ptr, 0, kind=3, IMPL, trace_wrapper, 0x400000}. The IMPL that follows a name belongs to THAT name (the block before a name belongs to the previous one). Wrappers time the call with clock_gettime and report to MediaTek gpud (`gpudVk*`).
Impl vaddrs: vkInvalidateMappedMemoryRanges 0x9339c0, vkFlushMappedMemoryRanges 0x9338b0, vkMapMemory 0x933ad0, vkCmdPipelineBarrier 0x913920, vkCmdBeginRendering 0x90de00, vkImportSemaphoreFdKHR 0x96ed50. Ghidra address = vaddr + 0x100000.

## vkInvalidateMappedMemoryRanges (verified in decompiled code)
- Skipped entirely for memory whose flag (byte +0x20 bit 2) marks it coherent.
- Else per range (VK_WHOLE_SIZE handled) -> allocator vtable (+0x48) under a mutex -> FUN 0x1da30f0:
  either ioctl 0x4020800f (KBASE_IOCTL_MEM_SYNC) or a userspace loop **DC CIVAC** (clean + invalidate) over the range, start aligned down to the cache line (CTR_EL0 line size), then DSB.
- => On Mali "invalidate" also WRITES BACK any CPU-dirty line in the range. Never let the CPU write (even neighbouring bytes in the same 64-byte line) into host-mapped memory the GPU writes, or an invalidate will clobber GPU data with stale CPU bytes.
