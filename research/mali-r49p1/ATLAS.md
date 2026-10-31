**ACTIONS**
1. Correct the inventory: it contains **121 functions**, all with a matching C.
2. Discard the costs attributed to foreign code folded in after `__stack_chk_fail`.
3. First proposal: reuse completed descriptor sets, eliminating resets and reallocations in the pool chain.
4. Second proposal: invalidate only the timestamps actually read, when profiling is active.
5. Keep the optimizations already present: descriptor cache, persistent mappings, pipeline and ImageView reuse.
6. Measure the passes, submissions and barriers actually emitted before changing their segmentation.
7. Do not freeze the renderArea to “fix” the SHA-1 cache: the C contradicts that explanation.
8. Do not salt the attachments at every BeginRendering: it increases misses, allocations and scans.
9. Treat erratum Arm 3786926 as a still-open correctness limit for this path.
10. No percentage of speedup or thermal reduction is demonstrated by this static analysis.


**FACT** means observed in the decompiled C or in the emulator source. **HYPOTHESIS** indicates a performance effect, an internal meaning not reconstructed, or a behavior still to be verified on the device. An operation visible in the decompiled code does not automatically prove that the branch is taken on the POCO during a game.

To make the references readable:

- `D` = (internal notes). In the tables, `t18:516` means `D/t18/<nome API>.c:516`.
- `B` = (internal notes).
- `E` = `src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/`.
- `CP`, `DCB`, `RTC`, `TC`, `PC`, `SM` = respectively `E/gpu/vulkan/vulkan_command_processor.cc`, `deferred_command_buffer.cc`, `vulkan_render_target_cache.cc`, `vulkan_texture_cache.cc`, `vulkan_pipeline_cache.cc`, `vulkan_shared_memory.cc`.
- `UI/<file>` = `E/ui/vulkan/<file>.cc`.

The frequencies `D/P/S/R` indicate respectively paths for draw/state, pass, submission and resource creation/recycling. They are classifications of the source, **not runtime counts**.

---

**Corrections**

| Entry | Claim to correct | Verified finding and consequence |
|---|---|---|
| `t09`, DestroyFramebuffer | After destruction a large pipeline rebuild would start. | **FACT:** the normal path returns at `t09/vkDestroyFramebuffer.c:533–534`; the creation code follows `FUN_030f78a0` at line 536. Do not attribute that rebuild to destruction. What remains is removal from the registries, destructors and free, lines 515–531. |
| `t18`, PipelineBarrier | The entry would run a “post-pass” on the barriers after the first helper. | **FACT:** the entry calls `FUN_00a107c0` and returns at lines 108–110. The following block follows the stack failure; the separate entry `vkCmdPipelineBarrier2` is present at `research/mali-r49p1/atlas-raw/vkCmdPipelineBarrier2.c:85–90`. |
| `t18`, PipelineBarrier | Allocation only beyond 15 barriers. | **FACT:** in the helper, in the branch without the object at `param_1+0x20`, the temporary is already allocated when `param_6 >= 2`; the processed block is limited to 16 elements. `t18/vkCmdPipelineBarrier.c:834–835,962–974`. The threshold stated there is wrong. |
| `t24`, FreeMemory | After the free it walks and frees a list of ranges. | **FACT:** normal return at `t24/vkFreeMemory.c:52–53`; from line 55 begins code folded in after the stack failure. The following loop belongs to the flush path and does not prove additional work by FreeMemory. |
| `t23`, DestroyBufferView | Possible synchronization ioctl cost in the destructor. | **FACT:** normal destruction ends at `t23/vkDestroyBufferView.c:58–59`; from line 61 begins foreign code. That syscall cannot be attributed to destruction based on this file. |
| `t20`, AllocateDescriptorSets | The following blocks would represent normal resets during allocation. | **FACT:** distinguish `FUN_00a2b370`, lines 567–629, from the code following the stack failure at line 631. The allocation branch searches for reusable slots and initializes the set; the reset described there is not demonstrated. |
| `t01`, ComputePipelines | SHA-256 hash. | **FACT:** it uses `FUN_02020820/850/b30`, for example `t01/vkCreateComputePipelines.c:5883–5893,6297`. The same helpers contain **SHA-1** instructions, visible at `B/br-lookup.c:1371–1416`. |
| `t00`, GraphicsPipelines | The initial allocation of the pipeline object would only be a cache miss. | **FACT:** the allocation is in the creation loop, before the compile/cache path: `t00/vkCreateGraphicsPipelines.c:543–558`. A shader cache hit does not eliminate the whole construction of the pipeline object. |
| `t22`, BindPipeline | Graphics and compute bind points inverted. | **FACT:** graphics is `0`, compute is `1`, in `vulkan_core.h:2388–2389`. The branch at `t22/vkCmdBindPipeline.c:33` is compute. |
| `t19`, ResetCommandPool | Bit 0 interpreted as `RESET_COMMAND_BUFFER_BIT`. | **FACT:** it is a flag passed to **ResetCommandPool**, therefore `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`. C: `t19/vkResetCommandPool.c:43–49`; enum: `vulkan_core.h:3118`. |
| `t03`, BeginRendering | The branch with bit 2 would be “suspended rendering”. | **FACT:** `flags & 4` is **RESUMING**; SUSPENDING is `2`. C: `t03/vkCmdBeginRendering.c:583–632`; enum: `vulkan_core.h:7365–7366`. |
| `t24`, PushConstants | Layout validation and saving of a value derived from the stages into `cmd+0x20`. | **FACT:** at `t24/vkCmdPushConstants.c:35–37` the helper receives offset, size and data; `+0x20` receives the **error code**. The allocation of `0x110` depends on state flags; it is not demonstrated as “one per layout”: lines 245–247. |
| `t16`, CreateImageView | Essentially a single allocation, with no cache maintenance on the useful path. | **FACT:** the helper builds descriptors and may obtain internal memory, write it and call `FUN_01e9b750`: `t16/vkCreateImageView.c:1056–1067,1096–1107`. The latter may take a mutex, lines 1553–1587. |
| `t18`, UpdateDescriptorSets | The linear binding scan and much of the shadow bookkeeping would always be active. | **FACT:** the scan at lines 214–222 is under the `FUN_030f4f50(2)` checks of lines 204–208. The actual write via `FUN_00a2cb50` at line 516 is outside that branch. Do not use the capture cost as an ordinary cost. |
| `t18`, UpdateDescriptorSets | Cache maintenance “only on the gaps”. | **FACT:** it accumulates intervals per compatible backing, flushes them when the backing changes, and also flushes the last interval at the end of the call: lines 518–541.897–929. It is not limited to gaps between copies. |
| `t19/21` | `0x3b9ce510` interpreted as an immutable sampler or descriptor buffer. | **FACT:** it is `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`, `vulkan_core.h:2375`. Type `8` is also not an input attachment: it is a dynamic uniform buffer. The earlier interpretations are not reliable. |
| `t20/24/25` | `0x3ba31aea` called timeline semaphore; `0x3ba1f5f6` called external/dedicated. | **FACT:** they are `VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS` and `VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO`, respectively `vulkan_core.h:439,436`. |
| `t20`, CreateShaderModule | Object sized `8*codeSize+0x68`. | **FACT:** the factor is `*(device+0x18)*8`, not codeSize: `t20/vkCreateShaderModule.c:40–41`. The code is copied separately by the helper, lines 199–206. |
| `t07` and others | `FUN_01d51010` called a Vulkan descriptor set allocator or GPU flush. | **FACT:** it checks the capacity of five internal streams/buffers and calls the grow helper when needed: `t07/vkCmdBlitImage.c:1940–2015`. The Vulkan types of those five streams are not demonstrated. |
| `t15/16` | `cmd+0xc` interpreted as “render pass active”, so draw skipped inside the pass. | **FACT:** it is also set when the renderArea has zero size: `t03/vkCmdBeginRendering.c:395–396`. The semantic name given there is wrong. |
| `t24`, GetFenceStatus | `clock_gettime` necessarily equals a syscall. | **FACT:** a library call and an fd polling wrapper are visible: `t24/vkGetFenceStatus.c:53–64,164,237`. The execution mode of `clock_gettime` is not demonstrated by the C. |
| Several cards | “No malloc/lock/ioctl in the file” implies a free API. | **FACT:** many allocations and backend operations go through callbacks and vtables. The absence of the libc symbol does not prove the absence of cost. |

