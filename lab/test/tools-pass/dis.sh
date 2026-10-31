#!/bin/bash
# dis.sh SLOT ADDR_HEX COUNT_INSNS
a=$((16#$2)); h=$(python3 -I tools-pass/xdsmem.py $1 $2 $(( $3 * 4 ))) || exit 1
echo $h | sed 's/../0x& /g' | /opt/homebrew/opt/llvm/bin/llvm-mc --disassemble -triple=powerpc-unknown-linux-gnu 2>/dev/null | grep -v "\.text" | awk -v b=$a '{printf "%08X %s\n", b+4*(NR-1), $0}'
