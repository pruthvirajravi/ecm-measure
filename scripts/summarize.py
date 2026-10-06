#!/usr/bin/env python3
"""summarize.py <results_root> <out_dir> — aggregate every job's outputs into tables.

Inputs (as uploaded by the jobs):
  <root>/fast/<tag>/{bitexact.txt, md5_*.txt, times.txt, sp_<tag>.txt, sp_summary.txt, ktrace_<tag>.txt.gz, dec_*.log}
  <root>/cache/<tag>/<cfg>/{fix_<tag>_<cfg>.log, resolve_<cfg>.txt, repeat_<cfg>.txt, time_<cfg>.txt, dump_<tag>.bin.gz}
Outputs: traffic.csv (per stream x cache cfg: control caches, floors, incremental ratios, per-tool MB/frame, ideal-LRU misses),
         sp.csv / sp_table.txt (sign-prediction coverage & workload), bitexact.csv, SUMMARY.md.
"""
import collections, csv, glob, gzip, json, os, re, subprocess, sys

root, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
NAMES = {0: "other", 1: "A:derive-rest", 11: "A:ARMC", 12: "A:TM", 13: "A:BDMVR", 2: "B:finalMC", 21: "B:OBMC", 22: "B:BDOF", 23: "B:LIC", 3: "intra"}

def ideal_lru_misses(first, hist, cap_lines):
    """misses of a fully-associative LRU with cap_lines lines = first touches + reuses with stack distance > cap.
    hist[b] counts accesses with distance in (2^(b-1), 2^b]; a bin is counted as a miss when 2^(b-1) >= cap (conservative: whole bin)."""
    m = first
    for b, n in enumerate(hist):
        lo = 0 if b == 0 else 2 ** (b - 1)
        if lo >= cap_lines: m += n
    return m

