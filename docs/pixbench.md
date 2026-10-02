# pixbench — can we make the pixels faster?

Two harnesses that pull the per-pixel colour math out of `d3d11_sw.c` and
`swrast.c` and answer the question directly, in numbers, for a 720p frame.

- **`pixbench.exe`** (`pixbench.c`) — every colour kernel as a scalar reference,
  an AVX2 form (8 px wide) and an AVX512 form (16 px wide). Each vector path is
  checked **bit-exact** against the scalar one before it is timed, then all three
  are run over one 1280×720 frame and reported as ms/frame and frames/sec.
- **`pixbench_gpu.exe`** (`pixbench_gpu.c`) — the same math on the real GPU (a
  D3D11 compute shader), to find out whether offloading it as an overlay beats
  the CPU once the cost of getting the frame onto the card and back is paid.

Both are wired into `build.ps1`.

## Why these operations

The frame's time outside the rasteriser lives in a short list of kernels:

| op | where it lives today | what it is |
|----|----------------------|------------|
| pack f32→8888 | `pack_argb` | clamp, scale, round, assemble |
| unpack 8888→f32 | shader-input decode | the inverse |
| decode B5G6R5 | `res_decode_pixels_inner` | 5/6/5 → 8/8/8 bit-replication |
| decode BC1 | `decode_dxt_color` | palette build + per-texel select |
| swizzle R↔B | `res_decode_pixels_inner` alias path | R8G8B8A8 ↔ B8G8R8A8 |
| color add / sub (sat) | `span_blend_add`, a REVSUBTRACT taken to the limit | per-channel saturating |
| mix / src-over | `span_blend_over` | `s·a + d·(1−a)` |
| modulate | `span_modulate` | vertex-colour scale, div255 |
| RGB→YCbCr / YCbCr→RGB | *new* | the baseband/video colour transform |

The YCbCr pair is the "render as if it were video" and "blue/yellow" part of the
ask: RGB rotated into a luma axis and two opponent-colour chroma axes — **Cb is
the blue↔yellow difference, Cr the red↔green** — which is exactly the space a
video codec works in and where chroma can be subsampled and baseband-encoded.
It is done in Q8 fixed point (JFIF integer coefficients) so the scalar and
vector paths are the same integer arithmetic and agree to the bit.

## CPU results (measured, AMD machine, AVX2+AVX512 present)

ms per 720p frame / frames-per-sec, and speedup over the honest scalar baseline:

```
operation             scalar ms/fps  | avx2 ms/fps     x    | avx512 ms/fps   x
pack f32->8888          0.499/  2005  |   0.245/  4080  2.04x |   0.245/  4081  2.04x
unpack 8888->f32        0.264/  3792  |   0.182/  5509  1.45x |   0.174/  5746  1.52x
decode B5G6R5           0.275/  3631  |   0.137/  7324  2.02x |   0.097/ 10261  2.83x
decode BC1 (texels)     0.450/  2222  |   0.450/  2220  1.00x |   0.482/  2074  0.93x
swizzle R<->B           0.107/  9363  |   0.060/ 16580  1.77x |   0.062/ 16236  1.73x
color add (sat)         0.521/  1920  |   0.087/ 11455  5.96x |   0.091/ 10952  5.70x
color sub (sat)         0.417/  2398  |   0.088/ 11353  4.74x |   0.091/ 10950  4.57x
mix / src-over          1.025/   975  |   0.196/  5090  5.22x |   0.153/  6539  6.71x
modulate (div255)       0.408/  2451  |   0.115/  8678  3.54x |   0.100/  9992  4.08x
baseband RGB->YCbCr     0.666/  1501  |   0.252/  3975  2.65x |   0.223/  4483  2.99x
baseband YCbCr->RGB     0.671/  1489  |   0.236/  4235  2.84x |   0.217/  4606  3.09x
```

Reading it:

- **The compute-dense ops are where the win is.** Source-over mix hits **6.7×**
  on AVX512, saturating add/sub **~5–6×**, modulate **~4×**, the YCbCr baseband
  transforms **~3×**, B5G6R5 decode **2.8×**. These are the kernels a frame
  actually spends its time in, and they widen almost linearly.
- **AVX512 pulls ahead of AVX2 only when there is arithmetic to hide the
  clock behind.** For the byte-shuffle ops (add/sub/swizzle) the two are level or
  AVX2 is a hair faster — they are memory-bandwidth bound and 512-bit width buys
  nothing. For the multiply-heavy ops (over, modulate, 565, YCbCr) 512 is clearly
  ahead (6.71× vs 5.22× on over; 2.83× vs 2.02× on 565).
- **Two flat spots are diagnostic, not disappointing.** `swizzle` and `unpack`
  are single-pass, low-arithmetic streams — bandwidth caps them near ~1.7×.
  `BC1` does **not** win, because the palette build is still per-block scalar and
  the per-texel `vpermd` alone can't pay for itself; beating it needs the palette
  math vectorised *across* blocks, which is the next move if BC1 decode ever
  shows up hot.

Methodology note: the baseline is built with generic codegen (no global `-mavx`),
so the scalar column is a real scalar/SSE2 baseline — the same code generation
the stock DLL ships with. Building the whole unit `-mavx512` lets the compiler
auto-vectorise the "scalar" reference too and collapses every ratio toward 1×,
which is a measurement artefact, not a result. `-ffp-contract=off` keeps the
float pack from contracting into an FMA that rounds differently than scalar.

## GPU results (measured, Radeon RX 7900 XTX)

ms per 720p frame / frames-per-sec:

```
operation               cpu-scalar    gpu dispatch    gpu roundtrip   gpu resident
color add (sat)          1.943(  515)   0.014( 72171)  2.080(  481)   0.001(1175088)
mix / src-over           2.017(  496)   0.011( 87347)  1.330(  752)   0.001(1745202)
baseband YCbCr->RGB      1.952(  512)   0.009(116058)  1.314(  761)   0.001(1709402)
```

Shader output is **bit-exact** with the CPU reference (same Q8 integer recipes).

The verdict is unambiguous and it is about data movement, not arithmetic:

- **The GPU ALU is effectively free.** One 720p frame's colour math dispatches in
  **9–14 µs** — 70k–120k fps. Repeated with data resident it disappears into the
  queue entirely.
- **Offloading it as an overlay is a loss.** The round-trip — upload two 720p
  inputs across PCIe, dispatch, copy to staging, map back — costs **1.3–2.1 ms**,
  which is roughly the same as CPU *scalar* and **10–20× worse than the CPU
  AVX512** number for the same op (0.09–0.22 ms). The transfer dwarfs the work by
  two orders of magnitude.
- **So the GPU only pays if the pixels never come back.** That is exactly the
  "render as if it were video, decode on the GPU, present from the GPU" path —
  `D3D11SW_GPU>=3` / `gpu_shadow_draw` + `gpu_release_cpu_copy` in `d3d11_sw.c`,
  where the frame is produced and presented on the card and the CPU copy is let
  go. There the 9 µs dispatch is the whole story and the readback line never runs.

## What to take from it

1. For the software path as it stands — pixels on the CPU, presented from the
   CPU — **AVX512 is the win**, and the kernels worth widening first are
   source-over, add/sub, modulate and the decodes, in that order. That is a
   straight lift of these bit-exact kernels into `swrast.c`'s span code, which
   already carries the AVX2 forms; the 512-bit forms here drop in beside them.
2. The GPU is not a faster way to *do* the colour math for one frame; it is a
   way to *never move the frame*. Attaching it directly only makes sense as the
   present-from-GPU pipeline, not as an offload overlay — the round-trip erases
   any ALU advantage many times over. This confirms the direction `gpu.c` /
   `gpu_mode` is already pointed and quantifies why the CPU-copy release matters.

## Hand tuning past the straight AVX512 kernels (`pixtune.exe`)

The follow-up question — is there more in the CPU path with hand work — is
answered by `pixtune.c`, which pits three levers against the plain AVX512 form,
each checked bit-exact:

```
color add: stream store     base  0.092 (10881)   tuned  0.119 ( 8386)   -22.9%
swizzle: stream store       base  0.061 (16458)   tuned  0.119 ( 8380)   -49.1%
src-over: 2 accumulators    base  0.163 ( 6135)   tuned  0.155 ( 6454)    +5.2%
decode+mod+over: fuse 3->1  base  0.350 ( 2858)   tuned  0.347 ( 2881)    +0.8%
```

These four numbers agree on one diagnosis: **at 720p the buffers are L3-resident
and the kernels are compute-bound, not bandwidth-bound.**

- **Non-temporal stores lose, badly** (−23% add, −49% swizzle). A streaming store
  bypasses the cache, and here the data was in cache — so it throws away the
  fast path and forces the write to memory. NT stores are the wrong lever for a
  720p frame; they would only pay if the working set were far larger than L3.
- **Two independent accumulators buy ~5% on source-over** — a real, free win. The
  div255 multiply chain is slightly latency-bound, and issuing two 16-wide chains
  per iteration gives the out-of-order engine something to overlap. Worth taking
  on the blends and modulate.
- **Fusion barely moves the clock at 720p (+0.8%)** — but not because fusion is
  worthless. It is because the three passes' *arithmetic* is the bottleneck, not
  the traffic between them, since L3 serves the intermediates almost for free.
  Fusion's real payoffs are elsewhere: it removes the two 3.6 MB scratch buffers
  entirely (this is a 2 GB process fighting for address space), and once the
  texture working set spills past L3 to DRAM — which the real game's 641 MB of
  payload does — the traffic it removes stops being free. `swrast.c`'s span
  kernel already fuses sample+modulate+blend+store, so the guidance is to keep
  the decodes folded into that path rather than run them as separate buffer
  passes.

What the diagnosis rules out and in: since the hot kernels are compute-bound on
the div255 multiplies, the remaining multipliers are (1) **threading**, which
`swrast.c` already has via its tile worker pool — that is the real Nx, not a
wider register; and (2) **cutting multiplies** in the blend, e.g. the lerp form
`d + a·(s−d)` uses one 16-bit multiply per channel-group instead of two, ~15–20%
of the over cost — but its signed rounding does not match the current
`div255(s·a + d·(255−a))` bit for bit, so it is a correctness/​speed trade to
decide deliberately, not a free lift. Everything that *is* free and bit-exact —
AVX512 width and the two-accumulator unroll — is already on the table above.

## Rebuild / rerun

```
pwsh -File build.ps1              # builds both alongside everything else
./pixbench.exe                    # CPU table
./pixtune.exe                     # hand-tuning levers vs the AVX512 baseline
# run the GPU one from outside the repo dir so the real d3d11/dxgi load:
cp pixbench_gpu.exe "$env:TEMP" ; & "$env:TEMP\pixbench_gpu.exe"
```

`pixbench.exe` exits nonzero if any vector path diverges from scalar;
`pixbench_gpu.exe` exits nonzero if the shader diverges from the CPU reference.
