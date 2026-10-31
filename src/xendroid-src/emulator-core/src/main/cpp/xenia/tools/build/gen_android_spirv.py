#!/usr/bin/env python3
"""Regenerate the Vulkan SPIR-V shader bytecode headers for the Android build.

Edge's in-tree shader pipeline drives a host-built tool (xenia-shader-cc) plus
slangc, neither of which runs inside the arm64 NDK cross-build. XenDroid instead
guards edge's shader rules off on Android (see gpu/vulkan and ui/vulkan
CMakeLists) and produces the SPIR-V bytecode headers with this host-side script,
which only needs Python + the system glslang/spirv tools (glslangValidator,
spirv-opt, spirv-dis on PATH or under $VULKAN_SDK).

For each stage shader source under the shader dirs it writes
  <shader_dir>/bytecode/vulkan_spirv/<id>.h
via compile_shader_spirv.py. Behaviour:
  * Prefers a hand-tuned .glsl/.xesl twin over the .slang form (matching xenia's
    xe_shader_rules_slang "defer to legacy twin" skip).
  * Skips sources tagged `// XE_DXIL_ONLY` (D3D12-only, no SPIR-V output).
  * Incremental: skips an output that is newer than its source.
  * Tolerant by default: a shader that fails to compile (e.g. a true-Slang
    source glslang can't parse) is warned about and skipped, so it never blocks
    the build of the shaders the Vulkan backend actually consumes. Pass --strict
    to fail on the first error.

Usage: gen_android_spirv.py [--strict] <shader_dir> [<shader_dir> ...]
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMPILE = os.path.join(HERE, "compile_shader_spirv.py")
STAGES = ("vs", "hs", "ds", "gs", "ps", "cs")
# Authoritative-source preference: a hand-tuned GLSL/xesl twin wins over .slang.
EXT_PREF = (".xesl", ".glsl", ".slang")


def direct_host_resolve_variants():
    """Enumerate the direct-host-resolve compute variants.

    Mirrors the upstream gpu/vulkan/CMakeLists foreach loops (which can't run
    here because they drive the host tool xenia-shader-cc). Each entry is
    (id, entry_source_basename, [define, ...]) where id ends in "_cs" and the
    defines select one bpp/MSAA/source-uint/scaled permutation from a shared
    .xesli body. 24 fast-color + 60 full-color + 6 depth = 90 variants, plus 8 + 20 2x2-source
    variants of the 4x color ones.
    """
    variants = []
    for source_uint in (0, 1):
        for bpp in (32, 64):
            for msaa in (1, 2, 4):
                for scaled in (0, 1):
                    ident = "resolve_host_color"
                    if source_uint:
                        ident += "_uint"
                    ident += f"_{bpp}bpp_{msaa}xmsaa"
                    defines = [
                        f"XE_RESOLVE_HOST_COLOR_BPP={bpp}",
                        f"XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES={msaa}",
                        f"XE_RESOLVE_HOST_COLOR_SOURCE_UINT={source_uint}",
                    ]
                    if scaled:
                        ident += "_scaled"
                        defines.append("XE_RESOLVE_RESOLUTION_SCALED=1")
                    ident += "_cs"
                    variants.append(
                        (ident, "resolve_host_color_entry.xesli", defines))
                    if msaa == 4:
                        # 4x kept as a 1x image of twice the size
                        # (vulkan_msaa_4x_as_2x2).
                        variants.append(
                            (ident[:-3] + "_2x2_cs", "resolve_host_color_entry.xesli",
                             defines + ["XE_RESOLVE_HOST_COLOR_SOURCE_2X2=1"]))
        for bpp in (8, 16, 32, 64, 128):
            for msaa in (1, 2, 4):
                for scaled in (0, 1):
                    ident = "resolve_host_color_full"
                    if source_uint:
                        ident += "_uint"
                    ident += f"_{bpp}bpp_{msaa}xmsaa"
                    defines = [
                        f"XE_RESOLVE_HOST_COLOR_FULL_DEST_BPP={bpp}",
                        f"XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES={msaa}",
                        f"XE_RESOLVE_HOST_COLOR_SOURCE_UINT={source_uint}",
                    ]
                    if scaled:
                        ident += "_scaled"
                        defines.append("XE_RESOLVE_RESOLUTION_SCALED=1")
                    ident += "_cs"
                    variants.append(
                        (ident, "resolve_host_color_full_entry.xesli", defines))
                    if msaa == 4:
                        # 4x kept as a 1x image of twice the size
                        # (vulkan_msaa_4x_as_2x2).
                        variants.append(
                            (ident[:-3] + "_2x2_cs", "resolve_host_color_full_entry.xesli",
                             defines + ["XE_RESOLVE_HOST_COLOR_SOURCE_2X2=1"]))
    for bpp in (32, 64):
        for ms in (0, 1):
            for tex in (0, 1):
                ident = f"resolve_host_color_inpass_{bpp}bpp"
                if ms:
                    ident += "_ms"
                if tex:
                    ident += "_tex"
                variants.append((ident + "_ps",
                                 "resolve_host_color_inpass_entry.xesli",
                                 [f"XE_RESOLVE_HOST_COLOR_BPP={bpp}",
                                  "XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES=1",
                                  "XE_RESOLVE_HOST_COLOR_SOURCE_UINT=0",
                                  "XE_RESOLVE_INPASS=1",
                                  f"XE_RESOLVE_INPASS_MSAA={ms}",
                                  f"XE_RESOLVE_INPASS_TEXTURE={tex}"]))
    for tex in (0, 1):
        ident = "resolve_host_color_inpass_32bpp_r32_uint"
        if tex:
            ident += "_tex"
        variants.append((ident + "_ps", "resolve_host_color_inpass_entry.xesli",
                         ["XE_RESOLVE_HOST_COLOR_BPP=32",
                          "XE_RESOLVE_HOST_COLOR_MSAA_SAMPLES=1",
                          "XE_RESOLVE_HOST_COLOR_SOURCE_UINT=0",
                          "XE_RESOLVE_INPASS=1",
                          "XE_RESOLVE_INPASS_MSAA=0",
                          f"XE_RESOLVE_INPASS_TEXTURE={tex}",
                          "XE_RESOLVE_INPASS_SOURCE_UINT=1"]))
    for msaa in (1, 2, 4):
        for scaled in (0, 1):
            ident = f"resolve_host_depth_32bpp_{msaa}xmsaa"
            defines = [f"XE_RESOLVE_HOST_DEPTH_MSAA_SAMPLES={msaa}"]
            if scaled:
                ident += "_scaled"
                defines.append("XE_RESOLVE_RESOLUTION_SCALED=1")
            ident += "_cs"
            variants.append((ident, "resolve_host_depth_entry.xesli", defines))
    return variants


def stage_of(name):
    base, ext = os.path.splitext(name)  # 'foo.cs', '.slang'
    if ext not in (".xesl", ".glsl", ".slang"):
        return None
    stage = os.path.splitext(base)[1].lstrip(".")  # 'cs'
    return stage if stage in STAGES else None


def collect(shader_dir):
    """Return {id: best_source_path} for stage shaders in shader_dir."""
    by_id = {}
    for name in sorted(os.listdir(shader_dir)):
        path = os.path.join(shader_dir, name)
        if not os.path.isfile(path) or stage_of(name) is None:
            continue
        ident = os.path.splitext(name)[0].replace(".", "_")  # 'foo_cs'
        ext = os.path.splitext(name)[1]
        cur = by_id.get(ident)
        if cur is None or EXT_PREF.index(ext) < EXT_PREF.index(os.path.splitext(cur)[1]):
            by_id[ident] = path
    return by_id


def is_dxil_only(path):
    try:
        with open(path, "r", errors="ignore") as f:
            for _ in range(40):
                line = f.readline()
                if not line:
                    break
                if line.lstrip().startswith("//") and "XE_DXIL_ONLY" in line:
                    return True
    except OSError:
        pass
    return False


def main():
    args = sys.argv[1:]
    strict = "--strict" in args
    dirs = [a for a in args if a != "--strict"]
    if not dirs:
        print(__doc__)
        return 2

    generated = skipped = failed = 0
    for shader_dir in dirs:
        shader_dir = os.path.abspath(shader_dir)
        out_dir = os.path.join(shader_dir, "bytecode", "vulkan_spirv")
        include_mtime = max([os.path.getmtime(__file__)] + [
            os.path.getmtime(os.path.join(shader_dir, name))
            for name in os.listdir(shader_dir) if name.endswith(".xesli")])
        for ident, src in sorted(collect(shader_dir).items()):
            if is_dxil_only(src):
                continue
            out = os.path.join(out_dir, ident + ".h")
            if os.path.exists(out) and os.path.getmtime(out) >= max(os.path.getmtime(src), include_mtime):
                skipped += 1
                continue
            r = subprocess.run([sys.executable, COMPILE, src, out],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               text=True)
            if r.returncode == 0:
                generated += 1
            else:
                failed += 1
                msg = (r.stderr or "").strip().splitlines()
                tail = msg[-1] if msg else "(no message)"
                print(f"WARN: skipped {os.path.basename(src)} -> {ident}.h: {tail}",
                      file=sys.stderr)
                if strict:
                    return 1

        # Direct-host-resolve variants: the 90 #define-driven permutations of
        # the resolve_host_* entry shaders. Only attempted for the shader dir
        # that actually contains those entry sources (gpu/shaders).
        variants = direct_host_resolve_variants()
        if all(os.path.isfile(os.path.join(shader_dir, v[1])) for v in variants):
            wrapper_dir = os.path.join(out_dir, "_dhr_wrappers")
            os.makedirs(wrapper_dir, exist_ok=True)
            # The variants share a handful of .xesli bodies via #include; rather
            # than track that graph, treat the newest .xesli in the dir as the
            # source timestamp so editing any body forces a rebuild.
            newest_src = include_mtime
            for name in os.listdir(shader_dir):
                if name.endswith(".xesli"):
                    newest_src = max(
                        newest_src,
                        os.path.getmtime(os.path.join(shader_dir, name)))
            for ident, entry, defines in variants:
                out = os.path.join(out_dir, ident + ".h")
                # The wrapper's basename minus ".<stage>" becomes the
                # array id, so name it "<base>.<stage>.xesl" where <base> =
                # ident without the trailing "_<stage>".
                if len(ident) > 3 and ident[-3] == "_" and ident[-2:] in STAGES:
                    base, stage = ident[:-3], ident[-2:]
                else:
                    base, stage = ident, "cs"
                wrapper = os.path.join(wrapper_dir, base + f".{stage}.xesl")
                # The entry (and its nested #includes) resolve against the
                # shader dir, passed below as an extra -I. Write idempotently so
                # the wrapper mtime stays stable across configures.
                wrapper_contents = f'#include "{entry}"\n'
                if (not os.path.exists(wrapper) or
                        open(wrapper).read() != wrapper_contents):
                    with open(wrapper, "w") as wf:
                        wf.write(wrapper_contents)
                if (os.path.exists(out) and
                        os.path.getmtime(out) >= newest_src):
                    skipped += 1
                    continue
                cmd = [sys.executable, COMPILE, wrapper, out]
                cmd += ["-D" + d for d in defines]
                cmd += ["-I" + shader_dir]
                r = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True)
                if r.returncode == 0:
                    generated += 1
                else:
                    failed += 1
                    msg = (r.stderr or "").strip().splitlines()
                    tail = msg[-1] if msg else "(no message)"
                    print(f"WARN: skipped direct-host-resolve {ident}.h: {tail}",
                          file=sys.stderr)
                    if strict:
                        return 1

    print(f"spirv bytecode: {generated} generated, {skipped} up-to-date, "
          f"{failed} skipped/failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
