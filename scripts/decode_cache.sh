#!/usr/bin/env bash
# decode_cache.sh <bin_dir> <bitstream> <cache_cfg> <out_dir> <dump:0|1> <repeat:0|1>
# J0090 cache-model decode with the D095 address-resolution fix. Outputs: fix_<tag>_<cfg>.log (CONTROL / FLOOR / RD histograms /
# SITE audit / RESOLVE), optional position dump (J0090_DUMP, gz), optional second decode to check per-site count identity.
set -euo pipefail
bin=$1; bs=$2; cfg=$3; out=$4; dump=$5; rep=$6; tag=$(basename "$bs" .bin); c=$(basename "$cfg" .cfg); mkdir -p "$out"
log="$out/fix_${tag}_${c}.log"
t0=$(date +%s)
if [ "$dump" = 1 ]; then export J0090_DUMP="$out/dump_${tag}.bin"; fi
J0090_FIX=1 "$bin/DecoderApp_instr" -b "$bs" -o /dev/null --CacheCfg="$cfg" > "$log" 2> "$out/fix_${tag}_${c}.err"
t1=$(date +%s); echo "cache_decode_s $((t1-t0))" > "$out/time_${c}.txt"
grep -q "Cache Statics in total" "$log" || { echo "decode did not finish"; tail -5 "$log"; exit 1; }
grep "^RESOLVE" "$log" | tee "$out/resolve_${c}.txt"
if [ -n "${J0090_DUMP:-}" ] && [ -s "$J0090_DUMP" ]; then gzip -f "$J0090_DUMP"; fi
if [ "$rep" = 1 ]; then
  unset J0090_DUMP
  J0090_FIX=1 "$bin/DecoderApp_instr" -b "$bs" -o /dev/null --CacheCfg="$cfg" > "$out/repeat_${c}.log" 2>/dev/null
  if diff <(grep "^SITE\|^CONTROL\|^FLOOR" "$log") <(grep "^SITE\|^CONTROL\|^FLOOR" "$out/repeat_${c}.log") > /dev/null; then echo "REPEAT IDENTICAL" | tee "$out/repeat_${c}.txt"; else echo "REPEAT DIFFERS" | tee "$out/repeat_${c}.txt"; fi
fi
