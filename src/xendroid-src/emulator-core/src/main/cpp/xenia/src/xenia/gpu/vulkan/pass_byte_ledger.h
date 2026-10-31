// Logical, uncompressed traffic. This observer never edits rendering commands.
#ifndef XENIA_GPU_VULKAN_PASS_BYTE_LEDGER_H_
#define XENIA_GPU_VULKAN_PASS_BYTE_LEDGER_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace xe::gpu::vulkan {
class PassByteLedger {
 public:
  enum Kind { kLoad, kStore, kRead, kWrite, kClear, kDraw, kUnknown, kClose, kAttachment, kTag };
  enum Category { kPassLoad, kPassStore, kResolve, kOwnership, kUpload,
                  kCopy, kClearBytes, kCategories };
  struct Rect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    uint64_t pixels() const { return uint64_t(w) * h; }
  };
  struct Event {
    Kind kind = kUnknown;
    Category category = kCopy;
    uint64_t image = 0;
    uint32_t aspect = 0, mip = 0, layer = 0;
    Rect rect;
    uint32_t format = 0, bpp = 0, samples = 1;
    uint32_t image_w = 0, image_h = 0;
    uint64_t read = 0, write = 0;
    bool conservative = false;
    uint32_t pass = 0;
    uint32_t load_op = UINT32_MAX, store_op = UINT32_MAX, slot = 0;
    const char* reason = "other";
    uint64_t direct = 0, outer = 0, pusher = 0;
    uint64_t bytes(Rect r) const { return r.pixels() * bpp * samples; }
  };
  struct Result {
    std::array<uint64_t, kCategories> bytes{};
    uint64_t avoid_store = 0, fold_load = 0, reentry = 0, candidates = 0;
    uint64_t dead_end = 0, unknown = 0;
    std::vector<std::string> lines;
  };

  static Rect Intersection(Rect a, Rect b) {
    int64_t x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    int64_t right = std::min(int64_t(a.x) + a.w, int64_t(b.x) + b.w);
    int64_t bottom = std::min(int64_t(a.y) + a.h, int64_t(b.y) + b.h);
    return {int32_t(x), int32_t(y), uint32_t(std::max(int64_t(0), right - x)),
            uint32_t(std::max(int64_t(0), bottom - y))};
  }
  // Subtraction partitions a rectangle into disjoint pieces; all lookahead
  // totals are charged to the original load/store, once per pixel and aspect.
  static void Subtract(Rect a, Rect cut, std::vector<Rect>& out) {
    Rect i = Intersection(a, cut);
    if (!i.pixels()) { out.push_back(a); return; }
    auto add = [&](int64_t x, int64_t y, int64_t w, int64_t h) {
      if (w > 0 && h > 0) out.push_back({int32_t(x), int32_t(y), uint32_t(w), uint32_t(h)});
    };
    add(a.x, a.y, a.w, int64_t(i.y) - a.y);
    add(a.x, int64_t(i.y) + i.h, a.w, int64_t(a.y) + a.h - i.y - i.h);
    add(a.x, i.y, int64_t(i.x) - a.x, i.h);
    add(int64_t(i.x) + i.w, i.y, int64_t(a.x) + a.w - i.x - i.w, i.h);
  }
  static std::string RectText(Rect r) {
    return std::to_string(r.x) + ',' + std::to_string(r.y) + ',' +
           std::to_string(r.w) + ',' + std::to_string(r.h);
  }
  static bool Same(const Event& a, const Event& b) {
    if (b.kind == kUnknown && !b.image) return true;
    return a.image && a.image == b.image && (a.aspect & b.aspect) &&
           a.mip == b.mip && a.layer == b.layer;
  }
  void Add(Event e) {
    events.push_back(e);
  }
  uint32_t BeginPass() { return ++current_pass; }
  void Reset() { events.clear(); current_pass = 0; }

  Result Finish(uint64_t frame) const {
    Result out;
    std::vector<uint64_t> pass_bytes(current_pass + 1);
    std::vector<uint32_t> pass_work(current_pass + 1);
    std::vector<uint64_t> pass_load(current_pass + 1), pass_store(current_pass + 1);
    auto log = [&](const std::string& body) {
      out.lines.push_back("[PBYTES] f=" + std::to_string(frame) + " " + body);
    };
    for (size_t i = 0; i < events.size(); ++i) {
      const Event& e = events[i];
      out.bytes[e.category] += e.read + e.write;
      pass_bytes[e.pass] += e.read + e.write;
      if (e.kind == kDraw) pass_work[e.pass] |= 1;
      if (e.kind == kClear) pass_work[e.pass] |= 2;
      if (e.category == kOwnership && e.kind == kRead) pass_work[e.pass] |= 4;
      if (e.kind == kLoad) pass_load[e.pass] += e.read;
      if (e.kind == kStore) pass_store[e.pass] += e.write;
      std::ostringstream s;
      s << "e=" << i << " p=" << e.pass << " kind=" << int(e.kind)
        << " cat=" << int(e.category) << " image=" << e.image
        << " aspect=" << e.aspect << " mip=" << e.mip << " layer=" << e.layer
        << " fmt=" << e.format << " bpp=" << e.bpp << " samples=" << e.samples
        << " image_wh=" << e.image_w << 'x' << e.image_h
        << " rect=" << e.rect.x << ',' << e.rect.y << ',' << e.rect.w << ',' << e.rect.h
        << " read=" << e.read << " write=" << e.write
        << " conservative=" << e.conservative << " payload=" << e.bytes(e.rect)
        << " slot=" << e.slot << " load_op=" << e.load_op << " store_op=" << e.store_op;
      if (e.kind == kClose) {
        s << " close=" << e.reason << " rpb=" << std::hex << e.direct << '/'
          << e.outer << " pusher=" << e.pusher;
      }
      if (e.kind == kUnknown) ++out.unknown;
      log(s.str());
      if (e.kind != kStore && e.kind != kLoad) continue;
      std::vector<Rect> remaining{e.rect};
      uint64_t classified = 0;
      if (e.kind == kLoad) {
        // Existing attachment clears immediately after LOAD may fold only if
        // one clear covers the entire render area before any image use.
        for (size_t j = i + 1; j < events.size(); ++j) {
          const Event& n = events[j];
          if (!Same(e, n) || n.kind == kAttachment || n.kind == kClose) continue;
          if (n.pass != e.pass) break;
          if (n.kind == kClear && Intersection(e.rect, n.rect).pixels() == e.rect.pixels()) {
            classified = e.bytes(e.rect); remaining.clear();
            out.fold_load += classified; out.candidates += classified;
            log("edge=" + std::to_string(i) + " next=" + std::to_string(j) +
                " class=clear_after_load rect=" + RectText(e.rect) + " bytes=" + std::to_string(classified));
          }
          break;
        }
      }
      // LOADs walk backwards; STOREs walk forwards. Draws conservatively kill
      // provenance, but never prove a full overwrite (blending/tests may read).
      const int step = e.kind == kStore ? 1 : -1;
      for (int64_t j = int64_t(i) + step;
           j >= 0 && j < int64_t(events.size()) && !remaining.empty(); j += step) {
        const Event& n = events[size_t(j)];
        if (!Same(e, n) || n.kind == kClose || n.kind == kAttachment || n.kind == kTag) continue;
        if (e.kind == kLoad && (n.kind == kRead || n.kind == kLoad)) continue;
        std::vector<Rect> next;
        for (Rect r : remaining) {
          Rect hit = n.kind == kUnknown && !n.image ? r : Intersection(r, n.rect);
          if (!hit.pixels()) { next.push_back(r); continue; }
          uint64_t bytes = e.bytes(hit);
          const char* tag = "unknown";
          if (e.kind == kStore) {
            if (n.kind == kClear || n.kind == kWrite) {
              tag = "overwrite"; out.avoid_store += bytes; out.candidates += bytes;
            } else if (n.kind == kLoad) {
              tag = "load"; out.reentry += bytes; out.candidates += bytes;
            }
            else if (n.kind == kRead) tag = n.category == kResolve ? "resolve" : "read";
          } else {
            if (n.kind == kClear && hit.pixels() == e.rect.pixels()) {
              tag = "clear"; out.fold_load += bytes; out.candidates += bytes;
            } else if (n.kind == kStore) {
              // This is the LOAD transaction; the STORE transaction is
              // charged independently by its forward edge.
              bool clear_content = false;
              if (hit.pixels() == e.rect.pixels()) {
                for (int64_t p = j - 1; p >= 0; --p) {
                  const Event& previous = events[size_t(p)];
                  if (!Same(e, previous) || previous.kind == kStore || previous.kind == kLoad ||
                      previous.kind == kRead || previous.kind == kAttachment || previous.kind == kClose) continue;
                  clear_content = previous.kind == kClear &&
                      Intersection(e.rect, previous.rect).pixels() == e.rect.pixels();
                  break;
                }
              }
              if (clear_content) { tag = "clear_via_store"; out.fold_load += bytes; }
              else { tag = "store_same_frame"; out.reentry += bytes; }
              out.candidates += bytes;
            } else tag = "written";
          }
          log("edge=" + std::to_string(i) + " next=" + std::to_string(j) +
              " class=" + tag + " rect=" + RectText(hit) + " bytes=" + std::to_string(bytes));
          classified += bytes;
          Subtract(r, hit, next);
        }
        remaining.swap(next);
      }
      uint64_t unclassified = 0;
      for (Rect r : remaining) {
        unclassified += e.bytes(r);
        log("edge=" + std::to_string(i) + " class=" +
            (e.kind == kStore ? "end_of_frame" : "previous_frame_or_unknown") +
            " rect=" + RectText(r) + " bytes=" + std::to_string(e.bytes(r)));
      }
      if (e.kind == kStore) out.dead_end += unclassified;
      // Also covers zero-byte ops, and is checkable in captured logs offline.
      log("partition=" + std::to_string(i) + " bytes=" +
          std::to_string(classified + unclassified) + " expected=" +
          std::to_string(e.bytes(e.rect)));
    }
    uint64_t total = 0;
    std::ostringstream summary;
    summary << "summary MB(decimal)";
    const char* names[] = {"load", "store", "resolve", "ownership", "upload", "copy", "clear"};
    for (size_t c = 0; c < kCategories; ++c) {
      total += out.bytes[c]; summary << ' ' << names[c] << '=' << out.bytes[c] / 1e6
                                           << ' ' << names[c] << "_bytes=" << out.bytes[c];
    }
    summary << " total_bytes=" << total << " total=" << total / 1e6 << " avoid_store=" << out.avoid_store / 1e6
            << " fold_load=" << out.fold_load / 1e6 << " reentry=" << out.reentry / 1e6
            << " candidates_union=" << out.candidates / 1e6
            << " end_frame_store=" << out.dead_end / 1e6 << " unknown=" << out.unknown
            << " avoid_store_bytes=" << out.avoid_store << " fold_load_bytes=" << out.fold_load
            << " reentry_bytes=" << out.reentry << " candidates_union_bytes=" << out.candidates
            << " end_frame_store_bytes=" << out.dead_end;
    log(summary.str());
    // Observed work: mixed ownership/draw passes belong to transfer; a clear
    // pass has clears and no draws. Empty/undecoded passes remain unknown.
    std::array<uint64_t, 4> counts{}, loads{}, stores{};
    for (uint32_t p = 1; p <= current_pass; ++p) {
      uint32_t c = (pass_work[p] & 4) ? 1 : (pass_work[p] & 1) ? 0 :
                   (pass_work[p] & 2) ? 2 : 3;
      ++counts[c]; loads[c] += pass_load[p]; stores[c] += pass_store[p];
    }
    const char* classes[] = {"scene", "transfer", "clear", "unknown"};
    for (size_t c = 0; c < counts.size(); ++c)
      log("pass_class=" + std::string(classes[c]) + " passes=" + std::to_string(counts[c]) +
          " load_bytes=" + std::to_string(loads[c]) + " store_bytes=" + std::to_string(stores[c]));
    std::vector<uint32_t> ranks;
    for (uint32_t p = 1; p <= current_pass; ++p) ranks.push_back(p);
    std::stable_sort(ranks.begin(), ranks.end(), [&](uint32_t a, uint32_t b) {
      return pass_bytes[a] > pass_bytes[b];
    });
    for (size_t i = 0; i < std::min(size_t(10), ranks.size()); ++i)
      log("top=" + std::to_string(i + 1) + " p=" + std::to_string(ranks[i]) +
          " bytes=" + std::to_string(pass_bytes[ranks[i]]));
    return out;
  }
  uint32_t current_pass = 0;
  std::vector<Event> events;
};
}  // namespace xe::gpu::vulkan
#endif
