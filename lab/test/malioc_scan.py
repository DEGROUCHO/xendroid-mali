#!/usr/bin/env python3
# usage: malioc_scan.py DUMPDIR > table.tsv
# Runs Arm Mali Offline Compiler (Mali-G720, Vulkan) on every translated shader dumped with
# GPU.dump_shaders (shader_<ucode>_<mod>.vulkan.bin.{frag,vert}) and prints per-variant
# cycles: arith total/fma/cvt/sfu, load/store, varying, texture, the bound unit, work registers.
import json, os, subprocess, sys, tempfile, shutil, concurrent.futures as cf

MALIOC = "/path/to/xendroid-ultimate-mali/third_party/arm-ps-2026.5/mali_offline_compiler/malioc"
PIPES = ["arith_total", "arith_fma", "arith_cvt", "arith_sfu", "load_store", "varying", "texture"]

def scan(path, tmp):
    stage = "fragment" if path.endswith(".frag") else "vertex"
    spv = os.path.join(tmp, os.path.basename(path) + (".frag.spv" if stage == "fragment" else ".vert.spv"))
    shutil.copyfile(path, spv)
    r = subprocess.run([MALIOC, "-c", "Mali-G720", "--vulkan", "--" + stage, "-d", "--format", "json", spv],
                       capture_output=True, text=True)
    rows = []
    try:
        d = json.loads(r.stdout)
    except ValueError:
        return [(os.path.basename(path), stage, "ERROR", r.stderr.strip()[:120])]
    for s in d.get("shaders", []):
        for v in s.get("variants", []):
            p = v["performance"]; names = p["pipelines"]
            lp = dict(zip(names, p["longest_path_cycles"]["cycle_count"]))
            props = {x["name"]: x["value"] for x in v.get("properties", [])}
            rows.append((os.path.basename(path), stage, v["name"],
                         *("%.3f" % (lp.get(k) or 0.0) for k in PIPES),
                         "+".join(p["longest_path_cycles"]["bound_pipelines"]),
                         props.get("work_registers_used", ""), props.get("stack_spilling", "")))
    return rows

def main():
    d = sys.argv[1]
    files = sorted(os.path.join(d, f) for f in os.listdir(d) if ".bin." in f)
    tmp = tempfile.mkdtemp()
    print("file\tstage\tvariant\t" + "\t".join(PIPES) + "\tbound\twork_regs\tspill")
    with cf.ThreadPoolExecutor(8) as ex:
        for rows in ex.map(lambda f: scan(f, tmp), files):
            for row in rows:
                print("\t".join(str(x) for x in row), flush=True)
    shutil.rmtree(tmp)

main()
