# Black Hole Sandbox

An interactive C++20 / OpenGL 4.6 renderer of a **Schwarzschild or Kerr**
(non-rotating or rotating, electrically neutral) black hole.

Every camera ray is a numerically integrated null geodesic. The shadow, the
photon ring, the Einstein rings in the starfield and the multiple images of the
accretion disk are **not drawn** — they emerge from the integration. There is no
UV distortion, no screen-space swirl and no radial warp anywhere in the project.

Start with [docs/PHYSICS.md](docs/PHYSICS.md) before treating any visual feature
as a scientific result. It labels every effect as physically derived, a
numerical approximation, or frankly artistic, and it lists what the renderer
gets wrong.

## Build — Windows 11, Visual Studio 2026, MSVC

```powershell
./build.ps1
```

That locates Visual Studio with `vswhere`, imports the x64 developer
environment, configures and builds — from any shell, not just a Developer
prompt. `./build.ps1 -Run` launches the result; `-Clean` wipes the build
directory first.

If you prefer to drive CMake yourself, from a **Developer PowerShell for VS
2026**:

```powershell
cmake --preset ninja-msvc
cmake --build --preset build-ninja-msvc
./out/build/ninja-msvc/bin/BlackHoleSandbox.exe
```

The first configuration downloads GLFW, GLM, GLAD and Dear ImGui automatically.
GLAD's small Jinja2 generator dependency is installed into an isolated folder
beneath the build directory, never into your global Python. The `vs2026-x64`
preset produces a Visual Studio solution instead.

## Run

Launch with no arguments for the interactive renderer. The camera runs on the
keyboard -- **arrow keys** or **WASD** to orbit, **+**/**-** to zoom -- so the
mouse pointer stays free for the control panel. A translucent key reference sits
in the bottom-right corner; **H** hides it, **F1** hides the panel, **F5** saves
a screenshot.

While the camera holds still the image progressively refines and gets visibly
cleaner. Full controls are in [docs/CONTROLS.md](docs/CONTROLS.md).

Render a still without opening a window:

```powershell
./out/build/ninja-msvc/bin/BlackHoleSandbox.exe --shot render.png --width 1920 --height 1080 --samples 200 --quality ultra
```

Or render every viewpoint and debug view at once:

```powershell
./tools/render_sheet.ps1
```

## Benchmarking

Each `--shot` prints how long the ray-tracing pass took on the GPU, measured
with a `GL_TIME_ELAPSED` query rather than inferred from the frame rate. The
interactive panel shows the same figures live.

```powershell
./scripts/bench.ps1
```

That times five scenes chosen to stress the ray marcher in different ways, then
compares each rendered image against the committed baseline in `bench/reference/`
and exits non-zero if any of them has drifted. `-UpdateReference` adopts the
current images as the new baseline; `-Determinism` proves each scene still
renders to identical bytes twice in a row. Details in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#measuring-the-renderer).

## What is implemented

| | |
| --- | --- |
| Light bending (no spin) | RK4 integration of the null geodesic Binet equation `u'' = 3/2 u² - u` |
| Light bending (spinning) | Full Kerr: RK4 on Hamilton's equations in Kerr–Schild coordinates, regular on the axis and at the horizon |
| Rotation | Spin `a*` from -0.998 to +0.998: frame dragging, lopsided shadow, spin-dependent ISCO and horizon |
| Event horizon & shadow | Emergent: apparent radius `3√3/2 r_s`, never drawn |
| Photon sphere | Emergent: visible as a step-count spike at `1.5 r_s` |
| Accretion disk | Volumetric emission/absorption transfer along the curved path |
| Disk emissivity | Novikov–Thorne zero-torque thin-disk profile, `T(r) ∝ F(r)^(1/4)` |
| Doppler shift | `δ = 1/(γ(1 - β·n̂))` in the locally non-rotating frame, using the *lensed* photon direction |
| Gravitational redshift | The Kerr lapse `α = sqrt(r²Δ/A)`, reducing to `sqrt(1 - r_s/r)` without spin |
| Frame dragging | The ZAMO velocity subtracts the dragging rate `ω = 2aMr/A`, so only motion the local observer can see counts |
| Relativistic beaming | `I_obs = g⁴ I_emit`, self-consistent with the temperature shift |
| Anti-aliasing | Progressive refinement over Halton-jittered sub-pixel samples |
| Post-processing | Multi-level bloom pyramid, ACES / Reinhard / Uncharted 2, dither |
| Debug views | Ten, covering classification, step count, optical depth and both frequency shifts |

Not implemented, and deliberately so: light travel-time delay, returning
radiation, polarisation, spectral rendering, the plunging region.
See [docs/PHYSICS.md §6](docs/PHYSICS.md#6-what-this-renderer-does-not-do).

Architecture and render-pass details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
