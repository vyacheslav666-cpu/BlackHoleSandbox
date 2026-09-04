# Architecture

A deliberately small C++20 host driving one heavy ray-tracing shader. The split is
strict: the CPU owns the window, the camera, the parameters and the pass
ordering; the GPU owns every single ray.

```
main.cpp
  └─ parseCommandLine ──► AppOptions ──► Application
                                            │
                    ┌───────────────────────┼───────────────────────┐
                    │                       │                       │
              interactive loop         capture loop            shared passes
              (run)                    (runCapture)            (renderScene, …)
```

---

## Source layout

| File | Responsibility |
| --- | --- |
| [`src/main.cpp`](../src/main.cpp) | Entry point. Also exports `NvOptimusEnablement`, without which a hybrid-graphics laptop silently runs this on the *integrated* GPU. |
| [`src/Application/AppOptions.*`](../src/Application/AppOptions.hpp) | Command-line parsing, including the `--set name=value` table that exposes every renderer parameter to the capture tool. |
| [`src/Application/Application.*`](../src/Application/Application.hpp) | Window, GL context, ImGui, render targets, pass ordering, both main loops. |
| [`src/Camera/Camera.*`](../src/Camera/Camera.hpp) | Orbit and free-flight camera. Orbit mode always looks at the origin, which is the black hole. |
| [`src/Physics/BlackHoleParameters.hpp`](../src/Physics/BlackHoleParameters.hpp) | The single struct describing a frame, its clamping rules, the landmark radii, and the quality presets. |
| [`src/Renderer/ShaderProgram.*`](../src/Renderer/ShaderProgram.hpp) | Move-only RAII GLSL program with actionable compile/link errors. Also a small `#include` preprocessor, since GLSL has none and the tracer is shared between two stages. |
| [`src/Renderer/RenderTarget.*`](../src/Renderer/RenderTarget.hpp) | One colour attachment plus its framebuffer, in RGBA16F, RGBA32F or RGBA8. |
| [`src/Renderer/ImageWriter.*`](../src/Renderer/ImageWriter.hpp) | Dependency-free PNG writer (stored-deflate) used by screenshots and `--shot`. |
| [`src/Renderer/GpuTimer.*`](../src/Renderer/GpuTimer.hpp) | `GL_TIME_ELAPSED` query ring that measures what the ray-tracing pass actually costs on the GPU. |
| [`src/UI/ControlPanel.*`](../src/UI/ControlPanel.hpp) | The whole ImGui panel. Reads and writes `BlackHoleParameters`; touches no GL. |

Shaders in [`shaders/`](../shaders), all sharing one vertex stage:

| Shader | Role |
| --- | --- |
| `fullscreen.vert` | One oversized triangle covering the viewport. No vertex buffer, no attributes — `glDrawArrays(GL_TRIANGLES, 0, 3)`. |
| `black_hole_common.glsl` | **The renderer.** Two geodesic integrators (planar Schwarzschild and full Kerr), volumetric disk transfer, starfield, all debug views. Heavily commented. Not a stage of its own: included by both entry points below. |
| `black_hole.frag` | Fragment entry point. Takes the pixel from the interpolated `vUv`, writes a colour attachment. |
| `black_hole.comp` | Compute entry point. Takes the pixel from `gl_GlobalInvocationID`, writes through `imageStore`. Selected with `--set tracer=compute`. |
| `wavefront_*.comp`, `wavefront_common.glsl` | A third scheduling of the same tracer: rays parked in a buffer and advanced a chunk at a time, compacted in between. Kept because it is measured, not because it is fast — see below. |
| `accumulate.frag` | Progressive refinement: a running arithmetic mean of jittered frames. |
| `bloom_downsample.frag` | One level of the bloom pyramid's downsample chain; level 0 also does the bright pass. |
| `bloom_upsample.frag` | One level of the upsample chain, blended into the finer level. |
| `postprocess.frag` | Bloom composite, exposure, tone mapping, dither, gamma. |

---

## The frame

