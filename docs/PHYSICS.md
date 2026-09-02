# Physics of the Black Hole Sandbox renderer

This document states exactly what the renderer computes, which parts follow
from general relativity, which parts are numerical approximations, and which
parts are frankly artistic. Nothing here is decoration: if you find a claim in
this file, you can find the line of GLSL that implements it, and the debug
views let you check it yourself.

Every effect is tagged with one of:

| Tag | Meaning |
| --- | --- |
| **DERIVED** | Follows from the Schwarzschild solution with no fudge factors. |
| **NUMERICAL** | A derived quantity, computed with a finite-precision approximation whose error is described. |
| **ARTISTIC** | Chosen because it looks right, not because it is a physical result. |

The whole ray tracer lives in [`shaders/black_hole.frag`](../shaders/black_hole.frag).

---

## 1. The spacetime

The renderer implements **two** spacetimes, selected by the **Spin a\*** slider:

* `a* = 0` -- **Schwarzschild**: non-rotating, electrically neutral. Handled by
  a fast solver that exploits spherical symmetry (section 2).
* `a* != 0` -- **Kerr**: a rotating black hole. Spherical symmetry is gone and
  the fast solver no longer applies, so a full 3-D geodesic integrator takes
  over (section 2.5).

Both are vacuum solutions outside the horizon. Starting with the non-rotating
case, its metric in Schwarzschild coordinates is

```
ds² = -(1 - r_s/r) c² dt² + dr²/(1 - r_s/r) + r² (dθ² + sin²θ dφ²)
```

with a single parameter, the **Schwarzschild radius**

```
r_s = 2GM/c²
```

Everything in the scene is measured in units of `r_s` (the
`uSchwarzschildRadius` uniform). That is why the renderer never asks for a
mass: the dimensionless light-bending problem is *identical* for a
stellar-mass and a supermassive black hole. Only the disk's temperature and
timescale would differ, and both of those are user parameters here.

### Landmark radii — **DERIVED**

| Radius | Value | What it is |
| --- | --- | --- |
| Event horizon | `r = r_s` | One-way surface. Rays that reach it are gone. |
| Photon sphere | `r = 1.5 r_s` | Where light can orbit — unstably. |
| ISCO | `r = 3 r_s` | Innermost *stable* circular orbit for matter. Default inner edge of the disk. |
| Shadow radius | `b = 3√3/2 r_s ≈ 2.598 r_s` | The *apparent* size of the black disc to a distant observer. |

These live in
[`src/Physics/BlackHoleParameters.hpp`](../src/Physics/BlackHoleParameters.hpp),
used by the UI to *label* the image. The renderer never draws them. The shadow
in the picture is the set of pixels whose integrated ray crossed `r = r_s`;
that it comes out at an apparent radius of 2.598 `r_s` rather than 1.0 `r_s` is
a result, not an input. Debug view 3 shows exactly that set.

Every one of these radii moves once the hole spins — see §2.5. The panel
recomputes and prints them for the current `a*`, and the "shadow radius" row
disappears there, because a rotating hole's shadow is not a circle and no single
radius describes it.

---

## 2. Light bending: the core of the renderer

### 2.1 Reducing 4-D geodesics to one ODE — **DERIVED**

Schwarzschild spacetime is spherically symmetric, so a photon's trajectory
stays inside the single plane spanned by its starting position and its
direction. That reduces a 4-dimensional geodesic problem to finding `r(φ)`
in one plane.

Writing the dimensionless inverse radius

```
u = r_s / r          (u = 0 at infinity, u = 1 at the horizon)
```

the null geodesic equation becomes the photon **Binet equation**:

```
d²u/dφ²  =  (3/2) u²  -  u                                          (1)
```

Read equation (1) carefully, because it is the whole story:

* The `-u` term **on its own** gives `u = A cos(φ - φ₀)`, which in polar
  coordinates is a *straight line*. No gravity, no bending.
* The `(3/2) u²` term is purely general-relativistic. It is the entire source
  of gravitational lensing in this renderer.

The shader integrates (1) as a first-order system with `v = du/dφ`:

```glsl
void geodesicDerivative(float u, float v, out float du, out float dv)
{
    du = v;
    dv = 1.5 * u * u - u;
}
```

That six-line function is where the light bends. There is no UV distortion,
no screen-space swirl, and no radial warp anywhere in this project.

### 2.2 Initial conditions from the impact parameter — **DERIVED**