**Methodological note:** do not automatically cut everything that follows a stack failure. Some blocks are also reached through explicit cleanup jumps, for example `vkCreateImageView.c:210 → 228` and `vkCreateShaderModule.c:90 → 107`. The control flow must be followed.

---

**Atlas of the functions used**

In the following tables the described cost is a **FACT local to the visible path**. When the backend is not resolved, this is stated explicitly.

The number `N` is the updated count of occurrences of `vkNome(` in the `.cc` files of `gpu/vulkan` and `ui/vulkan` only: it includes alternative and diagnostic paths; it excludes calls through wrappers, headers and VMA. **It is not a per-frame count.** This explains, for example, the 24 direct occurrences of DestroyShaderModule versus the 50 textual occurrences of the input.

For commands, the ordinary path builds CPU state or streams; **it is not equivalent to a submission to the kernel**. The internal deferred-list path may add allocations from arena/chunks. The list's internal pointer must not be confused with the emulator's `DeferredCommandBuffer`.

| API | Effective function, cost and slow conditions | C verified | Use in the emulator; N |
|---|---|---|---|
| `vkCmdBeginRendering` | Builds the attachment state; allocations through the internal allocator, array growth; SHA-1 and linear search in the cache. Miss: object of `0x18958` bytes. | `t03:217,406–443,542–549,927–1077`; details `B/br-lookup.c:169–319` | `DCB:450`, per pass; **1** |
| `vkCmdEndRendering` | Finalizes/emits the pass state; different paths for internal state and resume/suspend. It may require memory from the helpers; it is not a simple free operation. | `t04:25–78,820–839` | `DCB:454`, per pass; **1** |
| `vkCmdBeginRenderPass` | Wrapper and framebuffer/attachment setup; arrays reused or grown, loop over the attachments and construction of the state. | `t14:17–26,311–371,546–617` | `DCB:129`, Presenter; **4** |
| `vkCmdEndRenderPass` | Collection of the pass resources and finalization; heap temporary beyond 16 references on the observed path; dump adds copies. | `t15:134–339,361–434` | `DCB:379`, Presenter; **3** |
| `vkCmdDraw` | Prepares the draw and dirty state; reserves space in the streams. A particular branch repeats preparation/emission per instance. | `t16:41–69,191–261` | `DCB:363`, UI/Presenter; **3**, D |
| `vkCmdDrawIndexed` | Same backend as the draw, with indexed data; additional state cost and possible per-instance expansions. | `t15:44–70,200–324` | `DCB:373`, UI; **2**, D |
| `vkCmdDispatch` | Ignores zero sizes; prepares descriptors/state and streams, or records a deferred node. Stream growth on the slow path. | `t20:42–66,155–233,326–400` | `DCB:353`, frame generation; **5**, X |
| `vkCmdBindPipeline` | Handles diagnostic hooks, then updates state. The backend skips part of the work if the same pipeline is already current, with state exceptions. | `t22:25–158,240–262` | `DCB:202,213`; **5**, D |
| `vkCmdBindDescriptorSets` | Loop over the sets and handling of dynamic offsets; binding backend for each set. No general deduplication of content demonstrated. | `t23:36–88` | `DCB:148`; **4**, D |
| `vkCmdBindIndexBuffer` | Prepares address/range and type; the direct path may reserve space and emit state. | `t22:45–167` | `DCB:196`; **2**, D |
| `vkCmdBindVertexBuffers` | Copies/transforms arrays of handles and offsets; cost proportional to the number of bindings. The deferred list may copy more arrays. | `t21:61–96,141–173,201–258` | `DCB:235`; **2**, D |
| `vkCmdPushConstants` | Copies the values into the state/arena; may allocate an internal block of `0x110` when the state requires it. | `t24:35–54,84–87,245–247` | `DCB:498`; **8**, D/X |
| `vkCmdPipelineBarrier` | Translates stages/accesses, walks resources and records synchronization. Some paths allocate temporaries; particular layouts/ownership require additional work. | `t18:108,776–794,883–974,1388–1402` | `DCB:392,400,489`; `CP:10807`; **20**, P/X/S |
| `vkCmdCopyBuffer` | Loop over the regions; prepares a copy job per region and reserves stream/memory. Exhausted arena or streams add allocations. | `t17:27,95–131,187–254,436–526` | `DCB:295`; **3**, X |
| `vkCmdCopyBufferToImage` | Transforms regions and prepares the copy backend; beyond 16 regions uses a temporary heap. A gpud hook may add an artificial barrier. | `t18:24–31,104–150,386–387` | `DCB:310`; **2**, X |
| `vkCmdCopyImage` | Transforms regions and prepares the image copy; beyond 16 regions a temporary heap. Same diagnostic hook for an artificial barrier. | `t18:25–31,1324–1379,1569` | `DCB:321`; **1**, X |
| `vkCmdCopyImageToBuffer` | Transforms regions; stack up to 16, heap beyond; invokes the backend or records a list. | `t19:24–31,84–136` | `DCB:331`; **2**, X |
| `vkCmdBlitImage` | Transforms regions, handles formats/filters/layers; creates or reuses internal state. Heap beyond 16 regions; another temporary beyond 64 layers on the observed path. | `t07:52–114,574–588,1130–1178,1509–1518` | `DCB:342`; **1**, X |
| `vkCmdClearColorImage` | Clear outside a pass with lazy state and range/layer loops; or deferred recording. The first use may allocate state sized `0xe88`. | `t13:59–68,327–487,498–708` | `DCB:284`; **2**, X |
| `vkCmdClearAttachments` | Builds the state and clear work within the pass, with attachment/rectangle loops. Heap beyond 16 rectangles on the observed path. | `t17:147–277,355–414,499–508,751–817` | `DCB:278`; **3**, X |
| `vkCmdFillBuffer` | Computes the range, selects the fill path and prepares streams; reservations/arena on the slow path. | `t22:19–42,91–106,216–238` | `DCB:304`; **1**, X |
| `vkCmdBeginQuery` | Selects the backend by query type and prepares the commands; the deferred list may allocate a node. | `t20:25–53,898–928` | `DCB:240`; **1**, Q |
| `vkCmdEndQuery` | Selection by query type, construction of the end state; reserves space in the streams. | `t21:26–50,119–130` | `DCB:245`; **1**, Q |
| `vkCmdResetQueryPool` | Reset recorded on the GPU; prepares/emits streams, or records a node. Not equivalent to a host reset. | `t23:23–32,103–115,212–240` | `DCB:256`, `CP:10763`; **6**, Q |
| `vkCmdCopyQueryPoolResults` | Prepares the results copy with format/stride/flags; may include a GPU wait depending on the flags. | `t22:24–41,329–354,432–479` | `DCB:250`, `CP:10843`; **3**, Q |
| `vkCmdWriteTimestamp` | Translates the stage and records the timestamp; reserves a stream or deferred node. | `t21:20–61,106–117,232` | `DCB:262`, `CP:10765`; **7**, Q |
| `vkCmdSetViewport` | Loop over the viewports, transform/clamp and backend update; node on the deferred path. | `t23:50–92,148` | `DCB:565`; **3**, D |
| `vkCmdSetScissor` | Copies the rectangles and updates the state; node on the deferred path. | `t23:63–124,174` | `DCB:535`; **3**, D |
| `vkCmdSetFrontFace` | Maps the value and updates the state; the backend may reserve a stream. | `t23:28–43,120–156` | `DCB:579`; **1**, D |
| `vkCmdSetBlendConstants` | Copies four values and marks the state dirty; node on the deferred path. | `t24:18–44` | `DCB:523`; **1**, D |
| `vkCmdSetDepthBias` | Writes the three parameters and marks the state dirty; alternative to a node. | `t24:22–44` | `DCB:528`; **1**, D |
| `vkCmdSetCullMode` | Translates/writes the cull mode into the state; alternative to a node. | `t24:18–42` | `DCB:574`; **1**, D |
| `vkCmdSetPrimitiveTopology` | Lookup of the internal value and store; alternative to a node. | `t24:21–35` | `DCB:585`; **1**, D |
| `vkCmdSetPrimitiveRestartEnable` | Store of the boolean; alternative to a node. | `t25:18–34` | `DCB:590`; **1**, D |
| `vkCmdSetDepthTestEnable` | Store of the boolean and dirty bit; alternative to a node. | `t24:18–37` | `DCB:595`; **1**, D |
| `vkCmdSetDepthWriteEnable` | Store of the boolean and dirty bit; alternative to a node. | `t24:18–37` | `DCB:600`; **1**, D |
| `vkCmdSetDepthCompareOp` | Translates the compare op and marks the state dirty; alternative to a node. | `t24:22–49` | `DCB:605`; **1**, D |
| `vkCmdSetDepthClampEnableEXT` | Store and dirty bit; alternative to a node. | `t24:18–37` | `DCB:633`; **1**, D |
| `vkCmdSetStencilTestEnable` | Store and dirty bit; alternative to a node. | `t24:18–37` | `DCB:610`; **1**, D |
| `vkCmdSetStencilCompareMask` | Writes the mask for the selected faces, dirty bit; alternative to a node. | `t24:24–47` | `DCB:545`; **1**, D |
| `vkCmdSetStencilWriteMask` | As above, for the write mask. | `t24:24–47` | `DCB:559`; **1**, D |
| `vkCmdSetStencilReference` | As above, for the stencil reference. | `t24:24–47` | `DCB:552`; **1**, D |
| `vkCmdSetStencilOp` | Translates four operations, updates front/back and dirty bit; alternative to a node. | `t24:35–130` | `DCB:626`; **1**, D |
| `vkCmdSetFragmentShadingRateKHR` | Translates the parameters and updates the state; some branches prepare streams. Reducing the shading rate does not automatically satisfy the quality constraint. | `t24:25–46,166–171,178–288` | `DCB:620`; **1**, D |
| `vkCmdBeginDebugUtilsLabelEXT` | Diagnostic bookkeeping: lookup, strings, refcount/log, or deferred list. | `t21:27–118` | `DCB:687`; **1**, diagnostics |
| `vkCmdInsertDebugUtilsLabelEXT` | In the observed body it records a node only when the internal list is present and there is no error. | `t25:11–26` | `DCB:714`; **1**, diagnostics |

