// Samples Mali GPU hardware counters (libGPUCounters) once per interval and
// prints one line per sample - runs from adb shell next to the emulator, no
// root. usage: xd_gpuctr [seconds=60] [interval_ms=1000]
#include <hwcpipe/counter_database.hpp>
#include <hwcpipe/gpu.hpp>
#include <hwcpipe/sampler.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <unistd.h>

struct Ctr {
  hwcpipe_counter id;
  const char* name;
};

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 60;
  int interval_ms = argc > 2 ? atoi(argv[2]) : 1000;
  auto gpu = hwcpipe::gpu(0);
  if (!gpu) {
    printf("no gpu\n");
    return 1;
  }
  const Ctr wanted[] = {
      {MaliGPUActiveCy, "gpu"},
      {MaliBinningQueueActiveCy, "binQ"},
      {MaliMainQueueActiveCy, "mainQ"},
      {MaliCompQueueActiveCy, "compQ"},
      {MaliTilerActiveCy, "tiler"},
      {MaliCompOrBinningActiveCy, "coreBin"},
      {MaliMainActiveCy, "coreMain"},
      {MaliCoreActiveCy, "core"},
      {MaliNonFragThread, "nfThr"},
      {MaliFragThread, "fThr"},
      {MaliTilerPosShadThread, "posThr"},
      {MaliTilerVarShadThread, "varThr"},
      {MaliTilerPosShadStallCy, "posStall"},
      {MaliTilerPrimAsPosShadStallCy, "primAsmStall"},
      {MaliGeomTotalPrim, "prim"},
      {MaliGeomVisiblePrim, "vis"},
      {MaliGeomTotalCullPrim, "cull"},
      {MaliGeomDVSPrim, "dvs"},
      {MaliFragRastPrim, "rastPrim"},
      {MaliFragTile, "tiles"},
      {MaliFragTileKill, "tileKill"},
      {MaliFragRastQd, "rastQd"},
      {MaliFragEZSTestQd, "ezsTest"},
      {MaliFragEZSKillQd, "ezsKill"},
      {MaliFragLZSTestQd, "lzsTest"},
      {MaliFragLZSKillQd, "lzsKill"},
      {MaliFragFPKKillQd, "fpkKill"},
      {MaliFragShadedQd, "shadedQd"},
      {MaliFragOpaqueQd, "opaqueQd"},
      {MaliFragTransparentQd, "transpQd"},
      {MaliFragPrepassKillQd, "preKill"},
      {MaliFragQueueJob, "fragJobs"},
      {MaliExtBusRdBy, "extRd"},
      {MaliExtBusWrBy, "extWr"},
      {MaliTilerRdBy, "tilerRd"},
      {MaliTilerWrBy, "tilerWr"},
      // Shader core breakdown: is fragment shading ALU-, texture-, LS- or
      // varying-bound, and how much FP16 ("narrow") work the shaders do.
      {MaliEngFMAInstr, "fmaI"},
      {MaliEngCVTInstr, "cvtI"},
      {MaliEngSFUInstr, "sfuI"},
      {MaliEngArithInstr, "arithI"},
      {MaliEngNarrowInstr, "narrowI"},
      {MaliEngStarveCy, "starve"},
      {MaliTexIssueCy, "texIss"},
      {MaliTexFiltIssueCy, "texFilt"},
      {MaliLSIssueCy, "lsIss"},
      {MaliVarIssueCy, "varIss"},
      {MaliALUIssueCy, "aluIss"},
      {MaliSCBusTexExtRdBy, "texExtRd"},
      {MaliSCBusLSExtRdBy, "lsExtRd"},
      {MaliSCBusLSWrBy, "lsWr"},
      // IDVS vertex reuse: position/varying cache hits vs misses (a miss is a
      // shader invocation), and stalls waiting on the vertex cache.
      {MaliTilerPosCacheHit, "posHit"},
      {MaliTilerPosCacheMiss, "posMiss"},
      {MaliTilerVarCacheHit, "varHit"},
      {MaliTilerVarCacheMiss, "varMiss"},
      {MaliTilerPosShadTask, "posTask"},
      {MaliTilerVertCacheStallCy, "vcStall"},
  };
  auto config = hwcpipe::sampler_config(gpu);
  std::vector<Ctr> used;
  for (const Ctr& c : wanted) {
    if (!config.add_counter(c.id)) {
      used.push_back(c);
    } else {
      printf("unsupported: %s\n", c.name);
    }
  }
  auto sampler = hwcpipe::sampler<>(config);
  if (auto ec = sampler.start_sampling()) {
    printf("start failed: %s\n", ec.message().c_str());
    return 1;
  }
  int samples = seconds * 1000 / interval_ms;
  for (int i = 0; i < samples; ++i) {
    usleep(interval_ms * 1000);
    if (auto ec = sampler.sample_now()) {
      printf("sample failed: %s\n", ec.message().c_str());
      continue;
    }
    printf("GPUCTR");
    for (const Ctr& c : used) {
      hwcpipe::counter_sample s;
      if (sampler.get_counter_value(c.id, s)) {
        printf(" %s=?", c.name);
      } else if (s.type == hwcpipe::counter_sample::type::uint64) {
        printf(" %s=%llu", c.name, (unsigned long long)s.value.uint64);
      } else {
        printf(" %s=%.0f", c.name, s.value.float64);
      }
    }
    printf("\n");
    fflush(stdout);
  }
  return 0;
}