Integrating (1) needs a starting `(u, v)`. `u` is trivial: `u₀ = r_s / r_cam`.
`v₀` comes from the first integral of (1), the **radial potential**:

```
(du/dφ)²  =  (r_s/b)²  -  u²  +  u³                                 (2)
```

where `b` is the photon's impact parameter — a conserved quantity along the
whole trajectory. For a **static observer** at radius `r` who sees the ray
leave at angle `α` from the radial direction,

```
b = r sin(α) / sqrt(1 - r_s/r)
```

The `1/sqrt(1 - r_s/r)` factor converts the *locally measured* angle into the
globally conserved quantity. Dropping it is a classic subtle error that makes
the shadow come out the wrong size; the shader keeps it:

```glsl
float impactParameter = cameraRadius * tangentLength * inversesqrt(lapse);
float rsOverB = rs / max(impactParameter, 1e-6);
float radialPotential = max(rsOverB * rsOverB - u * u + u * u * u, 0.0);
float v = -sign(radialComponent) * sqrt(radialPotential);
```

The sign is set by whether the ray is heading outwards (`r` growing, so `u`
shrinking, so `v < 0`) or inwards.

### 2.3 Where the shadow comes from — **DERIVED**

Equation (2) also explains the black disc. The right-hand side is a cubic in
`u`; a ray can only turn around where it has a real root. The critical case is

```
b_crit = 3√3/2 · r_s ≈ 2.598 r_s
```

Rays with `b < b_crit` have **no turning point** — nothing in the equation lets
them come back out, so they must cross the horizon. Rays with `b` slightly
above `b_crit` wind many times near `r = 1.5 r_s` before escaping; that is the
**photon ring**, and it is why debug view 2 (step count) shows a sharp bright
circle exactly at the shadow's edge. The renderer does not know that number.
It integrates, and the number appears.

### 2.4 Integration — **NUMERICAL**

Classical fourth-order Runge–Kutta, stepping in the orbital angle `φ`:

```glsl
void integrateGeodesicRk4(inout float u, inout float v, float h) { ... }
```

**Why step in φ and not in an affine parameter?** Because `φ` is monotonic
along the trajectory, the system (1) has no explicit `φ` dependence, and the
angular sweep needed to reach infinity is bounded (`Δφ ≈ b/r` at large `r`).
It makes the problem well-conditioned in exactly the region that matters.

**Local error** is `O(h⁵)` per step; global error `O(h⁴)`. The step is adapted
by two rules:

1. **Curvature.** `h` shrinks by up to 3.6× as `u` grows, because the `(3/2)u²`
   term stiffens near the hole.
2. **Disk sampling.** When the ray is inside the disk's radial range and close
   to its plane, `h` is capped so the chord length `≈ r·h` stays small compared
   with the disk's scale height. Without this, a coarse step jumps straight
   over a thin disk.

This is a *bounded, deterministic* schedule rather than true error-controlled
adaptive stepping. That is a deliberate real-time choice: neighbouring pixels
then do predictable amounts of work, which matters enormously for GPU warp
coherence, and there is no temporal popping when the camera moves.

**Known limitations:**

* Rays extremely close to `b_crit` need unbounded winding. When the step budget
  runs out the shader extrapolates from the sign of `v` (still falling in ⇒
  captured, else escaped) rather than flickering. Debug view 3 paints any ray
  that reaches the budget in magenta; at the default settings there are none in
  a typical frame, but you can force them by lowering **Max RK4 steps**.
* "Escape" is declared at a *finite* radius (`uEscapeRadius`, default 90 `r_s`),
  not at infinity. The residual deflection beyond that point is `O(r_s/r)`,
  i.e. about 1% of the total, and it is neglected.
* The integration is in the *orbit plane only*. This is exact for
  Schwarzschild, and exactly why it cannot be reused once the hole spins — see
  the next section.

### 2.5 Rotation: the Kerr solver — **DERIVED**

Everything above rests on spherical symmetry. A rotating black hole does not
have it: frame dragging twists a ray out of any plane you try to confine it to,
so the trajectory genuinely needs three spatial degrees of freedom. Setting the
**Spin a\*** slider away from zero switches the renderer to a separate solver.

**Why Kerr–Schild and not Boyer–Lindquist.** Boyer–Lindquist is the textbook
chart for Kerr and the obvious first choice. It was in fact the first
implementation here, and it has two coordinate pathologies that both bite:

* **The spin axis.** The equations carry `L²/sin²θ` and `cot θ` terms that
  diverge at `θ = 0` and `π`. The *spacetime* is perfectly smooth there — only
  the chart is broken — but the integrator does not know that, and rays passing
  near the axis come out inaccurate. It showed up as a thin bright seam running
  straight along the axis of every rotating render.
* **The horizon**, where `Δ = 0` and the radial equation degenerates.

Kerr–Schild has neither, which is why production general-relativistic ray
tracers use it. Switching charts removed the seam completely.

**The metric.** Kerr–Schild writes the metric as flat spacetime plus a rank-one
correction built from a single null vector:

```
g_μν = η_μν + f l_μ l_ν
g^μν = η^μν - f l^μ l^ν
```

with `η = diag(-1, 1, 1, 1)` and

```
f    = 2 M r³ / (r⁴ + a² z²)
l_μ  = ( 1, (r x + a y)/(r²+a²), (r y - a x)/(r²+a²), z/r )
```

Two things make this pleasant to work with. The inverse metric needs no matrix
inversion — it is the same expression with the sign of `f` flipped, because
`l_μ` is null with respect to *both* `η` and `g`. And the coordinates are
ordinary Cartesian-like ones, so no Jacobian is needed to turn the integrated
velocity back into a world-space ray direction.

Here `r` is not the Euclidean radius. It is the Boyer–Lindquist radial
coordinate, defined implicitly by

```
(x² + y²)/(r² + a²) + z²/r² = 1
```

whose surfaces of constant `r` are oblate spheroids. A convenient identity
keeps the algebra short — writing `ρ² = x²+y²+z²`,

```
W = 2r² - ρ² + a² = r² + a² z²/r² = (r⁴ + a² z²)/r²
```

so that `f = 2Mr/W`, with `W > 0` everywhere. The same `W` appears in every
derivative of `r`, which is what makes the analytic gradients tractable:

```
∂r/∂x = r x / W      ∂r/∂y = r y / W      ∂r/∂z = z (r²+a²) / (r W)
```

**Equations of motion.** Hamilton's equations for `2H = g^μν p_μ p_ν = 0`. The
metric is independent of `t`, so `p_t = -E` is conserved and only the three
spatial positions and momenta evolve. Writing `S = E + l·p`:

```
dxⁱ/dλ = pᵢ - f S lᵢ
dpᵢ/dλ = ½ S² ∂f/∂xⁱ + f S (∂l_j/∂xⁱ) p_j
```

Both right-hand sides are smooth on the axis and across the horizon. The
gradients of `f` and `l` are worked out **analytically** in
`kerrSchildDerivatives`; a central finite difference in 32-bit floats loses
roughly three digits, and that error shows up as shimmering exactly where the
image is most interesting.

**Initial conditions.** The camera is the **normal (Eulerian) observer** of the
3+1 split — the Kerr–Schild counterpart of a ZAMO, and an observer that stays
well defined inside the ergosphere where nothing can remain static. Its
four-velocity is

```
u^μ = sqrt(1+f) ( 1, -f l / (1+f) )
```

and vectors orthogonal to it are exactly those with no time component, which
makes the construction short: normalise the viewing direction under the spatial
metric `g_ij = δ_ij + f lᵢ l_j`, add it to `u`, lower the indices, and rescale
so `E = 1`.

**What you can see.** All of this is checkable in the debug views:

* The **shadow stops being a circle.** Viewed edge-on at `a* = 0.998` it is
  visibly displaced sideways and flattened on one side — the classic D-shape.
  Reversing the spin mirrors it exactly. (Debug view 3 with the disk removed.)
* The **direction of the offset is derivable.** Retrograde photons have a much
  larger critical impact parameter than prograde ones — at `a = M` it is `7M`
  against `2M` — and the side of the sky a camera sees retrograde photons from
  is the side the shadow extends towards. That was how the sign convention was
  checked here, and it caught a real error: the Boyer–Lindquist implementation
  had the spin sense inverted, which only became visible once the two charts
  could be compared.
* The **ISCO moves inwards** for a prograde disk, from `6M` at `a* = 0` to about
  `1.24M` at `a* = 0.998`, so the inner disk runs hotter and faster — and, being
  deeper in the well, also more strongly redshifted. At very high spin the disk
  visibly *dims* overall even as the near side brightens.