```
                 ┌──────────────────────────────────────────┐
   camera  ─────►│ black_hole.frag / .comp                  │
   params  ─────►│   ray generation (jittered)              │
                 │   RK4 geodesic integration               │──► sceneTarget_
                 │   volumetric disk transfer               │    RGBA16F
                 │   procedural sky                         │
                 └──────────────────────────────────────────┘
                                    │
                 ┌──────────────────▼───────────────────────┐
                 │ accumulate.frag                          │──► accumulation
                 │   mean_n = mean_{n-1} + (x_n - m)/n      │    RGBA32F ×2
                 └──────────────────┬───────────────────────┘    (ping-pong)
                                    │
             ┌──────────────────────▼───────────────────────┐
             │ bloom pyramid                                │
             │   N× bloom_downsample.frag  (½ each level)   │──► bloomChain_
             │   N× bloom_upsample.frag    (blend upwards)  │    RGBA16F
             └──────────────────────┬───────────────────────┘
                                    │
             ┌──────────────────────▼───────────────────────┐
             │ postprocess.frag                             │──► default FB
             │   + bloom, × exposure, tone map, dither, γ   │    or captureTarget_
             └──────────────────────────────────────────────┘
                                    │
                              ImGui draw data
```

Everything stays in linear HDR until `postprocess.frag`. That is what lets the
bloom threshold and the tone mapper see real radiance values rather than
already-clipped colours.

### Why these formats

* `sceneTarget_` is **RGBA16F** — plenty for one frame's radiance.
* The accumulation buffers are **RGBA32F**. A running mean over hundreds of
  samples would visibly quantise in half precision.
* `captureTarget_` is **RGBA8** because it holds the final display-encoded image
  ready for `glReadPixels`.

---

## Progressive refinement

The single biggest quality win in the renderer, and it is almost free.

The tracer offsets its ray by a **Halton (2,3)** sub-pixel position that
changes every frame. `accumulate.frag` keeps a running arithmetic mean:

```
mean_n = mean_{n-1} + (sample_n - mean_{n-1}) / n
```

While the camera and every parameter hold still, the image is effectively
super-sampled: the shadow edge, the photon ring and the sub-pixel stars all
resolve cleanly within a second or two. Cost while moving is unchanged.

**Correctness depends entirely on knowing when to reset.** `Application`
computes an FNV-1a fingerprint over the camera matrix, the field of view, the
render resolution, every image-affecting parameter *and* the animation clock. If
the fingerprint changes, the sample counter resets to zero. Because there is no
motion-vector reprojection, a stale frame must never survive a change — so the
reset is conservative by construction.

The animation clock is frozen while refining (`freezeAnimationWhileRefining`),
otherwise the disk pattern would move under the average and smear it. The
practical effect is pleasant: fly around, stop, and the image resolves into a
clean still.

Once the budget is reached the renderer stops re-tracing entirely and just keeps
displaying the finished average — a converged still frame costs almost nothing.

## Bloom

A standard pyramid: `bloomLevels` successive half-resolution downsamples, then
back up, each coarse level blended into the next finer one.

* Level 0 applies the soft-knee bright pass. Later levels must not, or the glow
  would be eroded at every step.
* Level 0 also uses a **Karis average** (weight by `1/(1+luma)`), which stops a
  single very bright sub-pixel star from pumping a whole bloom kernel and
  flickering as the camera moves.
* The upsample is a **mix**, not an add — `fine = mix(fine, blur(coarse), w)` via
  `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`. Summing six levels additively
  multiplies total brightness by roughly six and turns every star into a sheet
  of haze; the earlier version of this renderer did exactly that.

Since a level is both a source and a destination, the shader outputs *only* the
blurred coarse level and the fixed-function blend unit does the combine.

---

## Three entry paths, one renderer

`Application::run()` branches once:

* **Interactive** — poll, input, resize, refine, bloom, postprocess to the
  default framebuffer, ImGui on top.
* **Capture** (`--shot`) — hidden window, still a real GL context, render
  `--samples` accumulated frames, postprocess into `captureTarget_`, read back,
  flip rows (GL is bottom-up, PNG is top-down), write PNG, exit.