rows = []
for log in sorted(glob.glob(os.path.join(root, "cache", "*", "*", "fix_*.log"))):
    txt = open(log, errors="ignore").read()
    if "Cache Statics in total" not in txt: continue
    m = re.search(r"fix_(.+)_(cache_\w+)\.log$", os.path.basename(log)); tag, cfg = m.group(1), m.group(2)
    line = int(re.search(r"Cache line size: (\d+)", txt).group(1)); nl = int(re.search(r"Cache line number (\d+)", txt).group(1)); nw = int(re.search(r"Cache way number (\d+)", txt).group(1))
    frames = len(re.findall(r"Cache Statics in frame", txt)) or 1
    agg = collections.defaultdict(lambda: [0, 0, 0])
    for s in re.findall(r"^SITE (\d+)\|(\S+) (\d+) (\d+) (\d+)", txt, re.M):
        agg[int(s[0])][0] += int(s[2]); agg[int(s[0])][1] += int(s[3]); agg[int(s[0])][2] += int(s[4])
    r = dict(tag=tag, cfg=cfg, cache_kB=line * nl * nw // 1024, line=line, frames=frames)
    for k, n in NAMES.items(): r[f"MBpf[{n}]"] = round(agg[k][2] * line / frames / 2**20, 3)
    A = sum(agg[k][2] for k in (1, 11, 12, 13)); B = sum(agg[k][2] for k in (2, 21, 22, 23))
    r["classA_MBpf"] = round(A * line / frames / 2**20, 3); r["classB_MBpf"] = round(B * line / frames / 2**20, 3)
    mc = re.search(r"CONTROL recon-only-cache misses (-?\d+)\s+derive-only-cache misses (-?\d+)\s+shared misses (\d+)", txt)
    if mc:
        ro, do, sh = map(int, mc.groups())
        r.update(reconOnly_MBpf=round(ro * line / frames / 2**20, 3), deriveOnly_MBpf=round(do * line / frames / 2**20, 3), shared_MBpf=round(sh * line / frames / 2**20, 3),
                 incrA_over_reconOnly=round((sh - ro) / ro, 3) if ro else None, deriveOnly_over_reconOnly=round(do / ro, 3) if ro else None)
    fl = re.search(r"FLOOR unique-lines-per-sequence A (\d+)\s+B (\d+)\s+all (\d+)", txt)
    if fl:
        ua, ub, uall = map(int, fl.groups()); r.update(floorA=ua, floorB=ub, floorAll=uall, A_over_floorA=round(A / ua, 2) if ua else None, B_over_floorB=round(B / ub, 2) if ub else None)
    for cls in ("A", "B", "ALL"):
        rd = re.search(rf"^RD {cls} FIRST (\d+) :((?: \d+)+)", txt, re.M)
        if rd:
            first = int(rd.group(1)); hist = list(map(int, rd.group(2).split()))
            r[f"first_{cls}"] = first
            for kb in (32, 64, 128):
                r[f"idealLRU{kb}k_misses_{cls}"] = ideal_lru_misses(first, hist, kb * 1024 // line)
    rs = re.search(r"RESOLVE counted-in-reference (\d+)\s+reattributed-to-other-ref (\d+)\s+current-picture-skipped (\d+)\s+intermediate-buffer-skipped (\d+)", txt)
    if rs: r.update(resolved=int(rs.group(1)), reattributed=int(rs.group(2)), curpic_skipped=int(rs.group(3)), tmpbuf_skipped=int(rs.group(4)))
    rep = glob.glob(os.path.join(os.path.dirname(log), "repeat_*.txt")); r["repeat"] = open(rep[0]).read().strip() if rep else ""
    rows.append(r)
if rows:
    keys = sorted({k for r in rows for k in r}, key=lambda k: (k not in ("tag", "cfg"), k))
    with open(os.path.join(out, "traffic.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys); w.writeheader(); w.writerows(rows)

# sign prediction
sp_dirs = [d for d in glob.glob(os.path.join(root, "fast", "*")) if glob.glob(os.path.join(d, "sp_*.txt"))]
sp_txt = ""
if sp_dirs:
    tmp = os.path.join(out, "_sp"); os.makedirs(tmp, exist_ok=True)
    files = [f for d in sp_dirs for f in glob.glob(os.path.join(d, "sp_*.txt"))]
    for f in files:
        dst = os.path.join(tmp, os.path.basename(f))
        if not os.path.exists(dst): os.symlink(os.path.abspath(f), dst)
    sp_txt = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "analyze_sp.py"), tmp], capture_output=True, text=True).stdout
    open(os.path.join(out, "sp_table.txt"), "w").write(sp_txt)

# bit-exactness + times
be = []
for d in sorted(glob.glob(os.path.join(root, "fast", "*"))):
    tag = os.path.basename(d)
    bx = open(os.path.join(d, "bitexact.txt")).read().strip() if os.path.exists(os.path.join(d, "bitexact.txt")) else "n/a"
    tm = open(os.path.join(d, "times.txt")).read().strip() if os.path.exists(os.path.join(d, "times.txt")) else ""
    kt = glob.glob(os.path.join(d, "ktrace_*.gz")); nk = 0
    if kt:
        with gzip.open(kt[0], "rt", errors="ignore") as fh:
            for _ in fh: nk += 1
    be.append(dict(tag=tag, bitexact=bx, times=tm, ktrace_lines=nk))
with open(os.path.join(out, "bitexact.csv"), "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=["tag", "bitexact", "times", "ktrace_lines"]); w.writeheader(); w.writerows(be)

with open(os.path.join(out, "SUMMARY.md"), "w") as fh:
    fh.write("# ECM-20 measurement run — summary\n\n")
    fh.write(f"Cache-model rows: {len(rows)}; fast rows: {len(be)}.\n\n## Bit-exactness (instrumented vs vanilla decoder)\n\n| stream | result | decode times | ktrace lines |\n|---|---|---|---|\n")
    for b in be: fh.write(f"| {b['tag']} | {b['bitexact']} | {b['times']} | {b['ktrace_lines']} |\n")
    if rows:
        fh.write("\n## J0090 reference traffic with address-resolution fix (MB/frame; incremental derivation = shared − recon-only)\n\n")
        fh.write("| stream | cfg | kB | recon-only | derive-only | shared | incrA/reconOnly | floorA | floorB | firstA | firstB | reattributed | curpic | tmpbuf | repeat |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            fh.write(f"| {r['tag']} | {r['cfg']} | {r['cache_kB']} | {r.get('reconOnly_MBpf','')} | {r.get('deriveOnly_MBpf','')} | {r.get('shared_MBpf','')} | {r.get('incrA_over_reconOnly','')} | {r.get('floorA','')} | {r.get('floorB','')} | {r.get('first_A','')} | {r.get('first_B','')} | {r.get('reattributed','')} | {r.get('curpic_skipped','')} | {r.get('tmpbuf_skipped','')} | {r.get('repeat','')} |\n")
        fh.write("\nIdeal fully-associative LRU misses (from reuse-distance histograms; whole-bin conservative) are in traffic.csv (idealLRU{32,64,128}k_misses_{A,B,ALL}).\n")
    if sp_txt:
        fh.write("\n## Sign prediction per-TU coverage and search workload\n\n```\n" + sp_txt + "```\n")
print(open(os.path.join(out, "SUMMARY.md")).read()[:3000])