* The **horizon shrinks** from `2M` to `M`.

**Known limitations of the Kerr path:**

* The step is taken in the affine parameter with a bounded adaptive schedule
  (never more than a fraction of the distance remaining to the horizon), and the
  budget is doubled relative to the Schwarzschild solver.
* Cost is roughly 1.5–2× the Schwarzschild path: about 117 fps at 1600×900 and
  `a* = 0.998` against 184 fps at `a* = 0` on an RTX 5070 laptop GPU.
* Spin is a property of the *hole*. The disk's own orbital direction is a
  separate control, so a retrograde disk around a prograde hole is possible and
  is handled correctly — its ISCO is pushed outwards instead of inwards.
* Rays are terminated at the horizon. Kerr–Schild would happily integrate
  through it, but there is nothing to render inside.

---

## 3. The accretion disk

### 3.1 Geometry — **ARTISTIC**, physically motivated

A Shakura–Sunyaev style thin disk. Vertical density is Gaussian about the
equatorial plane with a scale height that flares outwards:

```
ρ(r, z) = Σ(r) · exp( -z² / 2H(r)² ),      H(r) = H₀ (r/r_in)^q
```

Real thin disks do flare (`q ≈ 9/8` in the outer Shakura–Sunyaev region), but
`H₀` and `q` here are UI sliders chosen for looks, and the surface density
`Σ(r) ∝ r^-3/2` is a plausible shape rather than a solution of the disk
equations. Treat the geometry as *inspired by* the thin-disk model, not derived
from it.

### 3.2 Radial emissivity — **DERIVED** (in its thin-disk limit)

The radial brightness profile is the zero-torque thin-disk flux of the
Novikov–Thorne / Shakura–Sunyaev family:

```
F(r) ∝ r⁻³ · (1 - sqrt(r_in / r))
```

This is a genuine result: the `(1 - sqrt(r_in/r))` factor comes from imposing
zero viscous torque at the inner edge, which is why the disk fades to nothing
exactly at `r_in` instead of ending on a hard rim. The profile peaks at
`r = (49/36) r_in ≈ 1.361 r_in`, where the bracket equals `0.0568`; the shader's
`17.6` is just `1/0.0568`, normalising the peak to 1.

The local **emitted temperature** then follows Stefan–Boltzmann:

```
T(r) = T_peak · ( F(r) / F_peak )^(1/4)
```

so the inner disk really is hotter and bluer than the outer disk — you can see
that directly in debug view 9. `T_peak` is a slider. A real disk around a
stellar-mass black hole peaks in the X-ray, which is invisible; picking a
temperature in the few-thousand-kelvin range is an honest admission that we are
rendering something the human eye could actually see.

### 3.3 Turbulence — **ARTISTIC**

Procedural 3-D value noise, sampled in a co-rotating frame. Two details are
physically motivated:

* The azimuth is un-wound by the **Keplerian angular velocity** `Ω(r) ∝ r^-3/2`,
  so the pattern orbits with the gas and the inner disk visibly shears past the
  outer disk.
* The spiral lanes use `sin(k·log r - m·θ)`, which is *exactly* a logarithmic
  spiral with `m` arms.

The turbulence perturbs both density **and** temperature. The temperature part
is not cosmetic padding — it is required. Where the disk is optically thick the
observed radiance tends to the source function `j/κ`, and since emission and
extinction are both proportional to density, **density cancels out completely**.
Modulating density alone gives a perfectly smooth, featureless disk. Since
thermal brightness goes as `T⁴`, a ±20% temperature ripple is a factor of two in
brightness. Physically it stands in for patchy turbulent dissipation: the parts
of the flow doing more viscous work run hotter.

### 3.4 Radiative transfer — **NUMERICAL**

The disk is not a surface. Along each integrated trajectory the shader solves
the emission/absorption transfer equation:

```
radiance      += transmittance · emission · ds
transmittance *= exp(-extinction · ds)
```

With a large **Opacity** this converges to an opaque thin disk. With a small one
the disk glows through itself, which is what makes the multiple lensed images
layer softly instead of cutting each other off.

Approximations: the chord between two RK4 points is treated as straight (fine,
because the step is small), at most six samples are taken per segment, and the
path length is the *coordinate* length rather than the proper length measured by
a comoving observer. The last one is a real approximation — see §4.3.

---

## 4. Relativistic optics