The following creation entries are normally setup costs or **emulator cache misses**, not unavoidable per-frame costs.

| API | Effective function, cost and slow conditions | C | Use; N |
|---|---|---|---|
| `vkCreateGraphicsPipelines` | Allocation/initialization per pipeline, state/pNext parsing, layout and shaders; cache lookup/hash; compilation on miss; publication of shader memory. | `t00:543–558,3782–3818,3884–3918`; shared compiler `t01:6300–6308,6568–6570` | `PC:3113`, `RTC:10216,10504`; **7**, R |
| `vkCreateComputePipelines` | Object construction, stage/layout parsing, SHA-1 cache; compilation if needed and cache maintenance. | `t01:221–241,735–786,5883–5893,6300–6308,6568–6570` | `CP:2007`, TC/Presenter; **10**, R |
| `vkCreateShaderModule` | Object plus copy/analysis of the SPIR-V code. Does not demonstrate a full compilation here. | `t20:40–41,82,199–227` | `vulkan_shader:114`, Presenter; **4**, R |
| `vkCreatePipelineCache` | Allocates object and tables, initializes mutex; validates/imports any initialData with a loop over the blobs. | `t21:32–85,121` | `PC:424,3426,3467`; **6**, I/R |
| `vkCreatePipelineLayout` | Allocates and initializes the layout, walks the set layouts and builds internal tables; more expensive cleanup on errors. | `t22:40–48,143–176,196–210` | `CP:1700,5698`, RTC; **24**, R |
| `vkCreateDescriptorSetLayout` | Several allocations and loops over the bindings; handling of extended structures and of the effective immutable samplers. | `t19:69–76,204–303,403–448` | `CP:838` and various setup; **22**, R |
| `vkCreateDescriptorPool` | Allocates the object and set slots, initializes counters and backend; cost grows with the requested capacity/types. | `t21:37–118,122–221` | `CP:1175`, UI allocators; **11**, R |
| `vkCreateCommandPool` | Allocates object and internal state; allocator/backend configuration. | `t19:28–62,189–191` | `CP:10408,10626`; **6**, R |
| `vkCreateBuffer` | Allocates a small CPU object, stores size/usage and walks pNext. The memory is separate. | `t24:30–74` | `CP:1332`, SM/upload; **15**, R |
| `vkCreateBufferView` | Allocates object and builds a format/range descriptor; handles WHOLE_SIZE. | `t24:27–48,141` | `CP:1452,1465`; **2**, R |
| `vkCreateImage` | Allocates the object, walks pNext and builds layout/metadata for the image and subresources; external backends differentiated. Full cost of the backends not reconstructed. | `t06:34–94,178,3451–3455,4691` | `TC:5585`, `vulkan_util:177`; **4**, R |
| `vkCreateImageView` | Object, swizzle/format, internal descriptors; may obtain backend memory and publish it under a mutex. | `t16:65–66,109–125,203,1056–1107,1553–1587` | `TC:4333,4471`, `RTC:7254`; **14**, R |
| `vkCreateRenderPass` | Converts descriptions, allocates the object and the attachment/subpass structures. The body has shared tails: it must not be interpreted by counting only the lines before the stack check. | `t02:496–513,962–998,1922–1950` | `RTC:1841,6869`; **4**, R |
| `vkCreateFramebuffer` | Object and array of views; attachment loop. The diagnostic dump may create additional resources. | `t14:68–169,180–275` | `RTC:1866,7723`; **4**, R |
| `vkCreateSampler` | Small allocation, conversion of filters/LOD/border/pNext and sampler descriptor. | `t21:78–80,103–129,157–268` | `TC:1422`, `ui_samplers:52`; **3**, R |
| `vkCreateQueryPool` | Object, results memory, CPU arrays and initialization proportional to the number of queries. | `t21:28–82,176–275` | `CP:2050`, ZPD/Presenter; **5**, R/Q |
| `vkCreateFence` | Object and initialization of the wait payload; internal mutex. Export/pNext select particular paths. | `t23:18–23,128–146,415–429` | `vulkan_gpu_completion_timeline:101`; **1**, R |
| `vkCreateSemaphore` | Object, export/timeline parsing and payload initialization; backend differentiated by type. | `t23:36–89` | `CP:10610`, Presenter; **5**, R |