* **Sequence** (`--sequence`) — the same still, once per frame, each on its own
  animation clock with the accumulator reset in between. This is the only way to
  see the disk in motion *and* converged: interactively the clock is frozen
  while the image refines (`freezeAnimationWhileRefining`), because otherwise
  the pattern would move under the running average and smear it. The trade is
  unavoidable in real time and disappears offline.

  A frame's clock is computed from its index, never advanced from the frame
  before. Nothing in the renderer integrates state across frames — the disk's
  rotation, the accretion inflow and the jet knots are all closed forms in
  `uTime` — so this is exact rather than merely convenient, and an instant
  renders identically whatever `--frames` is. Verified: `t = k` is byte-identical
  as frame `k` of a 6-frame run and frame `2k` of an 11-frame run over the same
  interval.

Both call the same `renderScene` / `accumulateScene` / `renderBloom` /
`renderPostprocess`. That is the point: what `--shot` produces is byte-for-byte
what the window shows at the same settings, which is what makes visual changes
reviewable without a human watching.

[`tools/render_sheet.ps1`](../tools/render_sheet.ps1) drives it over a fixed set
of viewpoints and every debug mode.

---

## Two tracer backends, one tracer

The ray-tracing pass exists as both a fragment and a compute shader. They are not
two implementations: [`black_hole_common.glsl`](../shaders/black_hole_common.glsl)
holds the entire tracer and both entry points include it, so there is one copy of
the physics and it cannot drift. The wrappers are eighteen and sixty-eight lines,
and all either does is decide where a pixel coordinate comes from and where the
colour goes:

| | fragment | compute |
| --- | --- | --- |
| pixel | interpolated `vUv` | `(gl_GlobalInvocationID.xy + 0.5) / resolution` |
| output | colour attachment | `imageStore` into an `rgba16f image2D` |
| dispatch | one oversized triangle | workgroups rounded up over the image |

GLSL has no `#include`, so `ShaderProgram` implements one — the alternative was
two copies of a two-thousand-line tracer. Each file gets a source-string number
and a `#line` directive, so a compile error still names a real line in a real
file.

**Why the compute path needs an extension.** The tracer takes screen-space
derivatives, and not for anything exotic: the starfield sizes its anti-aliasing
filter with `fwidth()` on every ray of every frame, because lensing stretches the
sky so violently near the shadow that a fixed filter width is unusable. Core
OpenGL gives a compute stage no derivatives at all, so the port requires
`NV_compute_shader_derivatives`. Without it the starfield would have to be
rewritten and this would stop being a port, so the compute path is simply not
offered and the fragment path is unaffected.

Derivatives need 2×2 quads of invocations. `derivative_group_quadsNV` builds them
from the local invocation's x and y, exactly as the fragment stage does, which is
why 8×8 and 16×16 reproduce the fragment footprint and render **identically to
each other**. A workgroup with an odd dimension cannot form those quads and falls
back to `derivative_group_linearNV`, which pairs up four *consecutive*
invocations instead — still a filter width, but not the same one.

`--set compute=1` switches paths on a running build; `--set compute-group-x/-y`
picks the workgroup, applied when the shader is compiled. Measured at 1280×720,
96 samples, minimum of three runs:

| | wide | closeup | edge-on | kerr | ultra |
| --- | --- | --- | --- | --- | --- |
| fragment, ms | 4.206 | 10.861 | 13.809 | 14.239 | 23.187 |
| compute 8×8 | 1.07× | 1.06× | 1.03× | 1.17× | 1.09× |
| compute 16×16 | 1.09× | 1.05× | 1.04× | 1.14× | 1.09× |
| compute 32×1 | 0.91× | 0.82× | 0.91× | 0.83× | 0.88× |

8×8 and 16×16 are within noise of each other and a little ahead of the fragment
path. 32×1 is 9–18% *behind* it, which is what a thin strip of pixels should do:
a warp then covers 32 pixels in a row rather than an 8×4 tile, and neighbouring
rays in a row diverge more than neighbouring rays in a square. The gap is widest
on `closeup`, where divergence is worst.

Against the reference images the compute path lands at RMSE 0.000088–0.000191,
worst channel 6–12 levels out of 255: the arithmetic is identical but its order
is not, and half-float rounding differs accordingly. 32×1 is a different image
(RMSE 0.026–0.061) because of the derivative footprint, not the port.