Debug views 5 and 6 isolate these; the **Relativistic optics** sliders fade each
towards 1.0 so you can watch one at a time.

### 4.1 The local frame — **DERIVED**

Both the Doppler shift and the gravitational redshift are evaluated in the frame
of a **locally non-rotating observer** (a ZAMO), which is the right local frame
in Kerr and degenerates to the ordinary static observer when the hole does not
spin. The total is the product of two pieces:

```
g = alpha · delta
```

* `alpha` is the **lapse** — what this observer's clock does relative to one at
  infinity. On the equator,

  ```
  alpha = sqrt( r² Δ / A ),    Δ = r² - 2Mr + a²,   A = (r²+a²)² - a² Δ
  ```

  which reduces to the familiar `sqrt(1 - r_s/r)` at `a = 0`.

* `delta` is the ordinary special-relativistic Doppler factor,
  `1/(γ(1 - β·n̂))`, for the plasma's velocity **as that observer measures it**.

**Why not the covariant one-liner.** There is a shorter exact form,
`g = 1/[u^t (1 - Ω b_z)]`, using the photon's conserved axial angular momentum.
It was implemented here first and then removed. The problem is that it needs
`b_z` of the photon that actually *reaches* the camera, while the renderer
traces rays *backwards* — the sign is inverted, and with the correct sign the
denominator can pass through zero at high spin and blow the image out. The local
form is bounded by construction, because `β < 1` always. This is a real
trade-off, not a preference: the covariant form is more elegant, and the local
form is the one that survives contact with a backwards-traced ray.

### 4.2 The orbital velocity — **DERIVED**

For equatorial circular orbits in Kerr (upper sign prograde):

```
Ω = ± sqrt(M) / ( r^(3/2) ± a sqrt(M) )
```

The speed the ZAMO measures is **not** `Ω` times a radius. Frame dragging means
space itself is rotating at

```
ω = 2 a M r / A
```

and only the *difference* is motion the local observer can see:

```
β = (Ω - ω) sqrt(g_φφ) / alpha ,     g_φφ = A / r²  (on the equator)
```

At `a = 0` this collapses to `β = sqrt(M/(r - 2M))`, which at the Schwarzschild
ISCO (`r = 6M = 3 r_s`) gives exactly **β = 0.5** — half the speed of light.
That is why the asymmetry is so violent.

Two consequences worth watching for:

* Raising the spin moves the ISCO inwards, so the inner disk orbits faster *and*
  sits deeper in the potential well. The near side brightens while the disk as a
  whole **dims**, because `alpha` is falling. At `a* = 0.998` the whole image is
  noticeably darker than at `a* = 0` — that is the redshift, not a bug.
* Below the ISCO no circular orbit exists at all, so if you unlock the inner
  radius and drag it inwards, `β` is clamped rather than extrapolating a velocity
  the spacetime does not support.

The viewing direction `n̂` is taken from the *locally integrated photon
direction*, not the straight camera-to-pixel vector. That is what makes the
Doppler pattern follow the lensed image around the black hole rather than being
painted onto the screen — clearly visible in debug view 6, where the blue/red
boundary wraps with the disk's lensed arc.

### 4.3 Intensity — **DERIVED**

For the **intensity**, the invariant is `I_ν / ν³` along a null geodesic.
Integrating over frequency, the bolometric intensity transforms as

```
I_observed = g⁴ · I_emitted
```

For thermal emission this is beautifully self-consistent: a blackbody at
`T_emit` is *seen* as a blackbody at `T_obs = g·T_emit`, and
`σ(g T_emit)⁴ = g⁴ σ T_emit⁴`. **The colour shift and the beaming are two faces
of one transformation**, not two separate effects bolted together. The shader
therefore shifts the temperature and applies `g⁴`; there is no ad-hoc
"brightness boost on the approaching side" anywhere.

The measured result at the default view: the approaching side of the disk comes
out **2.3–4× brighter** than the receding side, and shifted towards blue while
the receding side goes warm. Both fall straight out of the equations above.

**What this gets wrong:**

* We render an RGB proxy for a blackbody, not a spectrum. `blackbodyRgb` is a
  fit to the Planck locus; a real spectral renderer would integrate `B_ν(T)`
  against colour matching functions.
* The transfer integral uses coordinate path length, so the frame dependence of
  `ds` itself is neglected. This is a leading-order treatment: it captures
  beaming and redshift correctly and gets the optically-thin path length
  slightly wrong.