For many destructors a common path appears: `FUN_00a323e0` takes a mutex and visits the list of registries; removal from the additional table is conditional. These are not necessarily three fixed locks per call, nor are those registries demonstrated to be “per-queue lists”.

| API | Observed work and slow conditions | C | Use; N |
|---|---|---|---|
| `vkDestroyBuffer` | Untracking, memory bookkeeping, possible hash removal and free via helper. | `t24:29–30,74–94,106–110` | CP/SM/TC/UI; **66**, R |
| `vkDestroyBufferView` | Untracking, possible hash removal and free. Foreign blocks after line 61 excluded. | `t23:47–59` | Loaded in `functions/device_1_0.inc:58`; **0** direct |
| `vkDestroyImage` | Untracking, virtual destructor, possible hash removal and free. | `t24:38–49` | CP/RTC/TC/UI; **23**, R |
| `vkDestroyImageView` | Untracking, descriptor/backend destructor, possible hash removal and free; mutexes/atomics in the backend release branches. | `t22:45–53,460–596,617–643` | `CP:9937`, `RTC:7068–7076`, TC; **27**, R |
| `vkDestroyFramebuffer` | Release of the arrays/children, untracking and free. No recompilation demonstrated. | `t09:515–534` | `CP:9921`, RTC; **6**, R |
| `vkDestroyRenderPass` | Loop over the per-subpass structures, release of arrays, untracking and free. | `t23:27–56` | RTC/Presenter; **3**, R |
| `vkDestroyPipeline` | Untracking, virtual destructor and possible hash removal/free. Destructor backend not fully expanded. | `t24:19–31` | PC/RTC/UI; **12**, R |
| `vkDestroyShaderModule` | Untracking, release of the code/module, possible hash removal and free. | `t24:26–39` | CP/PC/RTC/Presenter; **24**, R |
| `vkDestroyPipelineCache` | Untracking, cache destruction and free. The cost of the entries depends on the content. | `t24:18–31` | `PC:519,3404,3868`; **6**, I/R |
| `vkDestroyPipelineLayout` | Atomic decrement of the refcount; destructor only at the last reference. | `t25:20–35` | CP/RTC/TC; **6**, R |
| `vkDestroyDescriptorSetLayout` | Atomic decrement; destructor at the last reference. | `t25:20–35` | CP/TC and wrapper; **2**, R |
| `vkDestroyDescriptorPool` | Backend reset/release, loop over the slots, refcount and free of the arrays/object. | `t22:32–64,107–187,474–481` | TC/UI allocators; **12**, R |
| `vkDestroyCommandPool` | Loop over the command buffers and internal lists, release of memory and registries. | `t23:23–31,108–159` | CP/Presenter; **18**, R |
| `vkDestroySampler` | Untracking, possible hash removal and free. The creation folded in after the stack failure is excluded. | `t24:48–66` | TC/ui_samplers; **3**, R |
| `vkDestroyQueryPool` | Untracking and release of the query state/memory; free. | `t23:15–23` | ZPD and wrapper; **5**, R |
| `vkDestroyFence` | Atomic decrement; payload destructor at the last reference. | `t25:20–35` | Completion timeline; **3**, R |
| `vkDestroySemaphore` | Untracking, atomic decrement of the payload, teardown at the last reference and free. | `t24:33–39,275–292` | CP/Presenter; **6**, R |
| `vkDestroyDevice` | Global teardown, loops, locks, destruction of mutexes/semaphores and free. | `t22:18–31,56–81,151–156` | `vulkan_device:1762`; **1**, shutdown |

The remaining 39 functions complete the inventory.

