# Controls

The camera is driven from the keyboard, so **the mouse pointer stays free for
the control panel** — you can drag sliders at any time without switching modes
first. A translucent key reference sits in the bottom-right corner; press **H**
to hide it.

## Camera — Orbit mode (the default)

| Input | Action |
| --- | --- |
| **Arrow keys** *or* **W A S D** | Orbit around the black hole. Both do the same thing, so either hand works. |
| **+ / −** (main row or numpad) | Move closer / further. |
| **E / Q** | The same as + / −. |
| **Left mouse drag** | Orbit, when the drag starts on the render rather than on the panel. |
| **Scroll wheel** | Zoom, unless the pointer is over the panel. |

## Camera — Free flight (press **M**)

| Input | Action |
| --- | --- |
| **W A S D** | Fly forward / back / strafe. |
| **Arrow keys** | Look around. |
| **Q / E** | Down / up. |
| **+ / −** | Narrow / widen the field of view. |

## Everything else

| Input | Action |
| --- | --- |
| **M** | Toggle Orbit ↔ Free flight. |
| **R** | Reset the camera. |
| **H** | Show / hide the help overlay. |
| **F1** | Show / hide the control panel. |
| **F2** | Pause / resume the animation clock. |
| **F5** | Save a PNG screenshot next to the executable. |
| **Tab** | Capture the mouse for first-person style look. Press again to release. |
| **Esc** | Quit. |

Camera keys are ignored while an ImGui widget is actively being dragged or
typed into, so adjusting a slider never moves the camera at the same time.

The help overlay is drawn with a Cyrillic-capable system font (Segoe UI, with
Tahoma and Arial as fallbacks) and is written in Russian. If none of those fonts
can be loaded it falls back to English rather than drawing empty boxes.

> **Note on refinement.** While the camera holds still the image keeps refining
> and gets visibly cleaner. Any movement or slider change restarts it — that is
> intended, not a glitch. The progress bar at the top of the panel shows where
> you are.

---

## Control panel

### Quality

| Control | What it does |
| --- | --- |
| **Preset** | Low / Medium / High / Ultra. Changes only *how finely* the physics is resolved — integration budget, step size, resolution scale, sampling. The physical model never changes. |
| **Render scale** | Internal resolution relative to the window. Above 1.0 is supersampling. |
| **Progressive refinement** | Average successive jittered frames while nothing moves. Leave this on. |
| **Refinement budget** | How many samples to accumulate before the image is considered converged. |
| **Freeze animation while refining** | Holds the disk pattern still so the average stays coherent. Turn off if you want continuous motion at lower quality. |
| **Rays per pixel per frame** | Extra jittered rays every frame. Costs proportionally; improves quality *while moving*. |

Preset values:

| Preset | Max RK4 steps | dφ | Render scale | Rays/frame |
| --- | --- | --- | --- | --- |
| Low | 180 | 0.085 | 0.65 | 1 |
| Medium | 280 | 0.058 | 0.85 | 1 |
| High | 420 | 0.040 | 1.00 | 1 |
| Ultra | 820 | 0.024 | 1.00 | 2 |

### Black hole

| Control | What it does |
| --- | --- |
| **Schwarzschild radius** | `r_s = 2GM/c²`. Every length in the scene is measured against it, so this is effectively the mass. |
| **Spin a\*** | Dimensionless spin `Jc/GM²`, from -1 (maximally retrograde) through 0 (Schwarzschild) to +1. Any non-zero value switches to the full Kerr geodesic solver, which costs about 1.5–2× more. Watch the shadow go lopsided. |
| **Outline captured rays** | Draws an edge around the set of rays that crossed the horizon — the true shadow boundary. |
| **Highlight photon sphere grazes** | Lights up rays whose closest approach was `1.5 r_s`. |

The panel prints the horizon, photon sphere, ISCO and apparent shadow radius for
the current `r_s`. Those are *labels*: the renderer does not draw any of them.

### Ray integration