### A third scheduling: wavefront, and why it is not the default

The premise is sound and the numbers are not. Neighbouring rays in this scene
differ by an order of magnitude in how many steps they need -- a ray that sails
past the hole finishes in a few dozen, one grazing the photon sphere takes many
hundreds -- and a warp runs until its slowest lane is done. The wavefront path
attacks that directly: every ray is advanced by a fixed chunk of steps, parked in
an SSBO, and the survivors compacted into a dense list before the next chunk, so
a lane is never held by a ray that finished long ago.

It is implemented, it is correct, and it renders **byte-identically to the
compute path** on all five scenes. It is also the slowest of the three, on every
scene:

| ms, min of three runs | wide | closeup | edge-on | kerr | ultra |
| --- | --- | --- | --- | --- | --- |
| fragment | 4.142 | 10.738 | 13.258 | 13.831 | 22.770 |
| compute | 3.666 | 9.651 | 12.527 | 11.453 | 19.836 |
| wavefront | 4.950 | 11.725 | 14.501 | 14.353 | 23.699 |

Two measurements say why, and they separate the two halves of the scheme cleanly.
Running the wavefront path with a chunk larger than the whole step budget keeps
every buffer and every pass but performs no compaction at all; on `closeup` that
costs **+1.92 ms over compute, a flat 20% tax** for nothing but routing the ray
state through memory. Then shrinking the chunk to 32, which is where compaction
does the most work, adds a further **2.56 ms** rather than giving any of it back.

So the compaction is not merely failing to pay for its traffic. It is negative at
every chunk size tried, and the chunk sweep is monotonic -- 16, 32, 64, 128, 256,
512 steps improve in that order, with the optimum at "do not chunk". Ninety-six
bytes of ray state read and written per live ray per chunk is simply worth more
than the lanes the compaction recovers, on a laptop part where bandwidth is the
scarce resource. The disk and jet are integrated along the path, so the state
cannot shrink much: sixteen of its twenty-four floats are load-bearing.

It stays in the tree because the result is worth keeping and the switch costs
nothing -- `--set tracer=wavefront`, with `--set wavefront-chunk` and
`--set wavefront-threshold` to re-run the sweep on other hardware, where a part
with more bandwidth per FLOP could plausibly come out the other way.

---

## Measuring the renderer

Frame rate cannot answer "did that change make the tracing faster?". The frame
also contains vsync, the accumulation pass, the bloom pyramid, tone mapping and
the ImGui panel, and none of those respond to a ray-marching setting. Worse, a
CPU stopwatch around the draw call measures *submission*: the driver queues the
work and returns long before the GPU has run it.

So [`GpuTimer`](../src/Renderer/GpuTimer.hpp) brackets the `black_hole` pass in a
`GL_TIME_ELAPSED` query. Reading such a query back in the frame that issued it
would block until the GPU caught up — serialising the pipeline and inflating the
very number being measured — so a ring of eight query objects is cycled instead,
and each result is collected a few frames later once the driver reports it
ready. The timer only observes; it issues no draw and changes no pixel.

The collected window reports a **median** and a **p95** rather than a mean. The
first frame of a run pays for pipeline setup, and any frame can be interrupted
by the compositor; a mean folds those in, while the median ignores them and the
p95 says how bad the bad frames actually are. Both are nearest-rank, so each
figure is a timing that genuinely occurred.

* **Interactive** — shown in the control panel under the FPS line, over a
  rolling window of recent traced frames. The window restarts on a resize or a
  shader reload, since older timings then describe code or a resolution that is
  no longer in play.
* **`--shot`** — one machine-readable line on stdout after the image is written,
  covering the whole run:

  ```
  BHS_TIMING pass=black_hole frames=96 median_ms=8.593 p95_ms=10.451 min_ms=7.889 max_ms=10.493 total_ms=848.603 width=1280 height=720 samples=96
  ```

  Every duration is milliseconds in the C locale, so the decimal separator never
  depends on the machine the run happened on.

### The benchmark