| API | Effective function, cost and slow conditions | C | Use; N |
|---|---|---|---|
| `vkAllocateCommandBuffers` | Object per CB and internal state of `0x6ccc0` bytes; registration in the pool. Failure: rollback of the already created objects. | `t13:30–40,121–138,167–176,2017–2018` | `CP:10418,10638`; **6**, R |
| `vkBeginCommandBuffer` | Reinitializes the state; reuses or obtains internal backing. Inheritance adds setup; `SIMULTANEOUS_USE` selects additional mutex/list. | `t05:78–108,319–330,391–399` | `CP:10434,10753`; **7**, S |
| `vkEndCommandBuffer` | Stream finalization, reservations/growth and publication of the chunks. Locks and cache maintenance present in helpers, depending on attributes. | `t11:21–52,376–461,3732–3741,3782–3789` | `CP:10359,10910`; **8**, S |
| `vkResetCommandPool` | Loop over the CBs and backend reset. RELEASE_RESOURCES or an application workaround may free the cache structures. | `t19:17–49,52–78` | `CP:10427,10742`; **4**, S |
| `vkAllocateDescriptorSets` | Loop over the requested sets; search for reusable slots, layout compatibility, backend allocation and layout refcount. Extra shadow under gpud. | `t20:38–62,567–629,1039–1076` | UI allocators, CP; **21**, D/X/R |
| `vkUpdateDescriptorSets` | Loop over write/copy, descriptor encoding; no general elimination of identical writes is visible. Coalescing of ranges for cache clean; conditional diagnostic shadow. | `t18:204–222,516–546,897–929,1328–1346` | `CP:12835`, `DCB:188`, RTC/TC; **29**, D/X |
| `vkResetDescriptorPool` | Walks slots/capacity, releases references/layouts and backing; resets counters. It is not merely zeroing an index. | `t24:6,38–84,89–135` | `vulkan_descriptor_pool_chain:39`; **1**, recycling |
| `vkAllocateMemory` | Memory object, pNext parsing and selection of normal/imported/external backend. Full kernel cost not reconstructed across all vtables. | `t17:49–58` and branches of the file | CP/SM/TC/UI; **19**, R |
| `vkFreeMemory` | Untracking, virtual memory destructor, possible hash removal and free. No additional flush loop proven. | `t24:42–55` | CP/SM/TC/UI; **68**, R |
| `vkBindBufferMemory` | Reads backing and stores base+offset address; conditional hooks/statistics. It is not a copy of the buffer's data. | `t20:17–25,1114–1140` | CP/SM/upload; **12**, R |
| `vkBindBufferMemory2` | Loop over the bind infos; same work per element, plus pNext/status. | `t20:22–44` | Assigned to VMA in `vulkan_mem_alloc:72`; **0** direct |
| `vkBindImageMemory` | Binds memory and the image's layout/subresource; different external paths, hooks under conditions. | `t12:20–44,3438–3452` | TC/CP/util; **6**, R |
| `vkBindImageMemory2` | Loop and pNext parsing; delegates to an ordinary bind or external backend. | `t12:31–94` | VMA, `vulkan_mem_alloc:73`; **0** direct |
| `vkMapMemory` | Thunk with size query for WHOLE_SIZE and virtual map call; possible additional capture path of map/unmap/map. Actual backend not resolved. | `t25:13–51` | Persistent mappings CP/SM/upload; **25**, R/Q |
| `vkUnmapMemory` | Optional hook and virtual unmap. The backend cost cannot be deduced from the entry alone. | `t25:13–17` | Cleanup/errors and diagnostics; **23**, R/Q |
| `vkFlushMappedMemoryRanges` | Per range: skips coherent memory; computes the interval and invokes the backend under mutex if maintenance is required. May notify an internal worker. | `t24:24–48,65–88` | `SM:545`, `vulkan_util:75`; **2** direct |
| `vkInvalidateMappedMemoryRanges` | As above, backend slot `+0x48`; non-coherent: possibly an ioctl or `DC CIVAC` per line and DSB. | `t24:24–48,65–88`; `decomp/sync_invalidate_main.c:35–48,119–125,141` | `SM:2088,2124`, CP diagnostics; **11** |
| `vkGetBufferMemoryRequirements` | Field reads and size/alignment/type-bits arithmetic; no loop/allocator in the body. | `t25:9–20` | CP/SM/util; **14**, R |
| `vkGetBufferMemoryRequirements2` | Same data plus scan of output pNext. | `t25:12–36` | `SM:895`, VMA; **1** direct |
| `vkGetDeviceBufferMemoryRequirements` | Computes from the create info, with pNext scan for usage2; no buffer creation visible. | `t25:14–38` | VMA, `vulkan_mem_alloc:85–86`; **0** direct |
| `vkGetImageMemoryRequirements` | Computation on already built image metadata. | `t25:12–56` | TC/CP/util; **4**, R |
| `vkGetImageMemoryRequirements2` | Metadata query plus output pNext. | `t24:12–134` | VMA, `vulkan_mem_alloc:64–65`; **0** direct |
| `vkGetDeviceImageMemoryRequirements` | Rebuilds layout/formats from the create info; mip/layer loops and possible scratch allocation. Much more work than querying an existing image. | `t10:100–200,395–782,968–989` | VMA, `vulkan_mem_alloc:87–88`; **0** direct |
| `vkGetAndroidHardwareBufferPropertiesANDROID` | Queries/imports AHB metadata and converts format/usage, traversing pNext. Costs of the Android helper not fully reconstructed. | `t24:45–72,173–200` | `SM:831`; **1** direct, R |
| `vkGetDeviceProcAddr` | Binary search in the entry table and version/extension checks. | `t24:17–45` | Device/setup and specific lookups; **10** |
| `vkGetDeviceQueue` | Indexed read of the queue handle and init hook. | `t25:10–14` | `vulkan_device:1686`; **1**, I |
| `vkQueueSubmit` | Submit/pNext parsing, wait/signal/CB loops, temporaries; commit of the shared backend. May queue internal work: absence of an ioctl in the entry does not mean a submission without kernel. | `t08:210–268,336–385,487,541–565,752` | Wrapper `vulkan_device.h:467`, Presenter; **2** direct |
| `vkQueueBindSparse` | The entry provided only contains the device-loss check; no actual sparse implementation is visible. | `t25:10–28` | `CP:10720`, sparse path; **1** |
| `vkQueueWaitIdle` | Waits for previous work, refcount and timed waits; device-loss check. | `t24:13,249–277` | `vulkan_presenter:1551`, recreation/teardown; **1** |
| `vkGetFenceStatus` | Without an fd it reads a flag; with an fd it takes time and calls the polling wrapper. | `t24:29–30,53–64,164` | `vulkan_gpu_completion_timeline:180`; **1**, S/poll |
| `vkWaitForFences` | Arrays and refcount per fence; stack up to 32, heap from 33. Waits in slices; additional diagnostic ioctls after prolonged stalls. | `t19:168–206,286–327,330–343,437–468` | Completion timeline `:29,228`; **2**, wait |
| `vkResetFences` | Loop: resets fields/payload; closes the fd if present. | `t25:17–27,38–52` | Completion timeline `:86`; **1**, S |
| `vkGetSemaphoreCounterValue` | On the healthy path it reads the counter from memory through a helper consisting of one dereference; then device-loss check. | `t25:11–31,37–40` | Presenter `:3441,4013`; **2**, frame generation |
| `vkWaitSemaphores` | Array of handles; beyond 32 an external heap. The observed WAIT_ANY branch allocates two more arrays even with few semaphores; wait backend. | `t23:100–134,164–169,259–286,338–375` | Presenter `:197,3858`; **2** |
| `vkGetQueryPoolResults` | Optional wait per query when WAIT is requested; copies intermediate results and conversion/availability per query. | `t24:32,263–298` | `CP:9691`, Presenter `:4118`; **2**, Q |
| `vkResetQueryPool` | Host reset via memset of the results range. It is not the GPU command of the same name with the Cmd prefix. | `t25:6–8` | ZPD `:172,466`; **2**, Q |
| `vkGetPipelineCacheData` | Size query under mutex; data extraction with temporary array, iteration, sort and copies. | `t22:66–72,112–145,160–166` | `PC:3892,3896`; **2**, save |
| `vkMergePipelineCaches` | Loop over caches/entries, conditional lock, allocation and copy per entry; duplicates may be allocated and then discarded. | `t23:34–80` | `PC:3482`; **1**, startup |
| `vkSetDebugUtilsObjectNameEXT` | Mutex, hash lookup/removal, strlen, name/node allocation, insertion; an empty name deletes. | `t22:44–86` | Wrapper `vulkan_device.h:369–380`; **0** in the `.cc` files |

