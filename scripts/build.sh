#!/usr/bin/env bash
# build.sh <ecm_src> <variant: vanilla|instr> <outdir> — builds ECM-20 EncoderApp/DecoderApp (Release, GCC).
set -euo pipefail
src=$1; var=$2; out=$3
cmake_flags=(-DCMAKE_BUILD_TYPE=Release -DSET_ENABLE_SPLIT_PARALLELISM=OFF -DSET_ENABLE_WPP_PARALLELISM=OFF)
if [ "$var" = instr ]; then cmake_flags+=(-DCMAKE_CXX_FLAGS="-DJVET_J0090_MEMORY_BANDWITH_MEASURE=1"); fi
mkdir -p "$src/build_$var"; cd "$src/build_$var"
cmake .. "${cmake_flags[@]}" > cmake.log 2>&1 || { tail -40 cmake.log; exit 1; }
targets="DecoderApp"; [ "$var" = vanilla ] && targets="DecoderApp EncoderApp"
make -j"$(nproc)" $targets 2>&1 | tail -5
mkdir -p "$out"
for t in $targets; do f=$(find "$src/bin" -type f -name "${t}*" -perm -u+x | grep -i release | head -1); cp "$f" "$out/${t}_$var"; done
ls -la "$out"
