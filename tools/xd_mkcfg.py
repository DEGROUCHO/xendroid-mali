#!/usr/bin/env python3
"""Create per-test TOML configs, inserting the keys inside the right table
(no duplicate tables: xenia fails to parse with 'cannot redefine existing table')."""
import re
import sys

BASE = "/tmp/q8_rotoff.toml"


def load_base(path=BASE):
    with open(path) as f:
        return f.read()


def set_key(text, key, value, table):
    """Set key inside [table]; drop any existing occurrence (any table)."""
    # remove existing lines with this key
    lines = [l for l in text.splitlines()
             if not re.match(r"\s*%s\s*=" % re.escape(key), l)]
    out = []
    inserted = False
    cur = None
    for l in lines:
        m = re.match(r"\s*\[([^\]]+)\]\s*$", l)
        if m:
            cur = m.group(1)
            out.append(l)
            if cur == table and not inserted:
                out.append("%s = %s" % (key, value))
                inserted = True
            continue
        out.append(l)
    if not inserted:
        out.append("[%s]" % table)
        out.append("%s = %s" % (key, value))
    return "\n".join(out) + "\n"


def make(name, kv, base=BASE):
    t = load_base(base)
    for k, v in kv.items():
        table = "Vulkan" if k.startswith("vulkan_") else "GPU"
        t = set_key(t, k, v, table)
    p = "/tmp/%s.toml" % name
    with open(p, "w") as f:
        f.write(t)
    # check: no duplicate tables
    tabs = re.findall(r"^\[([^\]]+)\]\s*$", t, re.M)
    dup = len(tabs) != len(set(tabs))
    print("%-24s %s%s" % (name, p, "  [DUP!]" if dup else "  ok"))
    return p


if __name__ == "__main__":
    make("t1_ownmemo_off", {"rt_cache_ownership_claim_memo": "false"})
    make("t2_directresolve_off", {"vulkan_direct_host_resolve": "false"})
    make("t3_numfmt_off", {"resolve_check_number_format": "false"})
    make("t4_transfers_keys_off", {
        "vulkan_in_pass_transfers": "false",
        "vulkan_normalize_dontcare_keys": "false",
    })
    make("t5_inpass_off", {"vulkan_in_pass_resolve": "false"})
    make("t6_nuclear", {
        "rt_cache_ownership_claim_memo": "false",
        "resolve_check_number_format": "false",
        "vulkan_direct_host_resolve": "false",
        "vulkan_in_pass_transfers": "false",
        "vulkan_normalize_dontcare_keys": "false",
    })
    make("t0_baseline", {})