---

**In-depth look at the mechanisms that matter**

The BeginRendering cache is an **attachment-state** cache, not a cache that makes the start of the pass free.

**FACT:** on the ordinary path the hash covers the `0x48` bytes of the attachments, therefore also clearValue, padding and the value of the pNext pointer. It also covers viewMask and optional data; it does not directly cover renderArea, layerCount and flags. The lookup is a list scan; on a miss the block of `0x18958 = 100696` bytes is allocated. The periodic collection considers references and last use; it is not a guaranteed expiry of entries after one second. References: `B/br-lookup.c:169–228,243–319`.

**FACT:** the current renderArea and layerCount reach the setup even on a hit: `B/br-main.c:181–188,412–438,596–628`. So "the cache keeps the first rectangle and ignores the subsequent ones" is not an explanation supported by the C code.

**FACT:** the emulator's main path already fully zeroes the attachments, including the padding: `RTC:6664–6665,6694–6696`. Adding another canonicalization to that path would duplicate work already done.

**HYPOTHESIS to measure:** many variants of attachment/clearValue/load/store can increase list length, misses and internal memory. The actual population of the cache during games has not been measured.

For descriptors, the most substantial reduction is to avoid **the entire operation**, not just to merge calls.

**FACT:** UpdateDescriptorSets re-encodes the writes and can clear memory ranges of the descriptors; no general check of the form "new value equal to the previous one → return" is visible. Within the same call the driver merges ranges with compatible backing: `t18/vkUpdateDescriptorSets.c:516–541,897–929`.

**FACT:** the emulator already has caches of values and bindings, dynamic constants and deferred updates: `CP:12333–12337,12722–12744,12767–12815,12831–12842`. The replay updates in batches of 16 writes: `DCB:154–190`.

**Consequence:** moving UpdateDescriptorSets to the worker changes the thread that pays the cost; on its own it does not demonstrate a reduction in total work. A cache that avoids allocation/write/bind can instead eliminate it.

For non-coherent memory:

**FACT:** flush and invalidate skip the range work when the coherence flag is present. In the non-coherent branch the invalidate can perform **clean + invalidate**, via `DC CIVAC`, or require maintenance through an ioctl. The line size is read from `CTR_EL0`; it must not be fixed arbitrarily at 64 bytes.

**Correctness consequence:** an invalidate does not replace waiting for the GPU producer. Moreover, unpublished CPU writes in the same range/atom can make the content undefined; the clean can propagate dirty CPU data. Both the local C code and the [vkInvalidateMappedMemoryRanges specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html) are consistent with this.

**FACT:** the ordinary readback code already excludes coherent memory and narrows/aligns the ranges: `SM:2077–2088,2108–2124`; the common helpers do the same in `UI/vulkan_util:44–59`. There is no basis for a global replacement with WHOLE_SIZE or for deleting the necessary invalidates.

---

**Optimizations ordered by expected gain versus risk**

The two new changes are kept separate from the mechanisms already implemented. The timing gains remain **HYPOTHESES**; the eliminated operations, by contrast, can be quantified.

| Priority | Action and basis | Expected gain per frame | Risk and measurement |
|---|---|---|---|
| **1 — new proposal A** | Reuse the descriptor set handles in the `VulkanDescriptorPoolChain`, after completion, instead of resetting the pool and reallocating them. Driver: `t24/vkResetDescriptorPool.c:89–135`; `t20/vkAllocateDescriptorSets.c:567–629`. Emulator: `UI/vulkan_descriptor_pool_chain:31–40,106–150`. | **CPU:** eliminates one ResetDescriptorPool per recycled pool and one AllocateDescriptorSets per already-materialized set. No GPU/bandwidth saving directly demonstrated. It applies to the paths that use this chain, without title-based heuristics. | Low/medium: set lifetime, rewritten content, backend memory retained. Measure calls/frame, CPU ns/frame, memory after warmup and repeated scenes. Diff A below. |
| **2 — new proposal B, diagnostics** | Limit the timestamp invalidates to the pairs that are read. Driver: cache-line loop in `decomp/sync_invalidate_main.c:119–125`. Emulator: `CP:9722–9729,9756–9763`. | **CPU/cache:** the ring payloads are 24 KiB resolve and 48 KiB pass; a submission occupies at most 768/1536 bytes before alignment. Avoids walking the entire ring on every record. **No ordinary gain if diagnostics are off or the memory is coherent.** | Low: atom alignment and buffer limits. Measure invalidated bytes, collection time and identity of the diagnostic results. Diff B below. |
| **3 — already implemented, to validate** | Keep the descriptor cache by value and the reuse of dynamic constants. The driver re-encodes the writes anyway; source `CP:12722–12744,12767–12842`. | Potentially significant CPU where many draws reuse the same bindings. The incremental gain from the atlas is **zero** until a concrete gap in the existing cache emerges. | Verify hit rate, allocated sets and effective descriptor writes per draw. Do not add another overlapping cache. |
| **4 — already implemented, potentially greater bandwidth gain** | Keep the renderArea shrink and the relaxation of operations on unused attachments: `DCB:829–873,878–921`; `CP:5440–5460`. The driver receives the updated rectangle even on a hit. | **HYPOTHESIS:** less attachment/tile work and less traffic; potentially greater than the micro-optimizations on the entries. The C code does not quantify the physical traffic saved. | Correctness of clear, depth/stencil, resolve and draw coverage. Measure GPU ms and bandwidth with separate A/B tests. Do not state that the percentage of area saved equals the percentage of bandwidth. |
| **5 — investigation, no justified diff** | Count avoidable pass and submission breaks. `CP:5144–5169` already avoids reopening the same framebuffer; `DCB:392–403` adds two barriers around the uploads at the pass head. | **HYPOTHESIS:** a removable boundary can save CPU setup and GPU attachment work. The number of removable boundaries is unknown. | High if guest dependencies are deleted. The cause of the break, producer/consumer and the content of the pass head are needed. No barrier removal is proposed here. |
| **6 — already implemented** | Keep command pools/CBs reused, reset with flags `0` and ONE_TIME_SUBMIT recording: `CP:10427–10434,10742–10753`. | Avoids object churn and release/reconstruction of the internal backing. No new gain demonstrated. | Measure high-water mark, pools created and stable memory. Do not read the reset flag as the creation flag. |
| **7 — already implemented** | Keep the dirty/feedback handling of the pipeline cache and the separate saving: `PC:3085–3126,3453–3492,3835–3868`. | CPU and memory during compilation/saving; not a saving for stable draws. The driver performs sorts/copies during extraction. | Measure save times and bytes, valid hits and misses. Do not re-save the cache merely because CreateGraphicsPipelines succeeded. |
| **8 — already implemented** | Reuse the emulator's ImageViews and pipelines: `TC:4296–4341`; `RTC:10038,10231`; `PC:862–877`. | Avoids descriptor construction, allocations, cleanup and compilation on repeated paths. | Record create/destroy per frame after warmup. Do not create an additional global cache without real misses. |

