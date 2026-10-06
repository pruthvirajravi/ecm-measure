#!/usr/bin/env python3
"""Apply the measurement instrumentation to a pristine ECM-20.0 checkout.

Usage: apply_overlay.py <ecm_src_dir> <overlay_dir>

1. Verifies the five vanilla files we touch have the expected ECM-20.0 md5 (tag 3391a5f; LF-normalised).
2. Copies the instrumented versions (J0090 per-stage tags, control caches, reuse-distance histograms, call-site
   audit, D095 address-resolution fix [J0090_FIX=1], position dump [J0090_DUMP=file]).
3. Patches TrQuant.cpp in place: CLR_KTRACE kernel-fetch trace (ADR-013 Stage B, anchored inserts) + per-TU sign-prediction dump (SP_DUMP=file) using two textual anchors.
Everything is additive and env-gated: without the env variables the decoder is bit-identical to vanilla.
"""
import hashlib, json, os, shutil, sys

# md5 of the LF-normalised file (git checkouts are LF; Windows working copies are CRLF — both accepted)
VANILLA_MD5 = {
    "source/Lib/CommonLib/CacheModel.cpp": "e11acd35d4af3681f01ba80dcb0516d8",
    "source/Lib/CommonLib/CacheModel.h": "7cf8af41a91b844dfd8bbad12c45d71d",
    "source/Lib/CommonLib/InterPrediction.cpp": "365d23b0687da86828a56285a26f4317",
    "source/Lib/DecoderLib/DecCu.cpp": "e30b04d248b7f1cfafd25116cc9e585f",
    "source/Lib/CommonLib/TrQuant.cpp": "34e2f4e9846f32a2e4bd57c1a96c1107",
}

SP_HELPER = '''
// Hunt 3 / ADR-028: per-TU sign-prediction / residual-TU dump (env SP_DUMP=<file>). Additive only.
#include <cstdio>
#include <cstdlib>
static FILE* g_spDump = nullptr; static bool g_spDumpInit = false;
static inline FILE* spDump() { if ( !g_spDumpInit ) { g_spDumpInit = true; const char* p = getenv( "SP_DUMP" ); if ( p ) g_spDump = fopen( p, "w" ); } return g_spDump; }
'''

def md5(p):
    return hashlib.md5(open(p, "rb").read().replace(b"\r\n", b"\n")).hexdigest()

def main():
    src, ovl = sys.argv[1], sys.argv[2]
    bad = []
    for rel, want in VANILLA_MD5.items():
        got = md5(os.path.join(src, rel))
        print(f"{'OK ' if got == want else 'BAD'} {rel} {got}")
        if got != want:
            bad.append(rel)
    if bad and not os.environ.get("SKIP_MD5"):
        sys.exit("vanilla md5 mismatch for: " + ", ".join(bad) + " — refusing to overlay (wrong ECM tag?)")
    for rel in ["source/Lib/CommonLib/CacheModel.cpp", "source/Lib/CommonLib/CacheModel.h"]:
        shutil.copyfile(os.path.join(ovl, rel), os.path.join(src, rel))
        print("overlaid", rel)
    # InterPrediction.cpp / DecCu.cpp / TrQuant.cpp: pure insertions (J0090_CTX tags, g_j0090Pu, CLR_KTRACE) anchored on unique preceding context
    inserts = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "patches", "inserts.json")))
    for rel, items in inserts.items():
        fp = os.path.join(src, rel)
        raw = open(fp, "rb").read(); crlf = b"\r\n" in raw
        text = raw.decode("utf-8", errors="surrogateescape").replace("\r\n", "\n")
        pos = []
        for o in items:
            key = "\n".join(o["after"]) + "\n"
            n = text.count(key)
            if n != 1:
                sys.exit(f"anchor not unique ({n}) in {rel}: {o['after'][-1]!r}")
            pos.append((text.index(key) + len(key), "\n".join(o["lines"]) + "\n"))
        for q, ins in sorted(pos, reverse=True):
            text = text[:q] + ins + text[q:]
        if crlf: text = text.replace("\n", "\r\n")
        open(fp, "wb").write(text.encode("utf-8", errors="surrogateescape"))
        print(f"inserted {len(items)} blocks into {rel}")
    # TrQuant.cpp: SP_DUMP textual patch (after the CLR_KTRACE inserts)
    p = os.path.join(src, "source/Lib/CommonLib/TrQuant.cpp")
    c = open(p, encoding="utf-8", errors="surrogateescape", newline="").read().replace("\r\n", "\n")
    a1 = '#include "TrQuant.h"\n'
    assert c.count(a1) == 1, "anchor 1 (include) not unique"
    c = c.replace(a1, a1 + SP_HELPER, 1)
    a2 = "  const uint32_t uiHeight     = area.height;\n"
    i = c.index("void TrQuant::invTransformNxN(")
    j = c.index(a2, i)
    tu_line = ('  if ( FILE* f = spDump() ) fprintf( f, "TU %d %d %d %d %u %u %d %d %d %d\\n", tu.cs->slice->getPOC(), (int) compID, '
               'area.x, area.y, uiWidth, uiHeight, (int) tu.cu->predMode, (int) tu.cu->lfnstIdx, (int) tu.mtsIdx[compID], '
               '(int) TU::getUseSignPred( tu, compID ) );\n')
    c = c[:j + len(a2)] + tu_line + c[j + len(a2):]
    a3 = "  int32_t numPredSigns = (int32_t)predSignsXY.size();\n"
    k = c.index("void TrQuant::predCoeffSigns(")
    m = c.index(a3, k)
    sp_line = ('  if ( FILE* f = spDump() ) { if ( !tu.cs->pcv->isEncoder ) fprintf( f, "SP %d %d %d %d %u %u %d %d %d %d\\n", '
               'tu.cs->slice->getPOC(), (int) residCompID, tu.blocks[residCompID].x, tu.blocks[residCompID].y, '
               'tu.blocks[residCompID].width, tu.blocks[residCompID].height, (int) tu.cu->predMode, numPredSigns, '
               '(int) tu.cu->lfnstIdx, (int) tu.mtsIdx[residCompID] ); }\n')
    c = c[:m + len(a3)] + sp_line + c[m + len(a3):]
    open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(c)
    print("patched TrQuant.cpp (CLR_KTRACE inserts + SP_DUMP hook)")

if __name__ == "__main__":
    main()