* The **absolute** brightness scale is normalised to the disk's own peak
  temperature so the Brightness slider is decoupled from the Temperature
  slider. That is a units choice. Every *relative* variation across the image —
  the radial profile, the beaming, the redshift — is the physical one.

---

## 5. Accretion and outflow

### 5.1 The plunging region — **DERIVED** in outline, **NUMERICAL** in detail

The zero-torque thin disk of §3.2 ends abruptly at the ISCO, and the classical
picture leaves a sharp hole in the middle. That is not what happens. Inside the
ISCO no stable circular orbit exists, so the gas stops orbiting and falls — but
it does not stop *existing*, and it does not stop radiating. It spirals inwards
over a few orbits, thinning and dimming, until it crosses the horizon.

The renderer continues the disk inwards from the ISCO towards the horizon,
controlled by **Plunging region** (zero restores the sharp edge). Inside it:

* the emissivity carries the ISCO-edge value inwards and then falls off as the
  gas accelerates and its column depth collapses;
* the velocity blends from orbital towards radial free fall, so the Doppler
  shift changes character — the plunging gas is moving mostly *inwards*, not
  around, and is redshifted accordingly.

What is derived: that there are no circular orbits below the ISCO, and that the
infall speed approaches free fall. What is approximated: the free-fall scale
uses the Newtonian `sqrt(r_s/r)`, which is fair well outside the horizon and is
clamped rather than allowed to reach 1; and the emissivity fall-off is a fitted
shape, not a solution of the transfer problem in the plunging flow.

### 5.2 Inflow — **ARTISTIC**, physically anchored

A real thin disk accretes slowly: the radial drift is a small fraction of the
orbital speed, set by how efficiently viscosity transports angular momentum.
The **Inflow rate** slider sets that fraction directly.

It does two things at once, and deliberately the same two: it advects the
turbulence pattern inwards, so features visibly spiral towards the hole instead
of orbiting forever, *and* it adds a radial component to the velocity that the
Doppler calculation sees. The appearance and the physics therefore always agree
— if it looks like it is falling in, it is being Doppler-shifted as though it
were falling in.

The actual value is a slider, not a derivation. Deriving it needs the disk's
viscosity, which needs magnetohydrodynamics.

### 5.3 The relativistic jet

**Where the energy comes from — DERIVED.** The Blandford–Znajek mechanism taps
the *hole's own rotation*: magnetic field lines threading the horizon are wound
up by frame dragging and carry rotational energy away, with power scaling as

```
P_BZ  ~  a*² B² M²
```

The `a*²` is why the jet fades out entirely as the spin goes to zero — a
non-rotating black hole has no rotational energy to extract. That coupling is
switchable in the UI, but leaving it on is the physical behaviour, and it is the
reason the spin slider and the jet slider are related at all.

**Beaming — DERIVED.** The flow streams along the axis at a bulk Lorentz factor
Γ. For a *continuous* jet the observed intensity is boosted by

```
δ^(2 + α)
```

with δ the Doppler factor and α ≈ 0.7 the synchrotron spectral index. The
exponent is `2 + α` rather than the `4` used for the thermal disk because a
steady jet is a standing structure rather than a set of discrete blobs: one
power of δ is lost because the emitting volume is fixed in the observer's frame.

This is the whole reason one jet is blindingly bright and the counter-jet nearly
invisible. A modest difference in viewing angle becomes a large difference in
brightness — exactly the asymmetry seen in M87. Point the camera near the axis
and the near jet saturates while the far one almost vanishes; swing round to
edge-on and they even out.

**Geometry — ARTISTIC.** A parabolic envelope, `radius ~ height^collimation`,
fitted to what jets are observed to look like rather than solved for. The
default exponent of about 0.55 is close to the measured collimation profile of
M87's jet (`r ~ z^0.6`). The limb-brightened shell — emission concentrated
towards the walls rather than filling the cone — is likewise phenomenological,
though it is what makes real jets photograph as two rails rather than a solid
beam. The knots are advected noise.

**Radiation — ARTISTIC.** Synchrotron emission is not thermal, so there is no
temperature to anchor the brightness to the way there is for the disk. The
colour is a tint chosen on the Planck locus, and the overall emissivity is a
units constant picked so a side-on jet is comparable in brightness with the
disk. Every *relative* variation — the beaming asymmetry, the fall-off along the
flow, the `a*²` scaling — is the physical one.

