#!/usr/bin/env python3
"""Emit GitHub Actions matrices (JSON) for the measurement run.

plan.py encode  <ra_classes> <ai_classes>   -> encode matrix: one job per (sequence, mode, QP, shard), all sized to finish < 6 h
plan.py decode  <streams_dir> [more_dirs...] -> fast matrix (every stream) and cache matrix (RA streams x 5 cache configs)

Sizing (single-thread ECM-20, GitHub 4-vCPU runner ≈ the cloud core used for D094):
  RA GOP16 17 frames: class D QP32 took 19–64 min (measured) → class D at QP 22/27/37 ≤ 2 h; class C ≈ 4× → QP 27/37 only (≤ 5 h);
  class B/E RA cannot finish in 6 h → left to the workstation.
  AI (TemporalSubsampleRatio 8): class D 8 coded frames (~0.5 h), class C 8 frames (~1.5 h), class E 4 frames (~2–3 h),
  class B 4 frames per shard, 2 shards (~3–4 h each). AI shards are independent IRAP pictures, so each shard is a valid bitstream.
"""
import glob, json, os, re, sys

SEQ = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sequences.json")))
SEQ = {k: v for k, v in SEQ.items() if not k.startswith("_")}

def encode_matrix(ra_classes, ai_classes):
    jobs = []
    for name, s in SEQ.items():
        c = s["cls"]
        if c in ra_classes:
            qps = {"D": [22, 27, 37], "C": [27, 37]}.get(c, [])
            if name.startswith(("C1_", "C2_")): qps = sorted(set(qps + [32]))   # sequences without an existing QP32 stream
            for q in qps:
                jobs.append(dict(name=name, mode="ra16", qp=q, frames=17, skip=0, minutes={"D": 150, "C": 345}[c]))
        if c in ai_classes:
            if c == "D":   shards, frames, qps = [0], 8, [22, 27, 32, 37]
            elif c == "C": shards, frames, qps = [0], 8, [22, 27, 32, 37]
            elif c == "E": shards, frames, qps = [0], 4, [22, 32]
            elif c == "B": shards, frames, qps = [0, 32], 4, [22, 32]
            else: continue
            for q in qps:
                for sk in shards:
                    jobs.append(dict(name=name, mode="ai", qp=q, frames=frames, skip=sk, minutes={"D": 90, "C": 180, "E": 240, "B": 300}[c]))
    return jobs

def decode_matrices(dirs):
    streams = []
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, "*.bin"))):
            if "PARTIAL" in f: continue
            streams.append(f)
    fast = [dict(stream=s, tag=os.path.basename(s)[:-4]) for s in streams]
    cache = []
    for s in streams:
        tag = os.path.basename(s)[:-4]
        if "_ra16_" not in tag: continue                      # reference traffic is meaningful for inter streams only
        cls = re.search(r"_([A-E])\d_", tag).group(1)
        for cfg in ["cache_A", "cache_A64", "cache_B32_2D", "cache_E64_2D", "cache_G128_2D"]:
            dump = 1 if cfg == "cache_A" else 0
            rep = 1 if (cls == "D" and cfg == "cache_A") else 0          # repeat-decode identity check (cheap on class D)
            minutes = {"D": 60, "C": 150, "B": 345, "E": 200}[cls]
            cache.append(dict(stream=s, tag=tag, cfg=cfg, dump=dump, rep=rep, minutes=minutes))
    return fast, cache

if __name__ == "__main__":
    if sys.argv[1] == "encode":
        ra = list(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] != "none" else []
        ai = list(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] != "none" else []
        m = encode_matrix(ra, ai)
        print(json.dumps({"include": m}))
        print(f"{len(m)} encode jobs", file=sys.stderr)
    else:
        fast, cache = decode_matrices(sys.argv[2:])
        out = {"fast": {"include": fast}, "cache": {"include": cache}}
        print(json.dumps(out))
        print(f"{len(fast)} fast jobs, {len(cache)} cache jobs", file=sys.stderr)
