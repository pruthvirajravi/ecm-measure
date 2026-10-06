#!/usr/bin/env python3
"""Hunt 3 kill tests 3/4 (pre-registered D097): per-TU sign-prediction coverage and search workload from ECM-20 decoder dumps.
TU line: poc comp x y w h predMode lfnstIdx mtsIdx useSP   (every inverse-transformed TU; predMode 0=INTER 1=INTRA 2=IBC 3=PLT)
SP line: poc comp x y w h predMode numPredSigns lfnstIdx mtsIdx (every TU where SP is enabled; n may be 0)"""
import sys, glob, collections, math, os, re
rows=[]
root = sys.argv[1] if len(sys.argv) > 1 else "."
for f in sorted(glob.glob(os.path.join(root, "sp_*.txt"))):
    if os.path.basename(f) == "sp_summary.txt": continue
    tag=os.path.basename(f)[3:-4]; TU=[]; SP=[]
    for line in open(f):
        p=line.split()
        if p[0]=="TU": TU.append(tuple(map(int,p[1:])))
        elif p[0]=="SP": SP.append(tuple(map(int,p[1:])))
    frames=len({t[0] for t in TU}) or 1
    samples=sum(t[4]*t[5] for t in TU); nTU=len(TU)
    sp1=[s for s in SP if s[7]>=1]
    sp_samples=sum(s[4]*s[5] for s in sp1)
    cov=sp_samples/samples if samples else 0
    ndist=collections.Counter(s[7] for s in sp1)
    sum2n=sum(2**s[7] for s in sp1)
    adds=sum((2**s[7])*(min(s[4],32)+min(s[5],32)) for s in sp1)
    it_macs=sum(t[4]*t[5]*(t[4]+t[5]) for t in TU)
    inter=[s for s in sp1 if s[6]==0]; intra=[s for s in sp1 if s[6]==1]
    small=sum(1 for s in sp1 if s[4]*s[5]<=64)
    luma=[s for s in sp1 if s[1]==0]
    mcls=re.search(r"(?:^|_)([A-E])\d_", tag); cls=mcls.group(1) if mcls else "D"
    res={"D":416*240,"C":832*480,"B":1920*1080,"E":1280*720,"A":3840*2160}[cls]
    hyp_per_frame=sum2n/frames; scale=(3840*2160)/res
    hyp_4k60=hyp_per_frame*scale*60
    rows.append(dict(tag=tag,frames=frames,nTU=nTU,nSP=len(SP),nSP1=len(sp1),cov=cov,inter=len(inter),intra=len(intra),small_frac=small/len(sp1) if sp1 else 0,
                     luma_frac=len(luma)/len(sp1) if sp1 else 0,n_mean=sum(s[7] for s in sp1)/len(sp1) if sp1 else 0,n8=ndist.get(8,0)/len(sp1) if sp1 else 0,
                     hyp_pf=hyp_per_frame,hyp_4k60_M=hyp_4k60/1e6,adds_over_itmacs=adds/it_macs if it_macs else 0,ndist=dict(sorted(ndist.items()))))
print(f"{'stream':48s}{'fr':>3s}{'TUs':>7s}{'SP-TUs':>7s}{'n>=1':>7s}{'cov%':>6s}{'inter':>6s}{'intra':>6s}{'<=8x8%':>7s}{'luma%':>6s}{'n_mean':>7s}{'n=8%':>6s}{'hyp/frame':>10s}{'hyp@4K60 Mcyc/s':>16s}{'adds/ITmac':>11s}")
for r in rows:
    print(f"{r['tag']:48s}{r['frames']:3d}{r['nTU']:7d}{r['nSP']:7d}{r['nSP1']:7d}{100*r['cov']:6.1f}{r['inter']:6d}{r['intra']:6d}{100*r['small_frac']:7.1f}{100*r['luma_frac']:6.1f}{r['n_mean']:7.2f}{100*r['n8']:6.1f}{r['hyp_pf']:10.0f}{r['hyp_4k60_M']:16.1f}{r['adds_over_itmacs']:11.3f}")
for r in rows: print(r['tag'],"n distribution",r['ndist'])