The first proposal concerns two observed users:

- segmented shared memory: `TC:858–892`, pool of 128 sets, `TC:5142–5152`;
- resolve downscale: `CP:8904–8962`, pool of 64 sets, `CP:2016–2025`.

**FACT:** both rewrite the bindings before use. Reuse must keep the same completion barrier already present in the chain. A Vulkan reset implicitly frees the sets: the handles cannot be kept while continuing to call ResetDescriptorPool. This is also the behavior of the [descriptor pool reset specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetDescriptorPool.html).

**Diff A — eliminate reset and reallocation of recycled sets**

```diff
--- a/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/ui/vulkan/vulkan_descriptor_pool_chain.h
+++ b/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/ui/vulkan/vulkan_descriptor_pool_chain.h
@@ -53,6 +53,7 @@
     VkDescriptorPool pool;
     uint64_t last_submission_index;
     uint32_t allocated_sets;
+    std::vector<VkDescriptorSet> descriptor_sets;
     Pool* next;
   };
 
--- a/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/ui/vulkan/vulkan_descriptor_pool_chain.cc
+++ b/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/ui/vulkan/vulkan_descriptor_pool_chain.cc
@@ -34,9 +34,7 @@
     if (submitted_first_->last_submission_index > completed_submission_index) {
       break;
     }
-    // Reset the pool so it can be reused.
-    const VulkanDevice::Functions& dfn = device_->functions();
-    dfn.vkResetDescriptorPool(device_->device(), submitted_first_->pool, 0);
+    // Reuse the existing sets once all their GPU uses have completed.
     submitted_first_->allocated_sets = 0;
 
     // Move to writable list.
@@ -103,6 +101,14 @@
     writable_last_ = writable_first_;
   }
 
+  if (writable_first_->allocated_sets <
+      writable_first_->descriptor_sets.size()) {
+    VkDescriptorSet descriptor_set = writable_first_->descriptor_sets
+        [writable_first_->allocated_sets++];
+    writable_first_->last_submission_index = submission_index;
+    return descriptor_set;
+  }
+
   // Allocate from current pool.
   VkDescriptorSetAllocateInfo alloc_info = {};
   alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
@@ -146,6 +152,7 @@
     }
   }
 
+  writable_first_->descriptor_sets.push_back(descriptor_set);
   writable_first_->allocated_sets++;
   writable_first_->last_submission_index = submission_index;
   return descriptor_set;
```

The added cost on the emulator side is the vector of handles; the backend keeps the sets until the pool is destroyed. **HYPOTHESIS:** the steady-state memory remains acceptable and the CPU work saved exceeds this cost. This must be verified, especially after pool growth and scene changes.

**Diff B — narrow the timestamp invalidates**

```diff
--- a/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc
+++ b/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc
@@ -9723,8 +9723,18 @@
           VkMappedMemoryRange resolve_invalidate_range = {
               VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
           resolve_invalidate_range.memory = resolve_timestamp_buffer_memory_;
-          resolve_invalidate_range.offset = 0;
-          resolve_invalidate_range.size = VK_WHOLE_SIZE;
+          const VkDeviceSize atom =
+              GetVulkanDevice()->properties().nonCoherentAtomSize;
+          const VkDeviceSize first_byte =
+              VkDeviceSize(record.resolve_slot_base) * 2 * sizeof(uint64_t);
+          const VkDeviceSize end_byte =
+              first_byte + VkDeviceSize(record.resolve_pair_count) *
+                               2 * sizeof(uint64_t);
+          resolve_invalidate_range.offset = first_byte / atom * atom;
+          resolve_invalidate_range.size =
+              std::min(xe::round_up(end_byte, atom),
+                       resolve_timestamp_buffer_size_) -
+              resolve_invalidate_range.offset;
           GetVulkanDevice()->functions().vkInvalidateMappedMemoryRanges(
               GetVulkanDevice()->device(), 1, &resolve_invalidate_range);
           for (uint32_t i = 0; i < record.resolve_pair_count; ++i) {
@@ -9757,8 +9767,18 @@
           VkMappedMemoryRange pass_invalidate_range = {
               VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
           pass_invalidate_range.memory = pass_timestamp_buffer_memory_;
-          pass_invalidate_range.offset = 0;
-          pass_invalidate_range.size = VK_WHOLE_SIZE;
+          const VkDeviceSize atom =
+              GetVulkanDevice()->properties().nonCoherentAtomSize;
+          const VkDeviceSize first_byte =
+              VkDeviceSize(record.pass_slot_base) * 2 * sizeof(uint64_t);
+          const VkDeviceSize end_byte =
+              first_byte + VkDeviceSize(record.pass_pair_count) *
+                               2 * sizeof(uint64_t);
+          pass_invalidate_range.offset = first_byte / atom * atom;
+          pass_invalidate_range.size =
+              std::min(xe::round_up(end_byte, atom),
+                       pass_timestamp_buffer_size_) -
+              pass_invalidate_range.offset;
           GetVulkanDevice()->functions().vkInvalidateMappedMemoryRanges(
               GetVulkanDevice()->device(), 1, &pass_invalidate_range);
           const bool log_pass_sequence =
```

The rings are split per submission: `CP.h:932–952`; the query ranges are reset for that portion in `CP:10773–10782`. Consumption happens only for records whose submission has completed: `CP:9660–9664`. Build, range limits and identity of the collected data remain to be verified.

---

**Operations that are useless, counterproductive, or not justified by the atlas**

