#!/usr/bin/env bash
# encode.sh <enc_bin> <ecm_cfg_dir> <seq_name> <yuv> <mode: ra16|ai> <qp> <frames> <frameskip> <out_dir>
# ra16: encoder_randomaccess_gop16_ecm.cfg, IntraPeriod 16 (same configuration as the D094 streams).
# ai  : encoder_intra_ecm.cfg (CTC: TemporalSubsampleRatio 8 -> every 8th source frame; shards use FrameSkip in source frames).
set -euo pipefail
enc=$1; cfgdir=$2; name=$3; yuv=$4; mode=$5; qp=$6; frames=$7; skip=$8; out=$9
w=$(python3 -c "import json; print(json.load(open('sequences.json'))['$name']['w'])")
h=$(python3 -c "import json; print(json.load(open('sequences.json'))['$name']['h'])")
fps=$(python3 -c "import json; print(json.load(open('sequences.json'))['$name']['fps'])")
mkdir -p "$out"
if [ "$mode" = ra16 ]; then
  tag="ecm_ra16_${name}_f${frames}_q${qp}"; cfg="$cfgdir/encoder_randomaccess_gop16_ecm.cfg"; extra=(--IntraPeriod=16)
else
  tag="ecm_ai_${name}_s${skip}_f${frames}_q${qp}"; cfg="$cfgdir/encoder_intra_ecm.cfg"; extra=(--TemporalSubsampleRatio=8)
fi
echo "encode $tag"; start=$(date +%s)
"$enc" -c "$cfg" -i "$yuv" -wdt "$w" -hgt "$h" -fr "$fps" -f "$frames" --FrameSkip="$skip" --InputBitDepth=8 --OutputBitDepth=8 -q "$qp" "${extra[@]}" -b "$out/$tag.bin" -o /dev/null > "$out/$tag.enc.log" 2>&1
grep -q "Total Time" "$out/$tag.enc.log" || { tail -20 "$out/$tag.enc.log"; exit 1; }
echo "$tag $(( $(date +%s) - start )) s" | tee "$out/$tag.time"