[`scripts/bench.ps1`](../scripts/bench.ps1) runs five fixed scenes through
`--shot` and tabulates those timings. The scenes are chosen so the ray marcher
diverges differently in each: `wide` lets most rays escape to the background,
`closeup` puts many of them into near-critical orbits at the photon sphere,
`edge-on` sends long paths lengthwise through the disk, `kerr` takes the general
five-ODE solver instead of the planar reduction, and `ultra` raises the step
budget and traces two rays per pixel per frame. An optimisation that helps one
of those can easily hurt another, and a single representative scene would hide
it.

It doubles as an image-regression check. Every scene renders to `bench/current/`
and is compared against the committed baseline in `bench/reference/`, reporting
RMSE and the largest single-channel deviation, and exiting non-zero when RMSE
crosses `-Threshold` (0.002 by default, about half an 8-bit step spread over the
whole image). So a change that was meant to be a pure speedup can be shown to be
one.

That check rests on the renderer being reproducible: with `--time 0` and a fixed
`--samples`, the disk pattern is a function of the animation clock alone and the
sub-pixel jitter is a function of the sample index alone, so two runs produce
identical bytes. `-Determinism` renders every scene twice and compares hashes,
which is worth doing before trusting any RMSE — if it ever fails, the numbers
are measuring run-to-run noise rather than the change under test.

---

## Build

CMake fetches GLFW, GLM, GLAD 2 and Dear ImGui at configure time; nothing is
vendored. GLAD generates its OpenGL 4.6 core loader with a Jinja2 install placed
in a **build-local virtual environment**, never the user's global Python.

Shaders are copied next to the executable by a custom target with
`CONFIGURE_DEPENDS` globbing, so editing GLSL and rebuilding refreshes them —
and **Reload GLSL shaders** in the UI re-reads them without restarting. That
reload compiles every program before publishing any of them, so a syntax error
leaves the previous working set intact instead of blanking the screen.

[`build.ps1`](../build.ps1) locates Visual Studio via `vswhere`, imports the x64
developer environment into the PowerShell session, then configures and builds —
so it works from any shell, not just a Developer prompt.

### Header dependencies are declared explicitly

`CMakeLists.txt` sets `OBJECT_DEPENDS` so every object depends on every project
header. That looks heavy-handed, and it is deliberate.

With the Ninja generator CMake discovers header dependencies by parsing
cl.exe's `/showIncludes` output. On a **localized** Visual Studio those lines
are translated, and the encoding CMake records does not reliably match the code
page cl.exe writes — so Ninja records only *some* of the dependencies. The
failure is silent and genuinely dangerous: editing a field in a header rebuilds
some translation units but not others, and the link then succeeds against
objects that disagree about that struct's layout. It surfaces as parameters
reading back as nonsense, which is exactly how it was found here.

The project has eight source files and builds in seconds, so the blunt fix is
the right one. Touching any header rebuilds everything and the result is always
consistent.

---

## Design notes worth knowing

**Why a fragment shader and not compute?** Every ray is independent and writes
exactly one pixel. A compute shader would add plumbing and buy nothing. The
integration loop is the cost, and it is identical either way.

**Why the parameter struct is the only interface.** `ControlPanel` never touches
GL, and the renderer never queries ImGui. That is what lets `--shot` reuse the
entire pipeline with no UI at all, and what makes `--set name=value` a
one-line-per-parameter table.

**Why the step schedule is bounded rather than fully adaptive.** Neighbouring
pixels then do predictable amounts of work, which matters a great deal for GPU
warp coherence, and there is no temporal popping as the camera moves. See
[PHYSICS.md §2.4](PHYSICS.md#24-integration--numerical).

**Why there are two geodesic solvers.** Kerr reduces exactly to Schwarzschild at
`a* = 0`, so one solver would be enough — but the planar Schwarzschild reduction
integrates one ODE instead of five and runs 2–3× faster. The shader branches on
`abs(uSpin) > 1e-4`, so the common non-rotating case pays nothing for a feature
it does not use. The frequency-shift formula is deliberately *not* duplicated:
one covariant expression serves both.

**Where the cost actually goes.** At 1100×620 with the High preset the RTX 5070
traces roughly 200 frames per second, so a 96-sample refined still takes about a
third of a second. Most rays terminate early; the expensive ones are the
near-critical rays circling the photon sphere, which is exactly what debug view
2 shows.