| Operation | Assessment |
|---|---|
| Adding a new cache only to avoid rebinding the same pipeline | The driver already eliminates part of the identical rebinding, and the emulator already tracks state. It must be shown that the residual calls are numerous enough to justify additional state. |
| Rewriting identical descriptors while relying on driver deduplication | General deduplication is not visible. The emulator's cache is useful precisely because it avoids this work. |
| ResetDescriptorPool followed by rebuilding the same sets | This work is actually present in the chain and in the driver. It is the target of diff A. |
| WHOLE_SIZE on the timestamp rings for every completed record | Present in `CP:9727,9761`; it widens maintenance and reads relative to the data consumed. It is the target of diff B. |
| Flush/invalidate on memory that is certainly coherent | The driver already skips the range; the emulator's common helpers already avoid the call. The only residual marginal gain is to be sought in direct callers. |
| Map/unmap per frame as a universal optimization | The source already uses persistent mappings on the important paths. The full cost of the backend map has not been reconstructed; no new churn is justified. |
| Increasing descriptor batching indiscriminately | It can reduce calls and repeated cleans, but does not eliminate the encoding of the writes. Moreover, the main batches are already present. |
| Freezing renderArea to stabilize the hash | It does not change the observed digest; it can increase attachment work. It is not a demonstrated fix. |
| Salting clearValue on every BeginRendering | It changes the hash and can turn hits into misses, costing about 98 KiB plus a scan. It is not a performance proposal. |
| Using QueueSubmit2 or PipelineBarrier2 because they are "more modern" | No advantage demonstrated. The available function bodies keep parsing, translation and backend code shared. |
| Replacing fence polling with more frequent spinning | With fd, GetFenceStatus goes through a polling wrapper. More polling can increase CPU use without speeding up the GPU. |
| Reducing quality, skipping draws or reducing the shading rate to lower temperatures | Does not meet the goal. The comparison must keep the same draws and the same visual result. |

**FACT:** GetSemaphoreCounterValue has a much simpler healthy path than GetFenceStatus, but this alone does not justify migrating the completion timeline. The migration would involve submissions, asynchronous bookkeeping, errors and lifetimes. No diff to introduce it is proposed without a measurement of the current polling. The [counter specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetSemaphoreCounterValue.html) in any case requires a valid timeline semaphore; the value may already have been surpassed when it is observed.

---

**Correctness limit: dynamic rendering and ImageViews**

**DOCUMENTED FACT:** Arm includes r49p1 in erratum **3786926**. The documented workarounds are to use static render passes, or not to destroy the ImageViews referenced by RenderingInfo and its children. In the same document, 3734951 and 3785718 are listed as fixed in r49p1; the erratum on dynamic cull mode for non-triangle primitives, 3786517, instead includes this release. [Arm SDEN-3735689, pages 15, 21, 23 and 25](https://documentation-service.arm.com/static/6867b07546dc1e49e34b800d).

**FACT IN THE SOURCE:** there are actual destructions of render-target ImageViews: `RTC:7068–7076`; there is also deferred destruction on completion in `CP:9932–9938`.

**Consequence:** waiting for GPU completion satisfies the normal Vulkan lifetime, but is **not equivalent** to the Arm workaround of keeping those views. The earlier advice "keep dynamic rendering" must be accompanied by this limit.

**HYPOTHESIS:** reuse of handle addresses can make stale metadata resurface in the observed cache. The C code is compatible with this possibility, but does not prove the cause of a specific artifact in the emulator.

Unlimited retention is not proposed: it also keeps associated resources and can worsen memory and temperature. The static fallback is already selectable via `vulkan_dynamic_rendering`; it must be compared while keeping the other settings, also verifying MSRTSS/local read and performance. A universally faster and correct configuration is not certified here.

---

**Gaps and necessary work**

| Gap | What is missing and why it matters |
|---|---|
| Actual frequencies | Per-frame counts are needed for draws, passes, submissions, descriptor alloc/write/reset, ImageView create/destroy, pipeline create and polling. Occurrences in the source do not replace them. |
| Actual costs of the branches | Feature bits, allocation attributes, active gpud hooks, hit/miss and cache sizes during games are not known. |
| Boundaries and Ghidra prototypes | Several files include adjacent functions or shared tails; parameters and return types are sometimes wrong. Delimited entries and the stack-fail marked as non-returning are needed, followed by a recheck of the CFG. |
| Callbacks and vtables | For Map/Unmap, AllocateMemory and part of the resources, the actual targets and the full backend cost are missing. |
| Kernel submission | QueueSubmit reaches the backend/worker, but this extraction does not determine how many ioctls, wakeups and commits occur per frame. |
| GPU/bandwidth quantification | The streams are not enough to determine DRAM traffic, tile store/load or hardware cache misses. GPU and bandwidth measurements are needed. |
| Binary identity | The report assumes the driver indicated by the input; it does not certify here the hash/build-id of the `.so` or the identity of the binary loaded on the phone. |
| Dynamic rendering cache | Population, actual lifetime, misses and a controlled reproduction of any handle reuse are missing. |
| `vkFreeDescriptorSets` | It is not among the 121 nor among the direct calls examined. The raw C exists: wrapper and loop in `atlas-raw/vkFreeDescriptorSets.c:16–26`; helper with accounting/refcount in lines 76–111. It is not a demonstrated hot candidate. |
| `vkQueueSubmit2` | Raw available; wait/stage parsing and backend visible, for example `:208–234`. No direct call found in the path examined. |
| `vkCmdPipelineBarrier2` | Raw available and distinct from the block wrongly attributed to Barrier1. It is not the currently identified hot path. |
| Hot commands outside the inventory | `vkCmdSetPolygonModeEXT`, `vkCmdSetColorBlendEnableEXT`, `vkCmdSetColorBlendEquationEXT`, `vkCmdSetColorWriteMaskEXT`, `vkCmdSetRenderingInputAttachmentIndices`, `vkCmdEndDebugUtilsLabelEXT`: called in `DCB:517,638–666,697`, but outside the 121 cards. |
| Presentation outside the inventory | AcquireNextImage and QueuePresent, `vulkan_presenter:1709,2514`, can dominate waiting/pacing and are not described by the supplied atlas. |
| Coverage of all games | No static proof certifies the absence of regressions across the whole catalog. At least different paths are needed: host RT/accuracy, MSAA, depth/stencil, texture upload, resolve, memexport and scenes with heavy churn. |

To validate the proposals, two separate comparisons are initially sufficient, without simultaneously changing the scheduler, quality or segmentation:

1. **Diff A:** after warmup, AllocateDescriptorSets and ResetDescriptorPool must decrease in the two users of the chain; images and draws must remain equivalent, memory stable.
2. **Diff B:** with profiling active, the invalidated bytes must decrease; timestamps and aggregates must remain equivalent. With profiling off, no gain may be attributed to this diff.

For both: compare CPU ns/frame, GPU ms/frame, frame-time distribution and power use on a repeatable sequence. Temperature requires a sustained test at equal work: **no thermal reduction has yet been verified**.