| Control | What it does |
| --- | --- |
| **Max RK4 steps** | Budget per ray. Rays near the critical impact parameter wind many times around the photon sphere; too small a budget makes that ring unstable. Debug view 3 paints exhausted rays magenta. |
| **Angular step dφ** | Base Runge–Kutta step in the orbital angle. The shader shrinks it automatically in high curvature and near the disk. |
| **Escape radius** | Where remaining curvature is treated as negligible and the ray direction is handed to the starfield. |

The line below shows how much total angle your budget buys — useful when
hunting magenta pixels.

### Accretion disk

| Control | What it does |
| --- | --- |
| **Lock inner edge to the ISCO** | A thin disk normally ends at `r = 3 r_s`. Unlocking is an artistic override; inside the ISCO the orbital-velocity model is extrapolated, not physical. |
| **Outer radius** | Where the disk ends. Keep the camera outside it for a clean composition. |
| **Scale height H** | Standard deviation of the vertical Gaussian. Small values give the razor-thin cinematic look. |
| **Flare exponent** | `H(r) = H₀ (r/r_in)^q`. Real thin disks flare outwards; `q = 0` is a flat slab. |
| **Brightness** | Overall emission scale. |
| **Peak temperature** | Temperature at the hottest radius. Everything else follows the thin-disk law `T(r) = T_peak (F(r)/F_peak)^(1/4)`, so the inner disk really is bluer. |
| **Density** | Scales both emission and opacity. |
| **Opacity** | Extinction per unit length. Near zero the disk glows through itself; large values converge to an opaque disk that hides what is behind it. |
| **Turbulence** | Amount of procedural structure. |
| **Pattern rotation rate** | How fast the pattern is advected. The *radial* dependence is the real Keplerian law `Ω ∝ r^-3/2`, so the inner disk shears past the outer disk correctly. |
| **Prograde / Retrograde** | Flips the orbital direction — and with it, which side is Doppler-brightened. |

### Accretion

| Control | What it does |
| --- | --- |
| **Inflow rate** | Radial drift as a fraction of the orbital speed. Without it the turbulence circles forever instead of spiralling in. Feeds both the pattern motion and the Doppler shift. |
| **Plunging region** | How far inside the ISCO the infalling gas keeps radiating, as a fraction of the gap to the horizon. Zero gives the classical sharp-edged thin disk. |

### Relativistic jet

| Control | What it does |
| --- | --- |
| **Power** | Overall brightness of the outflow. Zero removes it. |
| **Powered by the spin** | Blandford–Znajek `a*²` scaling. With it on, a non-rotating hole has no jet — there is no rotational energy to extract. |
| **Bulk Lorentz factor** | How fast the flow streams. This is what makes one jet bright and the counter-jet nearly invisible. |
| **Length / Base radius / Collimation** | Geometry. Width grows as `height^collimation`; real jets are parabolic (`r ~ z^0.6`), not conical. |
| **Colour temperature** | Tint only — synchrotron emission is not thermal. |
| **Knots / turbulence** | Blobby structure, advected outwards with the flow. |

### Relativistic optics

Each slider fades an effect out towards 1.0 (no shift) so it can be studied in
isolation. At 1.0 the full physical value is used.

| Control | What it does |
| --- | --- |
| **Doppler shift** | `δ = 1/(γ(1 - β·n̂))`, using the local orbital velocity and the *lensed* photon direction. At the ISCO the orbital speed is 0.5 c. |
| **Gravitational redshift** | `sqrt(1 - r_s/r)` for an emitter at radius `r`. |
| **Relativistic beaming** | The `g⁴` intensity factor that follows from `I_ν/ν³` being invariant along a null geodesic. Not an artistic term — for thermal emission it is exactly `σ(gT)⁴`. |

**Try this:** set Doppler to 0, look at the disk, then bring it back to 1. The
approaching side jumps to 2.3–4× the brightness of the receding side, and the
colours split cool/warm. Then switch to Retrograde and watch it invert.

### Visual / HDR

