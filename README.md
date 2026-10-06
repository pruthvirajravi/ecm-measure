# ecm-measure — ECM-20 decoder measurement campaign on GitHub Actions

Purpose (project "Complementary Low-Rank Structure", D096/D097): collect, in one run, everything the measurement programme needs:
1. **J0090 reference-traffic re-measurement with the D095 address-resolution fix** (`J0090_FIX=1`): every random-access bitstream × five
   cache configurations (32 KB 1-D/128 B, 32 KB 1-D/64 B, 32 KB 2-D, 64 KB 2-D, 128 KB 2-D) → control caches (recon-only / derive-only /
   shared), unique-line floors, reuse-distance histograms (→ ideal fully-associative LRU misses), per-site audit, RESOLVE counters
   (sanity: current-picture-skipped = 0, intermediate-buffer reads counted), a repeat decode on class D (identical per-site counts),
   and the position dump (`J0090_DUMP`) on the 32 KB 1-D runs for the oracle check and footprint analyses.
2. **Bit-exactness**: MD5 of the reconstruction from the instrumented decoder vs the vanilla ECM-20.0 decoder, every stream.
3. **Sign prediction (ADR-028 K3/K4)**: per-TU dump (`SP_DUMP`) on every stream — coverage, n distribution, Σ2^n workload.
4. **Kernel-fetch trace** (`CLR_KTRACE`): inverse-transform kernel accesses per TU (for the NSPT kernel-delivery candidate).
5. **New bitstreams** from the CTC sequences on Google Drive: RA GOP16 (classes D, C; QPs that fit in 6 h) and All-Intra shards
   (D, C, E, B; TemporalSubsampleRatio 8; each shard is an independent IRAP bitstream).

## How to run
1. Upload the 12 committed D094/D096 RA bitstreams into `streams/` (Add file → Upload files; from `work/gha_ecm_measure/streams/`).
   Without them, run the workflow with `decode_existing = false`.
2. Make sure the Drive sequences are shared "anyone with the link" (ids in `sequences.json`), or add a repository secret
   `RCLONE_GDRIVE_TOKEN` (output of `rclone authorize "drive"`) for restricted shares.
3. Actions → **ecm-measure** → *Run workflow*. Defaults: encode RA classes `DC`, AI classes `DCEB`, measure the committed streams, write dumps.
   Expect ~15–20 h wall for the full campaign (≈ 80 encode jobs + ≈ 100 fast decodes + ≈ 160 cache decodes, 20 at a time).
4. Results: artifact **summary** (`SUMMARY.md`, `traffic.csv`, `sp_table.txt`, `bitexact.csv`, raw logs) — also printed in the run summary —
   plus `bitstreams-all` and per-job `results-*` artifacts (dumps, kernel traces). Retention 90 days: download into
   `07_results/hunt2/footprint/gha/` and `07_results/hunt3/gha/`.

## Layout
- `.github/workflows/ecm-measure.yml` — the pipeline (build → encode → plan → decode_fast / decode_cache → summarize)
- `overlay/` — instrumented CacheModel.h/.cpp; `patches/inserts.json` — the 43 anchored one-block insertions into InterPrediction.cpp/DecCu.cpp; `scripts/apply_overlay.py` verifies the
  vanilla md5s (tag ECM-20.0 / 3391a5f) before overlaying and patches TrQuant.cpp (SP dump) by anchors
- `cfg/` — the five J0090 cache configurations; `streams/` — the 12 D094/D096 RA bitstreams; `sequences.json` — Drive ids + geometry
- `scripts/` — build, fetch, encode, decode_fast, decode_cache, plan (matrices), analyze_sp, summarize

Everything added to ECM is env-gated and additive: without `J0090_*`, `SP_DUMP`, `CLR_KTRACE` the instrumented decoder is bit-identical
to vanilla (the run verifies this).