**Optically thin.** The jet adds light but absorbs almost none, which is right
for synchrotron emission at these densities. It still respects transmittance
already accumulated by the disk, because an opaque disk in front genuinely does
hide the outflow behind it.

### 5.4 What "forming an accretion disk" would actually take

The disk here is a *steady* structure with procedural turbulence: it does not
form, and matter does not accumulate into it. Genuinely simulating that means
general-relativistic magnetohydrodynamics — following magnetised fluid through
curved spacetime, resolving the magnetorotational instability that drives the
viscosity in the first place. Codes like HARM and BHAC do it, and a single run
takes days on a cluster. What they produce is then *fed into* a ray tracer very
much like this one.

So the honest split is: this renderer does the radiative transfer and the
geodesics that such a pipeline would do, and substitutes an analytic disk plus
procedural turbulence for the fluid dynamics that it would otherwise import.

---

## 6. Everything else

| Feature | Tag | Note |
| --- | --- | --- |
| Starfield | **ARTISTIC** | Procedural stochastic point sources on an octahedral map, coloured along the Planck locus. Not a star catalogue. |
| Star lensing | **DERIVED** | The stars themselves are invented, but their positions in the image come from the same integrated geodesics, so the arcs, Einstein rings and multiple images are real lensing. |
| Nebula band | **ARTISTIC** | Domain-warped fBm. Pure decoration. |
| Bloom | **ARTISTIC** | Camera/eye artefact, not astrophysics. |
| Tone mapping | **ARTISTIC** | ACES / Reinhard / Uncharted 2. A display transform. |
| Shadow interior | **DERIVED** | Pure black because the ray terminated, not because a black disc is drawn. Foreground disk emission in front of it is correctly still visible. |

---

## 7. What this renderer does **not** do

Stated plainly, so nothing here is oversold:

* **No time delay.** Light from the far side of the disk takes longer to
  arrive, so a truly consistent animation would show the far image lagging.
  The renderer evaluates the disk pattern at a single global time. This is the
  largest remaining gap.
* **No Reissner–Nordström or Kerr–Newman.** The hole is electrically neutral.
* **No self-illumination or returning radiation.** In reality a good fraction
  of the disk's light bends back onto the disk and is re-emitted. Not modelled.
* **No emission below the ISCO**, no jets, no corona, no Comptonisation.
* **No polarisation**, and no wavelength-dependent opacity.
* **Not spectrally rendered** — see §4.3.
* **No magnetohydrodynamics.** The disk does not form and matter does not
  accumulate into it; the turbulence is procedural, not a solved flow. See §5.4.
* **No jet launching physics.** The Blandford-Znajek *scaling* is used, but no
  magnetic field is modelled and nothing is actually accelerated. See §5.3.

---

## 8. Checking the claims yourself

| Question | How to check |
| --- | --- |
| Is the lensing real? | Debug view 1 hides the disk. Move the camera: the star arcs move consistently, and stars appear in *two* places (once directly, once lensed around the far side). A UV warp cannot do that. |
| Is the shadow the right size? | Debug view 3. The red region's angular radius should match `b_crit/D = 2.598 r_s / distance`. The UI prints `b_crit` for the current `r_s`. |
| Is the photon sphere real? | Debug view 2. The bright ring is where rays needed the most integration steps — i.e. where they wound around `r = 1.5 r_s`. Debug view 7 confirms their closest approach converges there. |
| Is the Doppler shift real or painted on? | Debug view 6, then orbit the camera. The blue/red split follows the *lensed* disk around the hole and inverts when you switch to Retrograde. |
| Is the spin doing anything real? | Set Spin to 0.998, Density to 0, and open debug view 3. The shadow is displaced sideways and flattened on one side. Flip the spin to -0.998: it mirrors exactly. A circle cannot do that. |
| Is the jet beaming real? | Point the camera near the spin axis: the near jet saturates and the counter-jet almost vanishes. Swing round to edge-on and they even out. Nothing in the shader knows which jet is which -- only the viewing angle changes. |
| Is the jet really tied to the spin? | Leave "Powered by the spin" on and drag Spin to zero. The jet disappears entirely, because Blandford-Znajek power goes as a*^2. |
| Is the integrator converged? | Debug view 3 again: magenta pixels mean the step budget ran out. Raise **Max RK4 steps** until they disappear, then compare renders at Low and Ultra — the geometry should not move. |
