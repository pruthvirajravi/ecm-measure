#!/usr/bin/env bash
# decode_fast.sh <bin_dir> <bitstream> <out_dir> - per-stream fast measurements (no cache model):
#  (1) vanilla decode -> recon MD5; (2) instrumented decode -> recon MD5 (bit-exactness gate); (3) SP_DUMP per-TU sign-prediction
#  records; (4) CLR_KTRACE kernel-fetch trace (inverse-transform kernel accesses per TU; gz) ; (5) decode times.
set -euo pipefail
bin=$1; bs=$2; out=$3; tag=$(basename "$bs" .bin); mkdir -p "$out"
t0=$(date +%s); "$bin/DecoderApp_vanilla" -b "$bs" -o "$out/recon_vanilla.yuv" > "$out/dec_vanilla.log" 2>&1; t1=$(date +%s)
md5sum "$out/recon_vanilla.yuv" | cut -d' ' -f1 > "$out/md5_vanilla.txt"; rm -f "$out/recon_vanilla.yuv"
SP_DUMP="$out/sp_$tag.txt" CLR_KTRACE="$out/ktrace_$tag.txt" "$bin/DecoderApp_instr" -b "$bs" -o "$out/recon_instr.yuv" > "$out/dec_instr.log" 2>&1; t2=$(date +%s)
md5sum "$out/recon_instr.yuv" | cut -d' ' -f1 > "$out/md5_instr.txt"; rm -f "$out/recon_instr.yuv"
gzip -f "$out/ktrace_$tag.txt" 2>/dev/null || true
if cmp -s "$out/md5_vanilla.txt" "$out/md5_instr.txt"; then echo "BITEXACT OK $(cat "$out/md5_vanilla.txt")" | tee "$out/bitexact.txt"; else echo "BITEXACT MISMATCH" | tee "$out/bitexact.txt"; fi
echo "vanilla_decode_s $((t1-t0)) instr_decode_s $((t2-t1))" | tee "$out/times.txt"
python3 scripts/analyze_sp.py "$out" > "$out/sp_summary.txt" 2>&1 || true