| Control | What it does |
| --- | --- |
| **Star density** | Number of procedural stars. |
| **Nebula strength** | Brightness of the galactic band. |
| **Exposure** | Linear multiplier before tone mapping. |
| **Bloom strength** | How much glow is composited back in. |
| **Bloom threshold / knee** | Which radiance levels bloom, and how softly it starts. |
| **Bloom spread** | Widens the tent filter for a softer falloff. |
| **Bloom level blend** | How strongly each coarser pyramid level mixes into the next. Higher pushes energy into the widest halo. |
| **Bloom levels** | Levels in the pyramid. More means a wider, softer glow. |
| **Tone mapper** | ACES fitted / Reinhard / Uncharted 2 filmic. |
| **V-sync** | Cap to the display refresh rate. |

### Camera

Mode, field of view, movement speed, orbit radius and **inclination**.
Inclination 0° is exactly edge-on to the disk — the most dramatic view, where
the far side of the disk appears bent up over the top of the black hole and down
beneath it.

Four one-click shots: **Edge-on**, **Cinematic**, **High angle**, **Close pass**.

### Debug visualisation

| Mode | Shows |
| --- | --- |
| **0 Final render** | The full HDR image. |
| **1 Lensed background only** | The disk is skipped, so only lensing of the sky remains. |
| **2 Ray step count** | How many RK4 steps each ray needed. The bright ring is the photon sphere. |
| **3 Escape / capture** | Red = crossed the horizon, blue = escaped, yellow = crossed the disk, **magenta = ran out of integration budget**. |
| **4 Disk optical depth** | Accumulated extinction, `1 - exp(-τ)`. |
| **5 Combined shift g** | `g = g_gravity · δ`. Red is redshifted, blue is blueshifted. |
| **6 Doppler only** | The Doppler factor with gravitational redshift removed. |
| **7 Closest approach** | Log-scaled closest approach, with bands at the photon sphere and the ISCO. |
| **8 Background only** | The unobstructed lensed starfield. |
| **9 Emission only** | The disk and jet emission alone, sky removed. |

Debug modes only change how the trace result is displayed. The integrator is
untouched, so what you measure in a debug view is what produced the final image.

---

## Command line

Run with no arguments for the interactive renderer. `--help` prints the full
list.

Render a single image and exit:

```bash
BlackHoleSandbox.exe --shot render.png --width 1920 --height 1080 --samples 200 --quality ultra
```

| Option | Meaning |
| --- | --- |
| `--shot <file.png>` | Render one image and exit. |
| `--samples N` | Refinement frames before readback (default 96). |
| `--with-ui` | Draw the control panel and help overlay into the captured image. |
| `--width N` / `--height N` | Output resolution. |
| `--distance R` | Orbit radius in scene units. |
| `--yaw D` / `--pitch D` | Orbit angles in degrees. Pitch 0 is edge-on. |
| `--fov D` | Vertical field of view. |
| `--time T` | Animation clock value for the disk pattern. |
| `--quality low\|medium\|high\|ultra` | Quality preset. |
| `--debug N` | Debug view 0–9. |
| `--set name=value` | Any renderer parameter. Repeatable. |

`--set` accepts: `rs`, `spin`, `ray-step`, `max-steps`, `escape-radius`, `disk-inner`,
`disk-outer`, `disk-thickness`, `disk-flare`, `disk-brightness`,
`disk-temperature`, `disk-density`, `disk-opacity`, `disk-turbulence`,
`disk-direction`, `orbit-speed`, `jet-power`, `jet-spin-scaling`,
`jet-length`, `jet-radius`, `jet-collimation`, `jet-lorentz`,
`jet-temperature`, `jet-turbulence`, `plunge`, `accretion-rate`, `doppler`, `gravitational-shift`, `beaming`,
`star-density`, `nebula`, `spp`, `exposure`, `bloom`, `bloom-threshold`,
`bloom-knee`, `bloom-scale`, `bloom-blend`, `bloom-levels`, `tone-mapper`,
`render-scale`, `lock-isco`, `horizon-guide`, `photon-guide`.

Isolate the Doppler effect from the command line:

```bash
BlackHoleSandbox.exe --shot no_doppler.png --pitch 1.8 --set doppler=0 --set beaming=0
```

Render every viewpoint and debug view at once:

```powershell
./tools/render_sheet.ps1 -Samples 96 -Width 1600 -Height 900
```
