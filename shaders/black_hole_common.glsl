
// =============================================================================
// BLACK HOLE SANDBOX -- primary HDR ray-tracing pass
// =============================================================================
//
// WHAT THIS SHADER DOES
// ---------------------
// For every pixel it launches one (or a few jittered) camera rays and follows
// each one *backwards* through the curved spacetime of a Schwarzschild black
// hole until the ray either
//
//   * crosses the event horizon        -> that pixel sees nothing (the shadow),
//   * escapes to large radius          -> sample the procedural sky,
//   * passes through the accretion disk-> accumulate glowing plasma along it.
//
// Because the trajectory is genuinely integrated, gravitational lensing, the
// black-hole shadow, the photon ring and the multiple images of the accretion
// disk are *emergent*: nothing in this file draws a circle or warps UVs.
//
//
// COORDINATES
// -----------
//   * The black hole sits at the world-space origin.
//   * World +Y is the disk's rotation axis; the disk lies around the y = 0 plane.
//   * All lengths use the same scene units as uCameraPosition. The natural unit
//     is the Schwarzschild radius r_s = 2GM/c^2 (uSchwarzschildRadius).
//
//
// THE PHYSICS: the null geodesic "orbit equation"
// -----------------------------------------------
// Schwarzschild spacetime is spherically symmetric, so a single light ray stays
// inside one fixed plane through the origin -- the plane spanned by the camera
// position and the ray direction. That reduces a 4-dimensional geodesic problem
// to a 2-dimensional one: we only need r(phi) inside that plane.
//
// Writing u = r_s / r (a dimensionless "inverse radius": u = 0 at infinity and
// u = 1 exactly at the horizon) the null geodesic equation becomes
//
//         d^2 u / dphi^2  =  (3/2) u^2  -  u                              (1)
//
// This is the Binet / orbit equation for photons. The "-u" term alone gives
// straight lines in polar coordinates (no deflection at all); the "(3/2) u^2"
// term is entirely general-relativistic and is what bends the light.
//
// We integrate (1) as a first-order system with v = du/dphi:
//
//         du/dphi = v
//         dv/dphi = (3/2) u^2 - u
//
// using classical fourth-order Runge-Kutta. See docs/PHYSICS.md.
//
// The first integral of (1) (the "radial potential") is
//
//         (du/dphi)^2 = (r_s/b)^2 - u^2 + u^3                             (2)
//
// where b is the photon's impact parameter, a conserved quantity. We use (2)
// once, at the camera, to obtain the correct initial v from the pixel's viewing
// angle. Equation (2) also explains the shadow: the critical impact parameter
// is b_crit = (3*sqrt(3)/2) r_s ~= 2.598 r_s, and rays with b < b_crit have no
// turning point and must fall in.
//
// =============================================================================


// ---- Camera -----------------------------------------------------------------
uniform mat4  uInvView;         // view^-1: camera space -> world space
uniform mat4  uInvProjection;   // projection^-1: clip -> camera space
uniform vec3  uCameraPosition;
uniform vec2  uResolution;
uniform float uTime;            // Animation clock (frozen while refining).
uniform vec2  uJitter;          // Extra sub-pixel offset, in pixels.
uniform int   uSampleIndex;     // Progressive-refinement sample number.
uniform int   uSamplesPerFrame; // Rays per pixel per frame (1..8).

// ---- Black hole and ray integrator -----------------------------------------
uniform float uSchwarzschildRadius; // r_s in scene units.
uniform float uSpin;                // Dimensionless spin a* = a/M, in [-1, 1].
uniform float uRayStep;             // Base RK4 angular increment dphi.
uniform int   uMaxRaySteps;         // Integration budget per ray.
uniform float uEscapeRadius;        // Radius treated as "far away".
// Closest approach, in r_s, beyond which the geodesic is replaced by the
// weak-field deflection series instead of being integrated. 0 disables the
// shortcut and always integrates.
uniform float uWeakFieldRadius;
// Angular step schedule.  uRayStep is the base increment and stays the overall
// quality control; these two say how far the step is allowed to open up once
// the ray is out where the trajectory is nearly straight.
uniform float uRayStepGrowth;   // Extra step per r_s of radius. 0 = fixed step.
uniform float uRayStepMax;      // Hard ceiling on dphi, in radians.

// ---- Accretion disk ---------------------------------------------------------
uniform float uDiskInnerRadius;
uniform float uDiskOuterRadius;
uniform float uDiskHalfThickness;   // Scale height H at the inner edge.
uniform float uDiskFlare;           // Exponent for H(r) growth; 0 = flat slab.
uniform float uDiskBrightness;
uniform float uDiskTemperature;     // Peak emitted colour temperature, kelvin.
uniform float uDiskDensity;         // Multiplies both emission and opacity.
uniform float uDiskOpacity;         // Extinction per unit length; large = opaque.
uniform float uDiskTurbulence;      // 0 = smooth analytic disk, 1 = full noise.
uniform float uDiskRotationDirection;
uniform float uArtisticOrbitSpeed;  // Pattern-rotation rate multiplier.

// ---- Relativistic jet -------------------------------------------------------
uniform float uJetPower;        // 0 disables the outflow entirely.
uniform float uJetLength;
uniform float uJetBaseRadius;
uniform float uJetCollimation;  // radius ~ height^collimation
uniform float uJetLorentz;      // Bulk Lorentz factor of the flow.
uniform float uJetTemperature;  // Colour only; synchrotron is not thermal.
uniform float uJetTurbulence;
uniform int   uJetScalesWithSpin; // Blandford-Znajek a*^2 coupling.

// ---- Accretion dynamics -----------------------------------------------------
uniform float uPlungeFraction;  // How far inside the ISCO gas keeps radiating.
uniform float uAccretionRate;   // Radial drift, as a fraction of orbital speed.
uniform float uIscoRadius;      // Computed on the CPU; depends on the spin.

// ---- Relativistic optics (1 = full physical strength, 0 = disabled) ---------
// These exist so each effect can be isolated and studied, not to suggest the
// physics is optional.
uniform float uDopplerStrength;
uniform float uGravitationalShiftStrength;
uniform float uBeamingStrength;

// ---- Environment ------------------------------------------------------------
uniform float uStarDensity;
uniform float uNebulaStrength;

// ---- Debug ------------------------------------------------------------------
uniform int uShowHorizonGuide;
uniform int uShowPhotonSphereGuide;
// 0 final render                1 lensed environment only (disk hidden)
// 2 RK4 step-count heat map     3 escape/capture classification
// 4 disk optical depth          5 combined frequency shift g
// 6 Doppler factor only         7 closest-approach / important radii
// 8 background only             9 disk only
uniform int uDebugMode;

// =============================================================================
// Constants
// =============================================================================

// Compile-time ceiling on the integration loop. GLSL wants a constant bound so
// the compiler can reason about the loop; uMaxRaySteps selects how much of this
// budget a given quality preset actually uses.
const int kCompiledMaxRaySteps = 2048;

// Ray outcomes.
const int kTraceEscaped   = 0; // Reached uEscapeRadius: sample the sky.
const int kTraceCaptured  = 1; // Crossed the horizon: black.
const int kTraceExhausted = 2; // Ran out of budget near the critical parameter.


struct TraceResult
{
    int   state;
    int   steps;
    float minRadius;        // Closest approach; reveals the photon sphere.
    vec3  escapeDirection;  // Sky direction if the ray escaped.

    // Volumetric accumulation through the disk.
    vec3  diskRadiance;     // Emission already weighted by g^4 and extinction.
    float transmittance;    // 1 = disk fully transparent along this ray.
    float opticalDepth;     // Integrated extinction, for the debug view.

    // Diagnostics, recorded where the disk contributed the most light.
    float peakWeight;
    float shiftTotal;       // g = g_gravity * doppler
    float shiftDoppler;
    float shiftGravity;
};

// =============================================================================
// Small utilities
// =============================================================================

float configuredOr(float value, float fallback)
{
    return value > 0.0 ? value : fallback;
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p3)
{
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 hash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float valueNoise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(cell),                  hash12(cell + vec2(1.0, 0.0)), f.x),
               mix(hash12(cell + vec2(0.0, 1.0)), hash12(cell + vec2(1.0, 1.0)), f.x),
               f.y);
}

// Trilinearly interpolated 3-D value noise. The disk uses this in a curved
// "orbital" coordinate frame so the turbulence flows with the plasma.
float valueNoise3(vec3 p)
{
    vec3 cell = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash13(cell + vec3(0.0, 0.0, 0.0));
    float n100 = hash13(cell + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(cell + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(cell + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(cell + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(cell + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(cell + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(cell + vec3(1.0, 1.0, 1.0));

    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
               f.z);
}

float fbm2(vec2 p)
{
    float sum = 0.0;
    float amplitude = 0.5;
    mat2 rotation = mat2(0.80, -0.60, 0.60, 0.80);
    for (int octave = 0; octave < 5; ++octave)
    {
        sum += amplitude * valueNoise(p);
        p = rotation * p * 2.03 + 17.2;
        amplitude *= 0.5;
    }
    return sum;
}

float fbm3(vec3 p, int octaves)
{
    float sum = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 5; ++octave)
    {
        if (octave >= octaves) break;
        sum += amplitude * valueNoise3(p);
        p = p * 2.07 + vec3(11.3, 7.7, 5.1);
        amplitude *= 0.5;
    }
    return sum;
}

// Planck-locus RGB approximation (Tanner Helland's fit). This is a *colour*
// only: its brightest channel peaks near 1.0, so luminance must be applied
// separately -- we use the Stefan-Boltzmann T^4 law for that.
vec3 blackbodyRgb(float kelvin)
{
    float t = clamp(kelvin, 800.0, 40000.0) * 0.01;
    float r, g, b;
    if (t <= 66.0)
    {
        r = 1.0;
        g = clamp(0.39008158 * log(max(t, 1.0)) - 0.63184144, 0.0, 1.0);
        b = t <= 19.0 ? 0.0 : clamp(0.54320679 * log(t - 10.0) - 1.19625409, 0.0, 1.0);
    }
    else
    {
        r = clamp(1.29293619 * pow(t - 60.0, -0.13320476), 0.0, 1.0);
        g = clamp(1.12989086 * pow(t - 60.0, -0.07551485), 0.0, 1.0);
        b = 1.0;
    }
    return vec3(r, g, b);
}

// =============================================================================
// Procedural environment (the distant sky)
// =============================================================================

vec2 signNotZero(vec2 value)
{
    return mix(vec2(-1.0), vec2(1.0), step(vec2(0.0), value));
}

// Octahedral direction encoding: maps the unit sphere onto the unit square with
// far less distortion than latitude/longitude, and with no polar pinch or
// visible vertical seam.
vec2 octahedralUv(vec3 direction)
{
    vec3 n = direction / max(dot(abs(direction), vec3(1.0)), 1e-6);
    vec2 p = n.xy;
    if (n.z < 0.0)
    {
        p = (1.0 - abs(p.yx)) * signNotZero(p);
    }
    return p * 0.5 + 0.5;
}

// One stochastic layer of sub-pixel HDR stars. Neighbouring cells are also
// examined so a star sitting on a cell border stays continuous as the lensed
// ray direction sweeps across it.
vec3 starLayer(vec2 uv, float cellCount, float density, float seed)
{
    vec2 p = uv * cellCount;
    vec2 baseCell = floor(p);
    vec2 local = fract(p);
    // Screen-space derivatives must be taken in uniform control flow.
    float filterWidth = max(max(fwidth(p.x), fwidth(p.y)) * 1.2, 0.0012);
    vec3 radiance = vec3(0.0);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y));
            vec3 h = hash33(vec3(baseCell + offset, seed));
            if (h.z > clamp(density, 0.0, 1.0))
            {
                continue;
            }

            vec2 delta = local - (offset + h.xy);
            float distanceToStar = length(delta);
            // A steep power keeps most stars faint and a rare few very bright,
            // roughly mimicking a real magnitude distribution. The scale is
            // chosen so an ordinary star sits well below 1.0 in linear HDR and
            // only the rare brightest ones clip and bloom.
            //
            // Star discs are kept small on purpose. Gravitational lensing
            // magnifies the sky enormously near the shadow, stretching each
            // star tangentially into an arc -- which is real, and is exactly
            // what produces the concentric rings. Start with big discs and that
            // same effect turns the whole image into scratches.
            float luminosity = mix(0.02, 5.0, pow(hash12(baseCell + offset + seed), 8.0));
            float radius = mix(0.006, 0.032, pow(h.y, 5.0));
            float core = 1.0 - smoothstep(radius - filterWidth, radius + filterWidth, distanceToStar);
            float halo = exp(-distanceToStar * distanceToStar / max(radius * radius * 11.0, 1e-5));
            vec3 colour = blackbodyRgb(mix(2400.0, 14500.0, pow(h.x, 1.4)));
            radiance += colour * luminosity * (core * 1.3 + halo * 0.05);
        }
    }
    return radiance;
}

vec3 sampleEnvironment(vec3 direction)
{
    vec3 n = normalize(direction);
    vec2 uv = octahedralUv(n);
    float density = max(uStarDensity, 0.0);

    // A faint galactic band. Domain warping breaks up the obvious fbm look.
    vec3 galacticPole = normalize(vec3(0.31, 0.82, -0.48));
    float bandDistance = abs(dot(n, galacticPole));
    float band = exp(-bandDistance * bandDistance * 70.0);

    vec2 warpUv = uv * vec2(14.0, 7.0);
    vec2 warp = vec2(fbm2(warpUv + vec2(1.7, 9.2)), fbm2(warpUv + vec2(8.3, 2.8)));
    float dust = fbm2(warpUv + 2.4 * warp);
    float filaments = pow(clamp(fbm2(warpUv * 2.7 + 5.0 * warp), 0.0, 1.0), 2.2);

    vec3 deepSpace = vec3(0.00012, 0.00028, 0.00090);
    vec3 coolGas   = vec3(0.0012, 0.0031, 0.0092);
    vec3 warmGas   = vec3(0.0090, 0.0026, 0.0011);
    vec3 galaxy = mix(warmGas, coolGas, dust) * (band * mix(0.10, 1.30, dust) + filaments * band * 0.55);
    galaxy *= max(uNebulaStrength, 0.0);

    // Two very different cell sizes stop the field from reading as a grid.
    vec3 stars = starLayer(uv, 320.0, 0.085 * density, 11.3);
    stars += starLayer(uv * 1.713 + vec2(0.193, 0.617), 940.0, 0.025 * density, 71.9);
    return deepSpace + galaxy + stars;
}

// =============================================================================
// Geodesic integration
// =============================================================================

// Right-hand side of the reduced null geodesic system:
//     du/dphi = v
//     dv/dphi = (3/2) u^2 - u
void geodesicDerivative(float u, float v, out float du, out float dv)
{
    du = v;
    dv = 1.5 * u * u - u;
}

// Classical RK4: four derivative probes per step, local error O(h^5).
void integrateGeodesicRk4(inout float u, inout float v, float h)
{
    float du1, dv1, du2, dv2, du3, dv3, du4, dv4;
    geodesicDerivative(u, v, du1, dv1);
    geodesicDerivative(u + 0.5 * h * du1, v + 0.5 * h * dv1, du2, dv2);
    geodesicDerivative(u + 0.5 * h * du2, v + 0.5 * h * dv2, du3, dv3);
    geodesicDerivative(u + h * du3,       v + h * dv3,       du4, dv4);

    u += (h / 6.0) * (du1 + 2.0 * du2 + 2.0 * du3 + du4);
    v += (h / 6.0) * (dv1 + 2.0 * dv2 + 2.0 * dv3 + dv4);
}

// Recover a world-space point from the in-plane polar coordinates (phi, u).
vec3 positionInOrbitPlane(float phi, float u, float rs, vec3 radialBasis, vec3 azimuthBasis)
{
    float radius = rs / max(u, 1e-6);
    return radius * (cos(phi) * radialBasis + sin(phi) * azimuthBasis);
}

// Analytic unit tangent of the trajectory, pointing forward along the ray.
// Using this instead of the chord between two integration points keeps the
// Doppler calculation accurate even at coarse quality settings.
vec3 directionInOrbitPlane(float phi, float u, float v, float rs, vec3 radialBasis, vec3 azimuthBasis)
{
    vec3 er   =  cos(phi) * radialBasis + sin(phi) * azimuthBasis;
    vec3 ephi = -sin(phi) * radialBasis + cos(phi) * azimuthBasis;
    float radius = rs / max(u, 1e-6);
    // dr/dphi = d(r_s/u)/dphi = -r_s * v / u^2
    float drDphi = -rs * v / max(u * u, 1e-8);
    // Read in the frame of a static observer, which measures proper radial
    // distance dr/sqrt(1 - r_s/r) while the tangential leg r dphi is already
    // proper. Without the stretch this returns the *coordinate* direction, a
    // different vector by O(r_s/r) -- over a pixel at the default escape radius,
    // and the reason the two solvers used to disagree at the far end.
    float stretch = inversesqrt(max(1.0 - u, 1e-5));
    return normalize(drDphi * stretch * er + radius * ephi);
}

// =============================================================================
// Accretion disk model
// =============================================================================
//
// GEOMETRY. A Shakura-Sunyaev style thin disk: vertical density is Gaussian
// about the equatorial plane with a scale height H(r) that may flare outwards,
//
//     rho(r, z) = Sigma(r) * exp( -z^2 / (2 H(r)^2) ),   H(r) = H0 (r/r_in)^q
//
// EMISSIVITY. The radial profile is the zero-torque thin-disk flux of the
// Novikov-Thorne / Shakura-Sunyaev family in its simple limit,
//
//     F(r) ~ r^-3 * (1 - sqrt(r_in / r))
//
// which vanishes at the inner edge, peaks a little outside it, and then falls
// off steeply. The local emitted temperature follows Stefan-Boltzmann,
//
//     T(r) = T_peak * ( F(r) / F_peak )^(1/4)
//
// so the inner disk is genuinely hotter (bluer) than the outer disk.

float diskScaleHeight(float radius, float innerRadius, float baseHeight)
{
    float flare = clamp(uDiskFlare, 0.0, 2.0);
    return baseHeight * pow(max(radius / innerRadius, 1.0), flare);
}

// Normalised radial flux profile, peaking at 1.0.
// F(r) ~ r^-3 (1 - sqrt(r_in/r)) has its maximum at r = (49/36) r_in, where the
// bracketed expression equals (36/49)^3 * (1 - 6/7) = 0.0568. Dividing by that
// constant is exactly where the 17.6 below comes from (1 / 0.0568).
float thinDiskFluxProfile(float radius, float innerRadius)
{
    float x = innerRadius / max(radius, 1e-4);          // = r_in / r, in (0, 1]
    float flux = x * x * x * max(1.0 - sqrt(x), 0.0);   // proportional to F(r)
    return clamp(flux * 17.6, 0.0, 1.0);
}

// Turbulent density modulation in a co-rotating coordinate frame.
//   * log(r) as the radial coordinate keeps feature size proportional to r.
//   * The azimuth is un-wound by the local Keplerian angular velocity, so the
//     pattern orbits with the gas instead of sliding across it.
//   * Omega(r) ~ r^-3/2 is the Keplerian law; differential rotation then shears
//     the pattern into trailing spiral lanes on its own.
float diskTurbulence(float radius, float polar, float height, float innerRadius)
{
    float amount = clamp(uDiskTurbulence, 0.0, 1.0);
    if (amount <= 0.0)
    {
        return 1.0;
    }

    float rotationDirection = uDiskRotationDirection < 0.0 ? -1.0 : 1.0;
    float keplerRate = pow(max(innerRadius / radius, 1e-3), 1.5);   // ~ Omega(r)
    float theta = polar - uTime * rotationDirection * max(uArtisticOrbitSpeed, 0.0) * keplerRate;
    // Advect the pattern *inwards* as well as around, so features visibly
    // spiral towards the hole instead of orbiting forever. The drift is scaled
    // by the same accretion rate that sets the radial velocity, which keeps the
    // appearance and the Doppler shift telling the same story.
    float inflow = uTime * max(uArtisticOrbitSpeed, 0.0) * clamp(uAccretionRate, 0.0, 0.9)
                 * keplerRate * 0.6;
    float logRadius = log(max(radius / innerRadius, 1.0)) + inflow;

    // Noise coordinate on a cylinder. The azimuth is sampled *around a circle*
    // so the pattern wraps seamlessly at 2*pi with no visible seam, and the
    // radial axis runs along the cylinder. The circle radius and the radial
    // scale are deliberately unequal so features come out shorter in radius
    // than in azimuth: gas sheared by differential rotation is stretched that
    // way, and isotropic noise here reads as radial "fur" instead.
    vec3 p = vec3(cos(theta), sin(theta), 0.0) * 1.4
           + vec3(0.0, 0.0, logRadius * 9.0 + height * 0.35);

    float coarse = fbm3(p, 4);
    float fine   = fbm3(p * 2.6 + vec3(0.0, 0.0, coarse * 1.5), 3);
    float turbulence = clamp(mix(coarse, fine, 0.40) * 2.0, 0.0, 1.35);

    // Trailing logarithmic spiral arms. sin(k*log r - m*theta) is *exactly* a
    // logarithmic spiral with m arms, and an integer m wraps perfectly in
    // azimuth. Two different arm counts stop the result looking periodic; the
    // small turbulence term breaks the arms into clumps without erasing them.
    float armsA = sin( 9.0 * logRadius - 2.0 * theta + turbulence * 1.4);
    float armsB = sin(17.0 * logRadius - 5.0 * theta - turbulence * 1.1);
    float lanes = 0.5 + 0.5 * (0.62 * armsA + 0.38 * armsB);

    float modulation = mix(0.45, 1.75, turbulence) * mix(0.68, 1.32, lanes);
    return mix(1.0, modulation, amount);
}

struct DiskSample
{
    vec3  radiance;   // Observed radiance contribution per unit length.
    float extinction; // Observed extinction coefficient per unit length.
    float shiftTotal;
    float shiftDoppler;
    float shiftGravity;
};

// Evaluate the disk at one point along the ray.
//   position   world-space sample point
//   photonDir  unit vector along the ray's forward direction (camera -> scene)
DiskSample sampleDisk(vec3 position, vec3 photonDir, float rs, float innerRadius, float outerRadius)
{
    DiskSample result;
    result.radiance = vec3(0.0);
    result.extinction = 0.0;
    result.shiftTotal = 1.0;
    result.shiftDoppler = 1.0;
    result.shiftGravity = 1.0;

    float radius = length(position.xz);

    // ---- The plunging region ----------------------------------------------
    // Inside the ISCO no stable circular orbit exists, so the gas stops
    // orbiting and falls. It does not stop *existing*, though, and it does not
    // stop radiating: it spirals inwards over a few orbits, thinning and
    // dimming, until it crosses the horizon. Ignoring that leaves the classical
    // thin disk with an unphysically sharp hole in the middle.
    //
    // uPlungeFraction says how far into the gap between the ISCO and the
    // horizon that infalling gas is still drawn. Zero restores the sharp edge.
    float horizonRadius = 0.5 * rs * (1.0 + sqrt(max(1.0 - uSpin * uSpin, 0.0)));
    float plungeInner = mix(innerRadius, horizonRadius * 1.02,
                            clamp(uPlungeFraction, 0.0, 1.0));
    if (radius < plungeInner || radius > outerRadius)
    {
        return result;
    }
    // 1 in the disk proper, ramping to 0 at the horizon.
    float plunging = 1.0 - smoothstep(plungeInner, innerRadius, radius);

    // ---- Density -----------------------------------------------------------
    float baseHeight = configuredOr(uDiskHalfThickness, 0.1 * rs);
    float scaleHeight = diskScaleHeight(radius, innerRadius, baseHeight);
    float vertical = exp(-0.5 * (position.y * position.y) / max(scaleHeight * scaleHeight, 1e-8));

    float fluxProfile = thinDiskFluxProfile(radius, innerRadius);
    // Inside the ISCO the zero-torque profile has nothing to say, so the
    // plunging gas carries its ISCO-edge emissivity inwards while thinning out
    // rapidly as it accelerates and its column depth collapses.
    if (plunging > 0.0)
    {
        float plungeDepth = clamp((innerRadius - radius) / max(innerRadius - plungeInner, 1e-4), 0.0, 1.0);
        fluxProfile = mix(fluxProfile, thinDiskFluxProfile(innerRadius * 1.02, innerRadius), plunging);
        fluxProfile *= 1.0 - plungeDepth * plungeDepth;
    }

    // Soften both rims so the disk does not end on a hard geometric edge.
    // (smoothstep requires edge0 < edge1, hence the explicit 1 - form.)
    float outerFade = 1.0 - smoothstep(mix(innerRadius, outerRadius, 0.86), outerRadius, radius);
    float innerFade = smoothstep(plungeInner, plungeInner * 1.06, radius);

    float polar = atan(position.z, position.x);
    float structure = diskTurbulence(radius, polar, position.y / max(scaleHeight, 1e-5), innerRadius);

    // Surface density falls off outwards; combined with the vertical Gaussian
    // this is the volumetric density we integrate through.
    float surfaceDensity = pow(innerRadius / max(radius, 1e-4), 1.5);
    float density = max(uDiskDensity, 0.0) * surfaceDensity * vertical * outerFade * innerFade * structure;
    if (density <= 1e-6)
    {
        return result;
    }

    // ---- Local emitted temperature ----------------------------------------
    // The turbulence also perturbs the temperature, not only the density.
    //
    // This matters more than it looks. Where the disk is optically thick the
    // observed radiance tends to the source function j/kappa, and since both
    // emission and extinction are proportional to density, *density cancels out
    // completely*. Modulating density alone would give a perfectly smooth,
    // featureless disk. Temperature does not cancel -- and because thermal
    // brightness goes as T^4, even a +/-20% ripple is a factor of two in
    // brightness. Physically this stands in for patchy turbulent dissipation:
    // the parts of the flow doing more viscous work run hotter.
    float peakTemperature = configuredOr(uDiskTemperature, 7000.0);
    float emittedTemperature =
        peakTemperature * pow(max(fluxProfile, 1e-4), 0.25) * pow(max(structure, 1e-3), 0.30);

    // ---- Relativistic frequency shift --------------------------------------
    //
    // The shift is evaluated in the frame of a *locally non-rotating observer*
    // (a ZAMO), which is the right local frame in Kerr and degenerates to the
    // static observer when the hole does not spin. Two pieces:
    //
    //   gravity  = alpha, the lapse. What this observer's clock does relative
    //              to one at infinity.
    //   doppler  = the ordinary special-relativistic factor for the orbiting
    //              plasma's velocity *as measured by that observer*.
    //
    // Doing it locally rather than through the covariant g = 1/[u^t(1 - Omega
    // b_z)] is a deliberate choice. The covariant form is exact and shorter,
    // but it needs the angular momentum of the photon that actually reaches the
    // camera -- and this ray is traced backwards, so the sign is inverted and
    // the denominator can pass through zero at high spin, which blows the
    // image out. The local form is bounded by construction: beta < 1 always.
    float mass = 0.5 * rs;
    float spin = clamp(uSpin, -0.998, 0.998) * mass;   // a, in scene units
    float rotationDirection = uDiskRotationDirection < 0.0 ? -1.0 : 1.0;
    float sqrtM = sqrt(max(mass, 1e-6));
    float r2 = radius * radius;
    float a2 = spin * spin;

    // Equatorial Kerr metric functions (sin(theta) = 1, Sigma = r^2).
    float delta = max(r2 - 2.0 * mass * radius + a2, 1e-6);
    float bigA = (r2 + a2) * (r2 + a2) - a2 * delta;
    float lapse = sqrt(max(r2 * delta / max(bigA, 1e-8), 1e-8));   // alpha
    float frameDrag = 2.0 * spin * mass * radius / max(bigA, 1e-8); // omega_ZAMO
    float gPhiPhi = bigA / max(r2, 1e-8);

    // Angular velocity of an equatorial circular orbit in Kerr:
    //     Omega = +- sqrt(M) / ( r^(3/2) +- a sqrt(M) )
    // The upper sign is prograde. At a = 0 this is the Kepler law
    // Omega = sqrt(M/r^3), and the whole block reduces to Schwarzschild.
    float r15 = radius * sqrt(radius);
    float orbitRate = rotationDirection * sqrtM
                    / max(r15 + rotationDirection * spin * sqrtM, 1e-5);

    // Plasma speed measured by the ZAMO. Subtracting the frame-dragging rate
    // is the whole point: close in, space itself is rotating, and only the
    // *difference* is motion the local observer can see.
    float beta = (orbitRate - frameDrag) * sqrt(max(gPhiPhi, 0.0)) / max(lapse, 1e-6);
    beta = clamp(beta, -0.999, 0.999);
    float lorentz = inversesqrt(max(1.0 - beta * beta, 1e-6));

    vec3 radialDirection = normalize(vec3(position.x, 0.0, position.z));
    vec3 orbitalDirection = normalize(cross(vec3(0.0, 1.0, 0.0), radialDirection));

    // ---- Infall ------------------------------------------------------------
    // Two things push the gas inwards.
    //
    //  * Accretion. A thin disk drifts inwards slowly -- viscous transport of
    //    angular momentum -- so the radial speed is a small fraction of the
    //    orbital speed. Without it the pattern would circle forever instead of
    //    spiralling in, which is the visual difference between a rotating
    //    texture and something actually falling into a black hole.
    //  * The plunge. Inside the ISCO there is nothing left to hold the gas up
    //    and it accelerates towards free fall. The Newtonian-limit escape speed
    //    sqrt(r_s/r) is used as the scale here; it is a fair approximation well
    //    outside the horizon and is clamped rather than allowed to reach 1.
    float driftSpeed = clamp(uAccretionRate, 0.0, 0.9) * abs(beta);
    if (plunging > 0.0)
    {
        float freeFall = sqrt(clamp(rs / max(radius, 1e-4), 0.0, 0.98));
        driftSpeed = mix(driftSpeed, freeFall, plunging);
    }
    driftSpeed = min(driftSpeed, 0.97);

    // Combine the orbital and radial motions into one velocity, then rebuild
    // the Lorentz factor from the total speed. Doing it this way keeps the
    // plunging gas relativistic in the right direction: mostly inwards, still
    // partly swept around by what angular momentum it retains.
    vec3 velocity = orbitalDirection * beta - radialDirection * driftSpeed;
    float speed = min(length(velocity), 0.999);
    lorentz = inversesqrt(max(1.0 - speed * speed, 1e-6));
    vec3 flowDirection = speed > 1e-5 ? velocity / speed : orbitalDirection;

    // photonDir points camera -> scene; the photon that actually reaches the
    // camera travels the other way, so the direction *towards the observer* is
    // its negative. Using the locally integrated direction (not the straight
    // camera-to-pixel vector) is what makes the Doppler pattern follow the
    // lensed image rather than the screen.
    vec3 toObserver = -photonDir;

    float doppler = 1.0 / max(lorentz * (1.0 - speed * dot(flowDirection, toObserver)), 1e-4);
    float gravity = lapse;

    // Blending towards 1.0 lets each effect be dialled down for study.
    doppler = mix(1.0, doppler, clamp(uDopplerStrength, 0.0, 1.0));
    gravity = mix(1.0, gravity, clamp(uGravitationalShiftStrength, 0.0, 1.0));

    // The two multiply: the photon is Doppler-shifted out of the orbiting
    // plasma into the local non-rotating frame, then redshifted climbing out of
    // the potential well to the distant camera.
    float g = clamp(gravity * doppler, 0.05, 8.0);

    // ---- Observed radiance -------------------------------------------------
    // I_nu / nu^3 is invariant along a null geodesic. Integrated over frequency
    // the bolometric intensity therefore transforms as
    //
    //     I_observed = g^4 * I_emitted
    //
    // For thermal emission this is beautifully self-consistent: a blackbody at
    // T_emit is *seen* as a blackbody at T_obs = g * T_emit, and
    // sigma (g T_emit)^4 = g^4 sigma T_emit^4. So the colour shift and the
    // beaming are two faces of the same transformation, not two separate
    // effects bolted together. uBeamingStrength only exists so the intensity
    // half can be switched off while studying the colour half.
    //
    // NORMALISATION (a units choice, not physics): the thermal term is divided
    // by the disk's own peak temperature, so `thermal` is 1 at the hottest
    // radius when unshifted. That decouples the Brightness slider from the
    // Temperature slider -- changing the colour no longer changes the exposure
    // by a factor of T^4. Every *relative* brightness variation across the
    // image -- the radial profile, beaming, redshift -- is still the physical
    // one.
    float observedTemperature = g * emittedTemperature;
    float thermal = pow(emittedTemperature / max(peakTemperature, 1.0), 4.0);
    float beaming = mix(1.0, pow(g, 4.0), clamp(uBeamingStrength, 0.0, 1.0));

    vec3 colour = blackbodyRgb(observedTemperature);

    result.radiance   = colour * thermal * beaming * density * max(uDiskBrightness, 0.0);
    result.extinction = density * max(uDiskOpacity, 0.0);
    result.shiftTotal = g;
    result.shiftDoppler = doppler;
    result.shiftGravity = gravity;
    return result;
}

// =============================================================================
// Relativistic jet
// =============================================================================
//
// A pair of collimated outflows along the spin axis.
//
// WHERE THE ENERGY COMES FROM. The Blandford-Znajek mechanism taps the *hole's
// own rotation*: magnetic field lines threading the horizon are wound up by
// frame dragging and carry away rotational energy, with power scaling as
//
//     P_BZ  ~  a*^2 B^2 M^2
//
// The a*^2 is why the jet here fades out entirely as the spin goes to zero --
// a non-rotating black hole has no rotational energy to extract. That coupling
// is switchable in the UI, but leaving it on is the physical behaviour, and it
// is the reason the spin slider and the jet slider are related at all.
//
// WHAT IS MODELLED AND WHAT IS NOT. The *geometry* is phenomenological: a
// parabolic envelope fitted to what jets are observed to look like, not the
// solution of any equation. The *kinematics and brightness* are real special
// relativity: the flow moves at a bulk Lorentz factor, and the resulting
// Doppler boost is what makes one jet blindingly bright and the counter-jet
// nearly invisible -- exactly the asymmetry seen in M87. Nothing here solves
// magnetohydrodynamics; that is a supercomputer problem, not a shader.

// Half-width of the outflow at a given height above the equatorial plane.
// Real jets are collimated parabolically -- M87's is measured at roughly
// r ~ z^0.6 -- rather than opening as a straight cone.
float jetRadiusAtHeight(float height, float baseRadius, float collimation)
{
    return baseRadius * pow(max(height, 1e-3), clamp(collimation, 0.0, 1.2));
}

// Emission from the outflow at one point. Returns radiance per unit length;
// the jet is treated as optically thin, which is right for synchrotron
// emission at these densities.
vec3 sampleJet(vec3 position, vec3 photonDir, float rs, float spinStar)
{
    float power = max(uJetPower, 0.0);
    if (power <= 0.0)
    {
        return vec3(0.0);
    }

    float height = abs(position.y);
    float jetLength = max(uJetLength, 1.0);
    if (height > jetLength)
    {
        return vec3(0.0);
    }

    // The base sits just outside the horizon; below that there is no outflow,
    // only infall.
    float launchRadius = 1.2 * rs;
    if (height < launchRadius * 0.35)
    {
        return vec3(0.0);
    }

    float cylindrical = length(position.xz);
    float width = jetRadiusAtHeight(height, max(uJetBaseRadius, 0.02), uJetCollimation);
    float normalized = cylindrical / max(width, 1e-4);
    if (normalized > 1.8)
    {
        return vec3(0.0);
    }

    // A limb-brightened shell rather than a filled cone: the emission in real
    // jets is concentrated towards the walls, which is why they photograph as
    // two rails rather than a solid beam.
    float shell = exp(-pow(abs(normalized - 0.75) / 0.28, 2.0));
    float core = 0.25 * exp(-normalized * normalized * 2.4);
    float profile = shell + core;

    // Fade in at the base and out at the tip so the outflow has no hard ends.
    float baseFade = smoothstep(launchRadius * 0.35, launchRadius * 1.8, height);
    float tipFade = 1.0 - smoothstep(jetLength * 0.55, jetLength, height);

    // Density drops as the flow expands and spreads its material over a wider
    // cross-section.
    float dilution = 1.0 / (1.0 + pow(height / max(3.0 * launchRadius, 1e-3), 1.35));

    // Knots and helical structure. Advected outwards with the flow, so the
    // pattern travels along the jet rather than sitting still.
    float turbulence = 1.0;
    float amount = clamp(uJetTurbulence, 0.0, 1.0);
    if (amount > 0.0)
    {
        // Advected with the flow, so knots travel outwards rather than sitting
        // still. The frequency is deliberately low: a jet's structure is a few
        // big blobs, and a high frequency here just aliases into stripes.
        float travel = height - uTime * 1.6;
        float twist = atan(position.z, position.x) - height * 0.12;
        vec3 noiseP = vec3(cos(twist), sin(twist), 0.0) * 0.8
                    + vec3(0.0, 0.0, travel * 0.16);
        float knots = fbm3(noiseP, 3);
        turbulence = mix(1.0, mix(0.45, 1.75, knots), amount);
    }

    float density = profile * baseFade * tipFade * dilution * turbulence;
    if (density <= 1e-5)
    {
        return vec3(0.0);
    }

    // ---- Relativistic beaming ----------------------------------------------
    // The flow streams away from the hole along the axis at a bulk Lorentz
    // factor Gamma. For a *continuous* jet the observed intensity is boosted by
    //
    //     delta^(2 + alpha)
    //
    // with delta the Doppler factor and alpha the synchrotron spectral index
    // (about 0.7 for these sources). The exponent is 2+alpha rather than the
    // 4 used for the thermal disk because a steady jet is a standing structure,
    // not a set of discrete blobs -- one power of delta is lost to the fact
    // that the emitting volume is fixed in the observer's frame.
    float lorentz = max(uJetLorentz, 1.0);
    float beta = sqrt(max(1.0 - 1.0 / (lorentz * lorentz), 0.0));
    vec3 flowDirection = vec3(0.0, position.y >= 0.0 ? 1.0 : -1.0, 0.0);
    vec3 toObserver = -photonDir;

    float doppler = 1.0 / max(lorentz * (1.0 - beta * dot(flowDirection, toObserver)), 1e-3);
    const float kSpectralIndex = 0.7;
    float boost = pow(doppler, 2.0 + kSpectralIndex);

    // Gravitational redshift on the way out. Far up the jet this is ~1.
    float radius = max(length(position), rs * 1.0001);
    float gravity = sqrt(max(1.0 - rs / radius, 1e-4));
    boost *= pow(gravity, 2.0 + kSpectralIndex);

    // Blandford-Znajek: the power available scales as a*^2.
    float spinScaling = uJetScalesWithSpin != 0 ? spinStar * spinStar : 1.0;

    vec3 colour = blackbodyRgb(max(uJetTemperature, 1500.0) * clamp(doppler, 0.3, 3.0));

    // Overall scale. Synchrotron emissivity has no natural normalisation the
    // way a blackbody does -- there is no temperature to anchor it to -- so
    // this constant simply puts a side-on jet at a brightness comparable with
    // the disk at the default settings. It is a units choice; every *relative*
    // variation (the beaming asymmetry, the fall-off along the flow, the a*^2
    // scaling) is the physical one.
    const float kJetEmissivity = 45.0;
    return colour * density * boost * spinScaling * power * kJetEmissivity;
}

// Accumulate jet emission along one trajectory segment.
//
// Unlike the disk, the jet is optically thin -- it adds light but absorbs
// almost none -- so this only ever brightens the ray. It still respects the
// transmittance already accumulated by the disk, because an opaque disk in
// front genuinely does hide the outflow behind it.
void accumulateJetSegment(vec3 pointA, vec3 pointB, float rs, float spinStar,
                          inout TraceResult trace)
{
    if (uJetPower <= 0.0)
    {
        return;
    }

    vec3 delta = pointB - pointA;
    float segmentLength = length(delta);
    if (segmentLength < 1e-7)
    {
        return;
    }

    // Cheap rejection: if both endpoints are far outside the widest the jet
    // ever gets, nothing along the chord can be inside it either.
    float jetLength = max(uJetLength, 1.0);
    float widest = jetRadiusAtHeight(jetLength, max(uJetBaseRadius, 0.02), uJetCollimation) * 2.5;
    bool aOutside = abs(pointA.y) > jetLength || length(pointA.xz) > widest;
    bool bOutside = abs(pointB.y) > jetLength || length(pointB.xz) > widest;
    if (aOutside && bOutside)
    {
        return;
    }

    vec3 direction = delta / segmentLength;

    // Sample count follows the segment length measured against the jet's own
    // width. Far from the hole the integrator takes long strides, and a fixed
    // handful of samples there shows up as visible banding along the outflow.
    float scale = max(uJetBaseRadius, 0.05) * 3.0;
    int sampleCount = int(clamp(ceil(segmentLength / scale), 1.0, 8.0));
    float ds = segmentLength / float(sampleCount);
    for (int i = 0; i < 8; ++i)
    {
        if (i >= sampleCount) break;
        vec3 position = pointA + delta * ((float(i) + 0.5) / float(sampleCount));
        trace.diskRadiance += sampleJet(position, direction, rs, spinStar)
                            * trace.transmittance * ds;
    }
}

// =============================================================================
// Volumetric integration of the disk along one trajectory segment
// =============================================================================
//
// Between two consecutive RK4 points the trajectory is nearly straight, so we
// treat it as a chord and march a few samples along whatever part of it lies
// inside the disk's vertical slab, applying the emission/absorption transfer
// equation:
//
//     radiance      += transmittance * emission * ds
//     transmittance *= exp(-extinction * ds)
//
// With a large uDiskOpacity this converges to an opaque thin disk; with a small
// one the disk glows through itself, which is what produces the soft layered
// look of the multiple lensed images.
void accumulateDiskSegment(
    vec3 pointA, vec3 pointB,
    float rs, float innerRadius, float outerRadius, float slabHalfHeight,
    inout TraceResult trace)
{
    vec3 delta = pointB - pointA;
    float segmentLength = length(delta);
    if (segmentLength < 1e-7)
    {
        return;
    }

    // Clip the chord to the vertical slab |y| <= slabHalfHeight.
    float tEnter = 0.0;
    float tExit  = 1.0;
    float dy = delta.y;
    if (abs(dy) < 1e-9)
    {
        if (abs(pointA.y) > slabHalfHeight) return;
    }
    else
    {
        float t0 = (-slabHalfHeight - pointA.y) / dy;
        float t1 = ( slabHalfHeight - pointA.y) / dy;
        tEnter = max(tEnter, min(t0, t1));
        tExit  = min(tExit,  max(t0, t1));
        if (tEnter >= tExit) return;
    }

    // Cheap conservative rejection: if both ends of the clipped chord are well
    // outside the disk, nothing along it can be inside either.
    vec3 clippedA = pointA + delta * tEnter;
    vec3 clippedB = pointA + delta * tExit;
    if (min(length(clippedA.xz), length(clippedB.xz)) > outerRadius) return;

    // Choose the sample count from how much length actually lies inside the
    // slab relative to the disk thickness, so an edge-on ray skimming along the
    // disk gets more samples than one punching straight through it.
    float insideLength = segmentLength * (tExit - tEnter);
    int sampleCount = int(clamp(ceil(insideLength / max(slabHalfHeight * 0.5, 1e-3)), 1.0, 6.0));
    float dt = (tExit - tEnter) / float(sampleCount);
    float ds = segmentLength * dt;

    for (int i = 0; i < 6; ++i)
    {
        if (i >= sampleCount) break;

        float t = tEnter + (float(i) + 0.5) * dt;
        vec3 position = pointA + delta * t;

        // The chord direction is the ray's propagation direction to within the
        // step size, which is small by construction near the disk.
        DiskSample s = sampleDisk(position, delta / segmentLength, rs, innerRadius, outerRadius);
        if (s.extinction <= 0.0 && s.radiance == vec3(0.0))
        {
            continue;
        }

        float weight = trace.transmittance * ds;
        trace.diskRadiance += s.radiance * weight;

        float dTau = s.extinction * ds;
        trace.opticalDepth += dTau;
        trace.transmittance *= exp(-dTau);

        // Remember the shift values where this ray picked up the most light, so
        // the debug views can show one meaningful number per pixel.
        float contribution = dot(s.radiance, vec3(0.2126, 0.7152, 0.0722)) * weight;
        if (contribution > trace.peakWeight)
        {
            trace.peakWeight   = contribution;
            trace.shiftTotal   = s.shiftTotal;
            trace.shiftDoppler = s.shiftDoppler;
            trace.shiftGravity = s.shiftGravity;
        }

        if (trace.transmittance < 0.002)
        {
            return;
        }
    }
}

// =============================================================================
// KERR SPACETIME -- a rotating black hole, in Kerr-Schild coordinates
// =============================================================================
//
// Everything above this point assumes spherical symmetry, which is what let the
// Schwarzschild solver collapse a 4-D geodesic problem into one ODE in a single
// plane. Rotation destroys that: frame dragging twists a ray out of any plane
// you try to confine it to, so the trajectory genuinely needs three spatial
// degrees of freedom.
//
//
// WHY KERR-SCHILD AND NOT BOYER-LINDQUIST
// ---------------------------------------
// Boyer-Lindquist is the textbook chart for Kerr, and it is the natural first
// choice. It has two coordinate pathologies, though, and both bite here:
//
//   * The spin axis. The equations carry terms in L^2/sin^2(theta) and
//     cot(theta) which blow up at theta = 0 and pi. The *spacetime* is
//     perfectly smooth there -- only the chart is broken -- but the integrator
//     does not know that, and rays passing near the axis come out inaccurate.
//     It shows up as a thin bright seam running straight along the axis.
//   * The horizon, where Delta = 0 and the radial equation degenerates.
//
// Kerr-Schild has neither. It is written in ordinary Cartesian-like coordinates
// and stays regular everywhere outside the ring singularity, which is why
// production general-relativistic ray tracers use it.
//
//
// THE METRIC
// ----------
// Kerr-Schild writes the metric as flat spacetime plus a rank-one correction
// built from a single null vector:
//
//     g_mu_nu = eta_mu_nu + f * l_mu * l_nu
//     g^mu^nu = eta^mu^nu - f * l^mu * l^nu
//
// with eta = diag(-1, 1, 1, 1), and
//
//     f   = 2 M r^3 / (r^4 + a^2 z^2)
//     l_mu = ( 1, (r x + a y)/(r^2+a^2), (r y - a x)/(r^2+a^2), z / r )
//
// Here r is *not* the Euclidean radius. It is the Boyer-Lindquist radial
// coordinate, defined implicitly by
//
//     (x^2 + y^2)/(r^2 + a^2) + z^2/r^2 = 1
//
// whose surfaces of constant r are oblate spheroids. Solving that quadratic in
// r^2 gives the closed form used below. Note that l_mu is null with respect to
// *both* eta and g, and that the inverse metric needs no matrix inversion at
// all -- it is the same expression with the sign of f flipped.
//
// A convenient identity keeps the algebra short. Writing rho^2 = x^2+y^2+z^2,
//
//     W = 2 r^2 - rho^2 + a^2 = r^2 + a^2 z^2 / r^2 = (r^4 + a^2 z^2) / r^2
//
// so that f = 2 M r / W, and W > 0 everywhere. W also appears in every
// derivative of r, which is what makes the analytic gradients tractable:
//
//     dr/dx = r x / W,   dr/dy = r y / W,   dr/dz = z (r^2 + a^2) / (r W)
//
//
// EQUATIONS OF MOTION
// -------------------
// Hamilton's equations for the photon Hamiltonian 2H = g^mu^nu p_mu p_nu = 0.
// The metric is independent of t, so p_t = -E is conserved and only the three
// spatial positions and momenta evolve. Substituting the Kerr-Schild form and
// writing S = E + l.p for the recurring contraction:
//
//     dx^i/dlambda = p_i - f S l_i
//     dp_i/dlambda = (1/2) S^2 df/dx^i + f S (dl_j/dx^i) p_j
//
// Both right-hand sides are smooth on the axis and across the horizon.

// The Boyer-Lindquist radial coordinate at a Cartesian point, with the spin
// axis along local +Z.
float kerrSchildRadius(vec3 position, float a)
{
    float rho2 = dot(position, position);
    float term = rho2 - a * a;
    float r2 = 0.5 * (term + sqrt(max(term * term + 4.0 * a * a * position.z * position.z, 0.0)));
    return sqrt(max(r2, 1e-12));
}

// Outer horizon r+ = M (1 + sqrt(1 - a*^2)). In Kerr-Schild this is an ordinary
// surface, not a coordinate singularity; the integrator could cross it happily,
// but there is nothing to see inside so rays are terminated there.
float kerrHorizon(float mass, float spinStar)
{
    return mass * (1.0 + sqrt(max(1.0 - spinStar * spinStar, 0.0)));
}

// The scalar f and the null vector l at a point, plus the pieces needed to
// differentiate them.
struct KerrSchildField
{
    float r;
    float f;
    vec3  l;
    vec3  dr;   // gradient of r
    float W;    // 2r^2 - rho^2 + a^2
    float Q;    // r^2 + a^2
};

KerrSchildField kerrSchildField(vec3 position, float mass, float a)
{
    KerrSchildField field;
    float r = kerrSchildRadius(position, a);
    float rho2 = dot(position, position);

    field.r = r;
    field.W = max(2.0 * r * r - rho2 + a * a, 1e-8);
    field.Q = r * r + a * a;
    field.f = 2.0 * mass * r / field.W;

    field.l = vec3((r * position.x + a * position.y) / field.Q,
                   (r * position.y - a * position.x) / field.Q,
                   position.z / r);

    field.dr = vec3(r * position.x / field.W,
                    r * position.y / field.W,
                    position.z * field.Q / (r * field.W));
    return field;
}

// Right-hand sides of Hamilton's equations. `momentum` holds the covariant
// spatial components p_i; `energy` is E = -p_t, conserved.
void kerrSchildDerivatives(vec3 position, vec3 momentum, float energy,
                           float mass, float a,
                           out vec3 dPosition, out vec3 dMomentum)
{
    KerrSchildField field = kerrSchildField(position, mass, a);
    float f = field.f;
    vec3 l = field.l;
    vec3 dr = field.dr;
    float r = field.r;
    float Q = field.Q;
    float W = field.W;

    // S = E + l.p appears in both equations and in the null condition.
    float s = energy + dot(l, momentum);

    dPosition = momentum - f * s * l;

    // ---- gradient of f -----------------------------------------------------
    // f = 2 M r / W, with W = 2r^2 - rho^2 + a^2.
    vec3 dW = 4.0 * r * dr - 2.0 * position;
    vec3 df = 2.0 * mass * (dr * W - r * dW) / (W * W);

    // ---- gradient of l -----------------------------------------------------
    // dl_j/dx^i, laid out so that row i holds the derivatives with respect to
    // x^i of all three components of l.
    float q2 = Q * Q;
    float lxNum = r * position.x + a * position.y;
    float lyNum = r * position.y - a * position.x;

    // d(l_x)/dx^i
    vec3 dlx = vec3((dr.x * position.x + r) * Q - lxNum * 2.0 * r * dr.x,
                    (dr.y * position.x + a) * Q - lxNum * 2.0 * r * dr.y,
                    (dr.z * position.x)     * Q - lxNum * 2.0 * r * dr.z) / q2;
    // d(l_y)/dx^i
    vec3 dly = vec3((dr.x * position.y - a) * Q - lyNum * 2.0 * r * dr.x,
                    (dr.y * position.y + r) * Q - lyNum * 2.0 * r * dr.y,
                    (dr.z * position.y)     * Q - lyNum * 2.0 * r * dr.z) / q2;
    // d(l_z)/dx^i, with l_z = z / r
    vec3 dlz = vec3(-position.z * dr.x,
                    -position.z * dr.y,
                    r - position.z * dr.z) / (r * r);

    // G_i = (dl_j/dx^i) p_j
    vec3 g = vec3(dlx.x * momentum.x + dly.x * momentum.y + dlz.x * momentum.z,
                  dlx.y * momentum.x + dly.y * momentum.y + dlz.y * momentum.z,
                  dlx.z * momentum.x + dly.z * momentum.y + dlz.z * momentum.z);

    dMomentum = 0.5 * s * s * df + f * s * g;
}

// Classical RK4 on the six-dimensional state (position, momentum).
void kerrSchildStepRk4(inout vec3 position, inout vec3 momentum, float energy,
                       float h, float mass, float a)
{
    vec3 k1p, k1m, k2p, k2m, k3p, k3m, k4p, k4m;
    kerrSchildDerivatives(position, momentum, energy, mass, a, k1p, k1m);
    kerrSchildDerivatives(position + 0.5 * h * k1p, momentum + 0.5 * h * k1m, energy, mass, a, k2p, k2m);
    kerrSchildDerivatives(position + 0.5 * h * k2p, momentum + 0.5 * h * k2m, energy, mass, a, k3p, k3m);
    kerrSchildDerivatives(position + h * k3p,       momentum + h * k3m,       energy, mass, a, k4p, k4m);

    position += (h / 6.0) * (k1p + 2.0 * k2p + 2.0 * k3p + k4p);
    momentum += (h / 6.0) * (k1m + 2.0 * k2m + 2.0 * k3m + k4m);
}

// The renderer puts the disk's rotation axis along world +Y, while the
// Kerr-Schild formulae above are written with the spin along +Z. These two
// rotations convert between the conventions. Both are proper rotations
// (determinant +1) -- a mirror would silently reverse the sense of the spin.
vec3 worldToSpinFrame(vec3 v) { return vec3(v.x, -v.z, v.y); }
vec3 spinFrameToWorld(vec3 v) { return vec3(v.x, v.z, -v.y); }

// =============================================================================
// The ray tracer
// =============================================================================

// =============================================================================
// Weak-field shortcut
// =============================================================================
//
// Radius of closest approach, in closed form.
//
// The turning point is where the radial potential vanishes,
//
//     (dU/dphi)^2 = 1/B^2 - U^2 + U^3 = 0,   U = r_s/r,  B = b/r_s
//
// so U_p is a root of the cubic U^3 - U^2 + 1/B^2 = 0. Depressing it with
// U = w + 1/3 gives w^3 - w/3 + (1/B^2 - 2/27) = 0, which has three real roots
// whenever B is above the critical parameter, and the trigonometric form solves
// it without a single iteration:
//
//     U_k = 1/3 + (2/3) cos( theta/3 - 2 pi k / 3 ),  theta = acos(1 - 27/(2B^2))
//
// k = 0 is the root inside the horizon and k = 2 is negative; k = 1 is the
// periapsis. At the critical parameter B = 3 sqrt(3)/2 the argument of the
// arccosine is exactly -1 and the formula returns 1.5 r_s, the photon sphere,
// which is the correct limit. Below that there is no turning point at all --
// the clamp pins the result there, and since the caller only accepts radii far
// larger than 1.5 r_s, such a ray is rejected on its own.
float schwarzschildPeriapsis(float impactParameter, float rs)
{
    float b = max(impactParameter / max(rs, 1e-6), 1e-6);
    float theta = acos(clamp(1.0 - 13.5 / (b * b), -1.0, 1.0));
    const float kTwoThirdsPi = 2.0943951023931953;
    float turningU = 1.0 / 3.0 + (2.0 / 3.0) * cos(theta / 3.0 - kTwoThirdsPi);
    return rs / max(turningU, 1e-6);
}

// Does the segment [origin, origin + direction * travel] come within `radius` of
// the y axis at any point where |y| <= halfHeight?
//
// Deliberately conservative: it may answer "yes" for a segment that in fact
// misses, but never "no" for one that touches. Both volumes the ray has to be
// proven clear of -- the disk slab and the jet -- are cylinders about the y
// axis, so one test serves for both.
bool segmentTouchesAxialCylinder(vec3 origin, vec3 direction, float travel,
                                 float radius, float halfHeight)
{
    // Clip the segment to the |y| <= halfHeight slab first.
    float tEnter = 0.0;
    float tExit = travel;
    if (abs(direction.y) < 1e-9)
    {
        if (abs(origin.y) > halfHeight)
        {
            return false;
        }
    }
    else
    {
        float ta = (-halfHeight - origin.y) / direction.y;
        float tb = ( halfHeight - origin.y) / direction.y;
        tEnter = max(tEnter, min(ta, tb));
        tExit  = min(tExit,  max(ta, tb));
        if (tEnter >= tExit)
        {
            return false;
        }
    }

    // Then minimise the distance to the axis over what is left, which is one
    // clamped quadratic.
    vec2 axialOffset = origin.xz;
    vec2 axialStep = direction.xz;
    float stepLengthSq = dot(axialStep, axialStep);
    float t = stepLengthSq > 1e-12
                  ? clamp(-dot(axialOffset, axialStep) / stepLengthSq, tEnter, tExit)
                  : tEnter;
    return length(axialOffset + axialStep * t) <= radius;
}

// Distance from a point to the solid cylinder {rho <= radius, |y| <= halfHeight}
// about the y axis, and zero anywhere inside it.  The step schedule uses this to
// find out how far it may travel before it could possibly reach the disk or the
// jet.
float distanceToAxialCylinder(vec3 position, float radius, float halfHeight)
{
    vec2 outside = vec2(length(position.xz) - radius, abs(position.y) - halfHeight);
    return length(max(outside, vec2(0.0)));
}

TraceResult makeTrace(vec3 direction, float cameraRadius)
{
    TraceResult trace;
    trace.state = kTraceEscaped;
    trace.steps = 0;
    trace.minRadius = cameraRadius;
    trace.escapeDirection = direction;
    trace.diskRadiance = vec3(0.0);
    trace.transmittance = 1.0;
    trace.opticalDepth = 0.0;
    trace.peakWeight = 0.0;
    trace.shiftTotal = 1.0;
    trace.shiftDoppler = 1.0;
    trace.shiftGravity = 1.0;
    return trace;
}

TraceResult traceSchwarzschild(vec3 rayOrigin, vec3 initialDirection, bool ignoreDisk)
{
    float rs = configuredOr(uSchwarzschildRadius, 1.0);
    float cameraRadius = length(rayOrigin);
    TraceResult trace = makeTrace(initialDirection, cameraRadius);

    // A camera inside the horizon is outside this renderer's model.
    if (cameraRadius <= rs * 1.0001)
    {
        trace.state = kTraceCaptured;
        trace.minRadius = cameraRadius;
        return trace;
    }

    float innerRadius = configuredOr(uDiskInnerRadius, 3.0 * rs);   // ISCO default
    float outerRadius = max(configuredOr(uDiskOuterRadius, 18.0 * rs), innerRadius + 0.05 * rs);
    float baseHeight  = configuredOr(uDiskHalfThickness, 0.10 * rs);
    // Integrate the disk out to three scale heights: a Gaussian is negligible
    // beyond that. Using the *outer* scale height keeps a flared disk inside
    // the slab test everywhere.
    float slabHalfHeight = 3.0 * diskScaleHeight(outerRadius, innerRadius, baseHeight);
    float escapeRadius = max(configuredOr(uEscapeRadius, 80.0 * rs), cameraRadius * 1.25);

    // ---- Build the ray's own orbital plane ---------------------------------
    // radialBasis points from the black hole to the camera; azimuthBasis is the
    // in-plane direction the ray initially moves towards. phi is measured from
    // radialBasis, so the camera sits at phi = 0.
    vec3 radialBasis = rayOrigin / cameraRadius;
    float radialComponent = dot(initialDirection, radialBasis);
    vec3 tangent = initialDirection - radialComponent * radialBasis;
    float tangentLength = length(tangent);

    // Perfectly radial rays have no well-defined orbital plane (b -> 0). They
    // also do not bend at all, so handle them as straight lines.
    if (tangentLength < 1e-5)
    {
        bool inbound = radialComponent < 0.0;
        float travel = inbound ? max(cameraRadius - rs, 0.0) : max(escapeRadius - cameraRadius, 0.0);
        vec3 endpoint = rayOrigin + initialDirection * travel;
        if (!ignoreDisk)
        {
            // A purely radial ray carries no angular momentum at all, so its
            // axial impact parameter is exactly zero: it sees only the
            // gravitational redshift, with no Doppler term.
            accumulateDiskSegment(rayOrigin, endpoint,
                                  rs, innerRadius, outerRadius, slabHalfHeight, trace);
            accumulateJetSegment(rayOrigin, endpoint, rs, 0.0, trace);
        }
        trace.steps = 1;
        trace.state = inbound ? kTraceCaptured : kTraceEscaped;
        trace.minRadius = inbound ? rs : cameraRadius;
        trace.escapeDirection = initialDirection;
        return trace;
    }

    vec3 azimuthBasis = tangent / tangentLength;

    // ---- Initial conditions from the conserved impact parameter ------------
    // A static observer at radius r sees the ray leave at angle alpha from the
    // radial direction, and since the direction is a unit vector,
    // sin(alpha) = tangentLength. The impact parameter is
    //     b = r sin(alpha) / sqrt(1 - r_s/r)
    // The 1/sqrt(...) converts the *locally measured* angle into the globally
    // conserved quantity; omitting it is a classic subtle error.
    float u = rs / cameraRadius;
    float lapse = max(1.0 - u, 1e-5);
    float impactParameter = cameraRadius * tangentLength * inversesqrt(lapse);

    // Radial potential, equation (2): (du/dphi)^2 = (r_s/b)^2 - u^2 + u^3.
    float rsOverB = rs / max(impactParameter, 1e-6);
    float radialPotential = max(rsOverB * rsOverB - u * u + u * u * u, 0.0);
    // Outward-moving rays have decreasing u, hence v < 0 when radialComponent > 0.
    float v = -sign(radialComponent) * sqrt(radialPotential);
    if (abs(radialComponent) < 1e-6)
    {
        v = 0.0;
    }

    float phi = 0.0;
    float baseStep = clamp(configuredOr(uRayStep, 0.02), 0.0005, 0.12);
    // Kerr needs a bigger budget than Schwarzschild: the affine step is much
    // finer near the horizon and near the spin axis, so the same quality preset
    // buys fewer "useful" steps.
    int maxSteps = clamp(2 * (uMaxRaySteps > 0 ? uMaxRaySteps : 512), 1, kCompiledMaxRaySteps);
    float escapeU = rs / escapeRadius;

    // ---- Volumes the ray is sampled inside -----------------------------------
    // The disk and the jet are integrated *along* the trajectory, so both the
    // shortcut below and the step schedule inside the loop have to know where
    // they are: one to refuse to skip them, the other to refuse to step over
    // them.  The bounds are inflated to exactly what the near-disk test and
    // accumulateJetSegment already reject against, so "clear of the disk" means
    // the same thing everywhere in this function.
    float hazardDiskRadius = outerRadius * 1.15;
    float hazardDiskHeight = slabHalfHeight * 2.5;
    // The whole disk fits inside this ball, so the step schedule can keep
    // clear of the disk by watching u alone.  Expressed as a value of u,
    // since that is the variable being integrated.  A debug view that skips
    // the disk has no inbound limit short of the horizon.
    float hazardBallRadius = length(vec2(hazardDiskRadius, hazardDiskHeight));
    float inboundLimitU = ignoreDisk ? 1.0 : rs / max(hazardBallRadius, 1e-4);
    // Spin is zero on this path, so a jet that scales with a* is not drawn here
    // at all and is not a hazard.
    bool jetVisible = uJetPower > 0.0 && uJetScalesWithSpin == 0;
    float jetHalfLength = max(uJetLength, 1.0);
    float jetWidest =
        jetRadiusAtHeight(jetHalfLength, max(uJetBaseRadius, 0.02), uJetCollimation) * 2.5;

    // ---- Weak-field shortcut ------------------------------------------------
    // A ray that never comes near the hole barely bends, and stepping its
    // geodesic all the way out to the escape radius is the largest single waste
    // in a wide shot, where most of the frame is exactly that case.
    //
    // Everything needed to skip it is already known at this point. b is
    // conserved, so the closest approach follows in closed form, and once that
    // is a few tens of r_s the deflection is the textbook series
    //
    //     alpha = 2 (r_s/b) + (15 pi/16) (r_s/b)^2 + (16/3) (r_s/b)^3 + ...
    //
    // truncated here after the cubic term. At the default threshold the next
    // term is of order 1e-6 rad, some three orders of magnitude below one pixel.
    //
    // The shortcut may only be taken when the ray provably touches neither the
    // disk slab nor the jet, because both are sampled *along* the trajectory:
    // skipping the integration for a ray that crosses them would not speed the
    // disk up, it would delete its distant parts. Two independent tests are
    // tried and either is sufficient:
    //
    //   * the closest approach lying outside the ball that contains the whole
    //     disk. This is exact and needs no safety margin, because that closest
    //     approach is a true minimum of the radius over the entire path;
    //   * the straight-line segment missing the bounding cylinder inflated by
    //     the furthest the bending can carry the ray sideways, alpha * path
    //     length. That covers the case the ball test cannot certify -- a ray
    //     passing cleanly over or under the disk on its way past.
    //
    // Only the escape direction is approximated. Nothing here is used for rays
    // that reach the disk, the jet or the horizon, so no image feature is drawn
    // from the approximation; the shortcut only decides which patch of distant
    // sky a ray that was always going to miss everything ends up sampling.
    float weakFieldRadius = max(uWeakFieldRadius, 0.0) * rs;
    if (weakFieldRadius > 0.0)
    {
        float periapsisRadius = schwarzschildPeriapsis(impactParameter, rs);
        // An outbound ray never reaches its periapsis: it is already receding,
        // so the camera itself is the closest it ever gets.
        float closestRadius = radialComponent < 0.0
                                  ? min(periapsisRadius, cameraRadius)
                                  : cameraRadius;

        if (closestRadius > weakFieldRadius)
        {
            // Total asymptotic bend, and the largest sideways displacement it
            // can produce over the whole traced path. The trajectory's tangent
            // turns by at most alpha in total, so its distance from the initial
            // straight line grows by at most alpha per unit length.
            float inverseB = rs / max(impactParameter, 1e-6);
            float totalDeflection =
                inverseB * (2.0 + inverseB * (2.9452431 + inverseB * 5.3333333));
            float pathLength = escapeRadius + cameraRadius;
            float margin = totalDeflection * pathLength;

            // The disk is bounded by the cylinder rho <= outerRadius,
            // |y| <= slabHalfHeight, which sits inside a ball of this radius.
            float diskBallRadius = length(vec2(outerRadius, slabHalfHeight));
            bool clearOfDisk = ignoreDisk
                            || closestRadius > diskBallRadius
                            || !segmentTouchesAxialCylinder(rayOrigin, initialDirection, pathLength,
                                                            outerRadius + margin,
                                                            slabHalfHeight + margin);

            bool clearOfJet = ignoreDisk
                           || !jetVisible
                           || !segmentTouchesAxialCylinder(rayOrigin, initialDirection, pathLength,
                                                           jetWidest + margin,
                                                           jetHalfLength + margin);

            if (clearOfDisk && clearOfJet)
            {
                // Distribute the bend along the path the way the leading order
                // does. Writing x for the distance along the straight ray from
                // its closest approach, the deflection accumulated up to that
                // point is (alpha/2)(x/r + 1), so between the camera and the
                // escape radius it is (alpha/2)(x_end/r_end - x_cam/r_cam).
                // x_cam/r_cam is exactly radialComponent, and at the far end the
                // ray is outbound, so x_end/r_end is the positive root.
                float flatImpact = cameraRadius * tangentLength;
                float sineAtEscape = min(flatImpact / escapeRadius, 1.0);
                float cosineAtEscape = sqrt(max(1.0 - sineAtEscape * sineAtEscape, 0.0));
                float bend = max(0.5 * totalDeflection * (cosineAtEscape - radialComponent), 0.0);

                // No coordinate conversion here any more: directionInOrbitPlane
                // now reports what a static observer measures, which is what the
                // rotation below already produces. The two agree by construction.
                // Unit vector perpendicular to the ray, pointing at the hole:
                // the direction the deflection turns towards.
                vec3 inward = -tangentLength * radialBasis + radialComponent * azimuthBasis;

                trace.state = kTraceEscaped;
                trace.steps = 1;
                trace.minRadius = closestRadius;
                trace.escapeDirection =
                    normalize(initialDirection * cos(bend) + inward * sin(bend));
                return trace;
            }
        }
    }

    vec3 previousPosition = rayOrigin;
    float previousPhi = phi;
    float previousU = u;
    float previousV = v;

    for (int stepIndex = 0; stepIndex < kCompiledMaxRaySteps; ++stepIndex)
    {
        if (stepIndex >= maxSteps)
        {
            break;
        }

        // ---- Adaptive angular step -----------------------------------------
        // Three competing requirements:
        //  1. Curvature. The (3/2)u^2 term grows quickly near the hole, so the
        //     step shrinks as u rises. (u = 2/3 is the photon sphere.)
        //  2. Straightness. Out where that term is negligible the equation is
        //     just u'' + u = 0 and the step can open up a long way, which is
        //     where most of the budget was previously going to waste.
        //  3. Disk sampling. When the ray is inside the disk's radial range and
        //     close to its plane, the chord length ~ r*h must stay small next to
        //     the disk thickness or thin structure gets stepped over.
        float inverseU = 1.0 / max(u, 1e-6);
        float radius = rs * inverseU;
        float curvature = smoothstep(0.12, 0.95, u);
        float h = baseStep * mix(1.0, 0.28, curvature);

        if (uRayStepGrowth > 0.0)
        {
            // Growth is a multiplier on the curvature schedule rather than an
            // alternative to it, so opening the step up far away can never undo
            // the shrink near the hole.
            float opened = min(h * (1.0 + uRayStepGrowth * max(inverseU - 1.0, 0.0)),
                               max(uRayStepMax, baseStep));

            // A long step must not cross either landmark in u.
            //
            //   * Falling inwards, that is the ball containing the whole disk.
            //     Staying outside the ball is *sufficient* to miss the disk, so
            //     bounding the change in r is enough and the sideways motion
            //     need not be considered at all.
            //   * Receding, it is the escape radius. The direction handed to the
            //     starfield is read off wherever the ray lands, so a long final
            //     step would report it from far outside uEscapeRadius and shift
            //     every background star.
            //
            // Both are thresholds on u itself and du/dphi is v, so the whole
            // test is one divide -- no square root, no position, and nothing
            // that varies within a warp. That matters: this runs on every step
            // of every ray, including the ones near the hole that can never
            // benefit from it.
            float outsideBall = inboundLimitU - u;      // positive only outside it
            // 0.9 rather than 1, because du over a step is v*h only to first
            // order and the curvature term adds to it.
            opened = min(opened, 0.9 * outsideBall / max(v, 1e-6));

            // Inside that ball the ray can cross the disk slab on any step,
            // travelling in either direction -- coming back out through the
            // slab is just as easy as falling in through it -- so the step
            // never grows there at all and the schedule above stands unchanged.
            opened = outsideBall > 0.0 ? opened : h;

            if (jetVisible)
            {
                // The jet is the one hazard that is not contained in a ball
                // about the origin -- it is a tall thin cylinder, and a receding
                // ray can still drift into it sideways. It is only ever drawn on
                // this path when the Blandford-Znajek coupling is switched off,
                // and uJetScalesWithSpin is a uniform, so this branch costs
                // nothing whenever the jet is absent.
                float roomToJet = distanceToAxialCylinder(previousPosition, jetWidest,
                                                          jetHalfLength);
                float radialRate = rs * abs(v) * inverseU * inverseU;   // |dr/dphi|
                float chordPerRadian = sqrt(radialRate * radialRate + radius * radius);
                opened = min(opened, 0.5 * roomToJet / max(chordPerRadian, 1e-6));
            }

            // Growth may only ever open the step, never close it.
            h = max(h, opened);
        }

        if (!ignoreDisk)
        {
            bool nearDisk = radius < hazardDiskRadius
                         && abs(previousPosition.y) < hazardDiskHeight;
            if (nearDisk)
            {
                // chord ~ radius * h, so h ~ desiredChord / radius.
                float desiredChord = max(slabHalfHeight * 0.45, 0.02 * rs);
                h = min(h, max(desiredChord / max(radius, 1e-4), baseStep * 0.12));
            }
        }

        // Explicit bounds. The floor is the one the disk cap already used; the
        // ceiling is an absolute accuracy limit, not a quality setting, because
        // what it protects against is under-resolving the sinusoid that u(phi)
        // becomes far from the hole. It is held at or above baseStep so it can
        // only ever restrain the growth, never override --quality.
        h = clamp(h, baseStep * 0.12, max(uRayStepMax, baseStep));

        // Aim the last step at the escape radius exactly.
        //
        // The loop below stops once u has fallen past escapeU, and hands the
        // starfield the direction at whatever u the step happened to land on --
        // which depends on the step size, and so on the whole history of the
        // schedule. Every ray therefore reported its direction from a slightly
        // different radius, and any change to the stepping moved all of them.
        // On a starfield that is the most visible error there is: a background
        // star is a delta function, and shifting it by a fraction of a pixel
        // swings a channel by a third of its range.
        //
        // Landing on escapeU deliberately removes that coupling. It also makes
        // uEscapeRadius mean what its name says: the radius the direction is
        // actually reported from, rather than an approximate lower bound on it.
        // The floor is insurance -- a step that only ever approached escapeU
        // asymptotically would never cross it and would burn the whole budget --
        // though in practice dv/dphi = (3/2)u^2 - u is negative out here, so the
        // aimed step always overshoots slightly and crosses on the first try.
        if (v < 0.0)
        {
            h = min(h, max((u - escapeU) / max(-v, 1e-6), baseStep * 0.12));
        }

        integrateGeodesicRk4(u, v, h);
        phi += h;
        trace.steps = stepIndex + 1;

        // Numerical overshoot past u = 0 means the ray has definitely escaped.
        if (u <= 0.0)
        {
            trace.state = kTraceEscaped;
            trace.escapeDirection = directionInOrbitPlane(phi, max(escapeU, 1e-5), v, rs, radialBasis, azimuthBasis);
            return trace;
        }

        vec3 currentPosition = positionInOrbitPlane(phi, u, rs, radialBasis, azimuthBasis);
        float currentRadius = length(currentPosition);
        trace.minRadius = min(trace.minRadius, currentRadius);

        // The disk is sampled *before* the termination tests: one step can cross
        // both a bright part of the disk and the horizon.
        if (!ignoreDisk)
        {
            accumulateDiskSegment(previousPosition, currentPosition,
                                  rs, innerRadius, outerRadius, slabHalfHeight, trace);
            // Spin is zero on this path, so the jet only appears here if the
            // Blandford-Znajek coupling has been switched off in the UI.
            accumulateJetSegment(previousPosition, currentPosition, rs, 0.0, trace);
            if (trace.transmittance < 0.002)
            {
                // Fully absorbed: whatever lies beyond cannot be seen. Report
                // "escaped"; the shading stage multiplies the background by the
                // (now almost zero) transmittance anyway.
                trace.state = kTraceEscaped;
                trace.escapeDirection = directionInOrbitPlane(phi, u, v, rs, radialBasis, azimuthBasis);
                return trace;
            }
        }

        if (u >= 1.0 || currentRadius <= rs)
        {
            trace.state = kTraceCaptured;
            trace.minRadius = min(trace.minRadius, rs);
            return trace;
        }

        if (u <= escapeU)
        {
            trace.state = kTraceEscaped;
            trace.escapeDirection = directionInOrbitPlane(phi, u, v, rs, radialBasis, azimuthBasis);
            return trace;
        }

        previousPosition = currentPosition;
        previousPhi = phi;
        previousU = u;
        previousV = v;
    }

    // ---- Budget exhausted --------------------------------------------------
    // Rays whose impact parameter is very close to the critical value
    // b_crit = (3*sqrt(3)/2) r_s wind many times around the photon sphere before
    // committing. Rather than flicker, extrapolate from the current radial
    // motion: v > 0 means u is still growing, i.e. the ray is spiralling in.
    trace.state = v > 0.0 ? kTraceCaptured : kTraceExhausted;
    trace.escapeDirection = directionInOrbitPlane(phi, max(u, 1e-5), v, rs, radialBasis, azimuthBasis);
    return trace;
}

// =============================================================================
// The Kerr ray tracer
// =============================================================================
//
// Same job as traceSchwarzschild, but integrating the full six-variable
// Hamiltonian system in Kerr-Schild coordinates instead of one planar ODE. The
// step is taken in the affine parameter, because with rotation there is no
// single orbital plane whose angle could serve as one.

// Direction a static observer at this point measures for the photon, as a unit
// vector in that observer's own orthonormal frame.
//
// The integrator carries dx^i/dlambda, which is a *coordinate* velocity, while
// the starfield is sampled as a flat sky -- handing over the coordinate velocity
// asks two different geometries to agree. The gap is only O(r_s/r), but at the
// default escape radius that is more than a pixel at 720p.
//
// A static observer has no spatial velocity, so the coordinate components of the
// direction it measures are just p^i; all that remains is to leave the spatial
// metric. Its rest space carries h_ij = delta_ij + [f/(1-f)] l_i l_j, so an
// orthonormal frame is reached by stretching the component along l by
// 1/sqrt(1-f) and leaving the perpendicular ones alone.
//
// Static is the right frame here rather than merely a consistent one: the sky is
// at rest at infinity, so a direction meant to index it has to be read in a
// frame at rest too. It is also the frame the camera end uses and the frame the
// planar solver uses, which is what lets the two solvers agree.
vec3 kerrObservedDirection(vec3 position, vec3 dPosition, float mass, float a)
{
    KerrSchildField field = kerrSchildField(position, mass, a);
    float staticNorm = 1.0 - field.f;
    if (staticNorm <= 1.0e-4)
    {
        // Inside the ergosphere there is no static frame to read. Such a ray is
        // on its way through the horizon and its background is multiplied out
        // anyway, so the raw coordinate velocity is a good enough placeholder.
        return normalize(spinFrameToWorld(dPosition));
    }
    vec3 measured = dPosition
                  + (inversesqrt(staticNorm) - 1.0) * dot(field.l, dPosition) * field.l;
    return normalize(spinFrameToWorld(measured));
}

// =============================================================================
// The Kerr tracer, in three parts
// =============================================================================
//
// The integration is split so that it can be *paused*.  traceKerr below still
// runs it start to finish in one call and is what the fragment and compute
// paths use; the wavefront path calls kerrAdvance repeatedly with a small step
// budget, parking the state in a buffer in between, so that rays which finish
// early stop occupying lanes in a warp alongside rays that need a thousand
// steps.
//
// Nothing about the trajectory changes. The only thing the split has to get
// right is that everything carried between steps lives in KerrRay or
// TraceResult, and it does: position and momentum are the integrator's whole
// state, and previousWorld is by construction the world position of the step
// that just finished, so a chunk can rebuild it from position alone.

struct KerrRay
{
    vec3 position;   // Kerr-Schild, spin frame
    vec3 momentum;   // spin frame, energy normalised to 1
};

// Initial conditions. `done` comes back true when the ray is over before it
// starts, which happens only for a camera inside the horizon.
KerrRay kerrBegin(vec3 rayOrigin, vec3 initialDirection, out TraceResult trace, out bool done)
{
    KerrRay ray;
    ray.position = vec3(0.0);
    ray.momentum = vec3(0.0);
    done = false;
    float rs = configuredOr(uSchwarzschildRadius, 1.0);
    float mass = 0.5 * rs;
    float spinStar = clamp(uSpin, -0.998, 0.998);
    float a = spinStar * mass;

    float cameraRadius = length(rayOrigin);
    trace = makeTrace(initialDirection, cameraRadius);

    float horizon = kerrHorizon(mass, spinStar);
    if (cameraRadius <= horizon * 1.02)
    {
        trace.state = kTraceCaptured;
        trace.minRadius = cameraRadius;
        done = true;
        return ray;
    }

    // ---- Initial conditions ------------------------------------------------
    // Work in the spin frame, where the rotation axis is +Z.
    vec3 position = worldToSpinFrame(rayOrigin);
    vec3 rayDirection = worldToSpinFrame(initialDirection);

    KerrSchildField field = kerrSchildField(position, mass, a);

    // ---- Which observer is holding the camera ------------------------------
    // The camera sits at a fixed coordinate position and looks at the hole: it
    // is *static*, and the ray direction handed in is what a static observer
    // measures in its own orthonormal frame. That is also exactly what the
    // planar solver assumes, and it is the reason the two now agree in the
    // limit -- see below.
    //
    // A static observer has u^mu = (1/sqrt(1-f), 0, 0, 0), which is timelike
    // only while f < 1, that is, outside the ergosphere. Its rest space carries
    // the induced metric
    //     h_ij = delta_ij + [f/(1-f)] l_i l_j
    // (found by imposing g(u, e) = 0 on e = A d_t + V^i d_i, which fixes
    // A = [f/(1-f)] l.V and leaves that norm on V). So the coordinate
    // components of a direction come from *undoing* the stretch along l, and
    // the photon is p^mu = u^mu + N^mu with N = (A, V).
    //
    // At a = 0 this reproduces b = r sin(alpha)/sqrt(1 - r_s/r) exactly, which
    // is the planar solver's own expression: the conserved energy comes out as
    // sqrt(1 - r_s/r) and the angular momentum as r sin(alpha).
    float lDotD = dot(field.l, rayDirection);
    float pTimeUp;
    vec3 pSpaceUp;

    float staticNorm = 1.0 - field.f;   // -g_tt, positive outside the ergosphere
    if (staticNorm > 1.0e-4)
    {
        float stretch = field.f / staticNorm;
        vec3 spaceDir = rayDirection
                      - (1.0 - inversesqrt(1.0 + stretch)) * lDotD * field.l;
        pTimeUp = inversesqrt(staticNorm) + stretch * dot(field.l, spaceDir);
        pSpaceUp = spaceDir;
    }
    else
    {
        // Inside the ergosphere no observer can stand still, so the frame falls
        // back to the *normal* (Eulerian) observer of the 3+1 split, which stays
        // well defined everywhere outside the horizon:
        //     u^mu = sqrt(1+f) * ( 1, -f l / (1+f) )
        // The image is then aberrated by that observer's infall, but it exists,
        // which the static frame does not.
        float onePlusF = 1.0 + field.f;
        float lapse = sqrt(onePlusF);
        float dNorm2 = dot(rayDirection, rayDirection) + field.f * lDotD * lDotD;
        pTimeUp = lapse;
        pSpaceUp = -lapse * field.f * field.l / onePlusF
                 + rayDirection * inversesqrt(max(dNorm2, 1e-12));
    }

    // Lower the indices with g_mu_nu = eta_mu_nu + f l_mu l_nu, remembering
    // l_t = 1 so the contraction l_mu p^mu is (p^t + l.p^i).
    float lDotP = pTimeUp + dot(field.l, pSpaceUp);
    float pTimeDown = -pTimeUp + field.f * lDotP;            // p_t
    vec3 momentum = pSpaceUp + field.f * lDotP * field.l;    // p_i

    // Normalise to E = 1 so the conserved impact parameter is read off directly.
    float energy = -pTimeDown;
    float invEnergy = 1.0 / max(abs(energy), 1e-6);
    momentum *= invEnergy;
    energy = 1.0;

    ray.position = position;
    ray.momentum = momentum;
    return ray;
}

// The extrapolation used when a ray runs out of its step budget: rays very close
// to the critical impact parameter would otherwise flicker between outcomes.
void kerrFinish(inout KerrRay ray, inout TraceResult trace)
{
    float rs = configuredOr(uSchwarzschildRadius, 1.0);
    float mass = 0.5 * rs;
    float spinStar = clamp(uSpin, -0.998, 0.998);
    float a = spinStar * mass;

    vec3 position = ray.position;
    vec3 momentum = ray.momentum;
    float energy = 1.0;
    vec3 dPosition = vec3(0.0);
    vec3 dMomentum = vec3(0.0);
    // Budget exhausted. As in the Schwarzschild solver, extrapolate from the
    // current radial motion rather than flickering: still falling inwards means
    // the ray was almost certainly going to be captured.
    kerrSchildDerivatives(position, momentum, energy, mass, a, dPosition, dMomentum);
    float radialRate = dot(normalize(position), dPosition);
    trace.state = radialRate < 0.0 ? kTraceCaptured : kTraceExhausted;
    trace.escapeDirection = kerrObservedDirection(position, dPosition, mass, a);
}

// Advances the ray by at most `budget` steps. Returns true once it is finished:
// captured, escaped, fully absorbed, or out of its global budget.
bool kerrAdvance(inout KerrRay ray, inout TraceResult trace, bool ignoreDisk, int budget)
{
    float rs = configuredOr(uSchwarzschildRadius, 1.0);
    float mass = 0.5 * rs;
    float spinStar = clamp(uSpin, -0.998, 0.998);
    float a = spinStar * mass;

    float horizon = kerrHorizon(mass, spinStar);
    float cameraRadius = length(uCameraPosition);
    float innerRadius = configuredOr(uDiskInnerRadius, 3.0 * rs);
    float outerRadius = max(configuredOr(uDiskOuterRadius, 18.0 * rs), innerRadius + 0.05 * rs);
    float baseHeight  = configuredOr(uDiskHalfThickness, 0.10 * rs);
    float slabHalfHeight = 3.0 * diskScaleHeight(outerRadius, innerRadius, baseHeight);
    float escapeRadius = max(configuredOr(uEscapeRadius, 80.0 * rs), cameraRadius * 1.25);

    // Kerr needs a bigger budget than Schwarzschild: the affine step is much
    // finer near the horizon, so the same quality preset buys fewer steps.
    int maxSteps = clamp(2 * (uMaxRaySteps > 0 ? uMaxRaySteps : 512), 1, kCompiledMaxRaySteps);
    // uRayStep is an angular step for the Schwarzschild solver; here it only
    // scales the adaptive affine step, normalised so the default lands at 1.
    float qualityScale = clamp(configuredOr(uRayStep, 0.04) / 0.04, 0.05, 3.0);


    vec3 position = ray.position;
    vec3 momentum = ray.momentum;
    float energy = 1.0;
    // The previous point is where the last completed step left the ray, which is
    // exactly where it is now -- so a chunk boundary needs nothing carried over.
    // |position| exceeds r by at most a^2/2r, so a margin of a makes a test on r
    // a safe stand-in for one on |position|. Hoisted: it is loop-invariant.
    float escapeGate = escapeRadius - abs(a);

    vec3 previousWorld = spinFrameToWorld(position);
    vec3 dPosition = vec3(0.0);
    vec3 dMomentum = vec3(0.0);

    for (int chunkStep = 0; chunkStep < kCompiledMaxRaySteps; ++chunkStep)
    {
        // Two bounds now, not one: the chunk this call was given, and the
        // global budget the ray has been spending across every chunk so far.
        if (chunkStep >= budget || trace.steps >= maxSteps)
        {
            break;
        }

        kerrSchildDerivatives(position, momentum, energy, mass, a, dPosition, dMomentum);

        // ---- Adaptive affine step ------------------------------------------
        // With no coordinate singularity to dodge, one criterion does the job:
        // never travel more than a fraction of the distance that remains to the
        // horizon, nor a fraction of the current radius. Both shrink the step
        // exactly where the trajectory bends hardest.
        float r = kerrSchildRadius(position, a);
        float gapToHorizon = max(r - horizon, 0.03 * mass);
        float speed = max(length(dPosition), 1e-6);
        float h = qualityScale * 0.25 * min(r, gapToHorizon) / speed;

        if (!ignoreDisk)
        {
            // Near the disk the chord must stay short compared with its
            // thickness, or thin structure is stepped straight over.
            vec3 world = spinFrameToWorld(position);
            bool nearDisk = r < outerRadius * 1.15 && abs(world.y) < slabHalfHeight * 2.5;
            if (nearDisk)
            {
                h = min(h, max(slabHalfHeight * 0.45, 0.02 * rs) / speed);
            }
        }
        h = clamp(h, 1e-4 * rs, 6.0 * max(r, rs));

        // Aim the last step at the escape radius exactly.
        //
        // Below, the ray is declared escaped wherever the step happened to leave
        // it past escapeRadius -- and the step is adaptive, a quarter of the
        // current radius, so that landing point depends on the entire history of
        // the schedule. Every ray was reporting its direction from a slightly
        // different radius, and any change to the stepping moved all of them.
        // On a starfield that is the most visible error the renderer can make.
        // It is the same fault the Schwarzschild solver had, fixed the same way.
        //
        // The trajectory is very nearly straight this far out, so where the
        // chord crosses the sphere is a quadratic worth solving outright rather
        // than creeping up on. position and dPosition are in the spin frame, but
        // spinFrameToWorld only permutes and negates axes, so radii and dot
        // products are the same in both.
        // The step can only cross the sphere if it is long enough to reach it,
        // and that rules it out on every step of a ray but the last. The test is
        // written against r, which is already in hand, rather than |position|,
        // which would cost another dot product on every step of every ray: the
        // two differ by at most a^2/2r, so escapeGate carries a margin of a and
        // the gate stays conservative. Where it does not fire, the aiming below
        // would have been a no-op anyway -- an unreachable sphere gives
        // toEscape > h.
        if (r + h * speed >= escapeGate)
        {
            float alongRadius = dot(position, dPosition);
            float outside = dot(position, position) - escapeRadius * escapeRadius;
            float discriminant = alongRadius * alongRadius - speed * speed * outside;
            if (outside < 0.0 && alongRadius > 0.0 && discriminant > 0.0)
            {
                float toEscape = (-alongRadius + sqrt(discriminant)) / (speed * speed);
                // Floored, so a step that only ever approaches the sphere cannot
                // burn the budget creeping towards it. The residual overshoot is
                // then under a thousandth of r_s however the ray arrived.
                h = min(h, max(toEscape, 1.0e-3 * rs / speed));
            }
        }

        kerrSchildStepRk4(position, momentum, energy, h, mass, a);
        trace.steps = trace.steps + 1;

        vec3 currentWorld = spinFrameToWorld(position);
        float currentRadius = kerrSchildRadius(position, a);
        trace.minRadius = min(trace.minRadius, currentRadius);

        if (!ignoreDisk)
        {
            accumulateDiskSegment(previousWorld, currentWorld,
                                  rs, innerRadius, outerRadius, slabHalfHeight, trace);
            accumulateJetSegment(previousWorld, currentWorld, rs, spinStar, trace);
            if (trace.transmittance < 0.002)
            {
                trace.state = kTraceEscaped;
                trace.escapeDirection = normalize(currentWorld - previousWorld);
                ray.position = position;
                ray.momentum = momentum;
                return true;
            }
        }

        if (currentRadius <= horizon * 1.001)
        {
            trace.state = kTraceCaptured;
            trace.minRadius = min(trace.minRadius, horizon);
            ray.position = position;
            ray.momentum = momentum;
            return true;
        }

        if (length(currentWorld) >= escapeRadius)
        {
            trace.state = kTraceEscaped;
            kerrSchildDerivatives(position, momentum, energy, mass, a, dPosition, dMomentum);
            trace.escapeDirection = kerrObservedDirection(position, dPosition, mass, a);
            ray.position = position;
            ray.momentum = momentum;
            return true;
        }

        previousWorld = currentWorld;
    }


    ray.position = position;
    ray.momentum = momentum;
    if (trace.steps >= maxSteps)
    {
        kerrFinish(ray, trace);
        return true;
    }
    return false;
}

TraceResult traceKerr(vec3 rayOrigin, vec3 initialDirection, bool ignoreDisk)
{
    TraceResult trace;
    bool done;
    KerrRay ray = kerrBegin(rayOrigin, initialDirection, trace, done);
    if (!done)
    {
        kerrAdvance(ray, trace, ignoreDisk, kCompiledMaxRaySteps);
    }
    return trace;
}

// =============================================================================
// Debug palettes
// =============================================================================

vec3 heatMap(float t)
{
    t = clamp(t, 0.0, 1.0);
    vec3 c0 = vec3(0.02, 0.01, 0.10);
    vec3 c1 = vec3(0.05, 0.20, 0.85);
    vec3 c2 = vec3(0.05, 0.95, 0.80);
    vec3 c3 = vec3(1.00, 0.82, 0.05);
    vec3 c4 = vec3(1.00, 0.08, 0.02);
    if (t < 0.25) return mix(c0, c1, t * 4.0);
    if (t < 0.50) return mix(c1, c2, (t - 0.25) * 4.0);
    if (t < 0.75) return mix(c2, c3, (t - 0.50) * 4.0);
    return mix(c3, c4, (t - 0.75) * 4.0);
}

// Red = redshifted (g < 1), grey = unshifted, blue = blueshifted (g > 1).
vec3 shiftVisualisation(float shift)
{
    float blue = smoothstep(1.0, 1.8, shift);
    float red = 1.0 - smoothstep(0.70, 1.0, shift);
    float neutral = 1.0 - max(blue, red);
    return red * vec3(1.0, 0.05, 0.01) + neutral * vec3(0.70) + blue * vec3(0.02, 0.32, 1.0);
}

// =============================================================================
// Ray generation and per-pixel shading
// =============================================================================

vec3 reconstructWorldRay(vec2 uv)
{
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 viewPosition = uInvProjection * vec4(ndc, 1.0, 1.0);
    vec3 viewDirection = normalize(viewPosition.xyz / max(viewPosition.w, 1e-6));
    return normalize((uInvView * vec4(viewDirection, 0.0)).xyz);
}

// Radical inverse in base 2 (van der Corput). Paired with a base-3 sequence
// this gives Halton points: a low-discrepancy pattern that covers the pixel far
// more evenly than uniform random jitter, so refinement converges faster.
float radicalInverse2(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float halton3(int index)
{
    float result = 0.0;
    float f = 1.0 / 3.0;
    int i = index + 1;
    for (int k = 0; k < 12; ++k)
    {
        if (i <= 0) break;
        result += f * float(i % 3);
        i /= 3;
        f /= 3.0;
    }
    return result;
}

// Everything downstream of the trace: the background lookup, every debug view,
// the composite and the guides.
//
// It is separate because the wavefront path cannot run it where it traces. Both
// the starfield's filter width and the horizon guide's edge are screen-space
// derivatives, and in a compacted ray list an invocation's neighbours are
// unrelated pixels, so derivatives there would be meaningless. This runs in an
// image-space pass instead, where a workgroup is a tile of adjacent pixels and
// the derivatives mean what they always did.
vec3 shadeTraceResult(TraceResult trace)
{
    int debugMode = uDebugMode;
    // The background is only visible through whatever the disk did not absorb.
    vec3 environment = trace.state == kTraceCaptured ? vec3(0.0)
                                                     : sampleEnvironment(trace.escapeDirection);

    if (debugMode == 1 || debugMode == 8)
    {
        return environment;
    }
    if (debugMode == 2)
    {
        int budget = uMaxRaySteps > 0 ? uMaxRaySteps : 512;
        return heatMap(float(trace.steps) / float(max(budget, 1)));
    }
    if (debugMode == 3)
    {
        if (trace.state == kTraceCaptured)  return vec3(0.95, 0.03, 0.01);
        if (trace.state == kTraceExhausted) return vec3(0.95, 0.05, 0.95);
        if (trace.opticalDepth > 0.05)      return vec3(1.00, 0.75, 0.05);
        return vec3(0.05, 0.35, 1.0);
    }
    if (debugMode == 4)
    {
        return heatMap(1.0 - exp(-trace.opticalDepth));
    }
    if (debugMode == 5)
    {
        return trace.peakWeight > 0.0 ? shiftVisualisation(trace.shiftTotal) : vec3(0.0);
    }
    if (debugMode == 6)
    {
        return trace.peakWeight > 0.0 ? shiftVisualisation(trace.shiftDoppler) : vec3(0.0);
    }
    if (debugMode == 7)
    {
        // Closest approach of each ray, in Schwarzschild radii.
        //
        // The background is a log-scaled map of that distance; the two bright
        // bands mark rays that grazed the photon sphere (1.5 r_s) and the ISCO
        // (3 r_s). Nothing here is drawn as a circle: the rings appear because
        // the integrated trajectories really do turn around at those radii.
        //
        // The bands are deliberately wide. Approaching the critical impact
        // parameter, the closest approach converges on 1.5 r_s *logarithmically
        // slowly*, so the set of rays within a hair of it is a sub-pixel-thin
        // annulus -- a tight tolerance renders as nothing at all.
        float rs = configuredOr(uSchwarzschildRadius, 1.0);
        float normalizedMin = trace.minRadius / rs;

        vec3 base = heatMap(clamp(log(max(normalizedMin, 1.0)) / log(14.0), 0.0, 1.0)) * 0.45;
        if (trace.state == kTraceCaptured)
        {
            base = vec3(0.30, 0.02, 0.01);
        }
        float photonSphere = exp(-pow((normalizedMin - 1.5) / 0.22, 2.0));
        float isco         = exp(-pow((normalizedMin - 3.0) / 0.22, 2.0));
        return base + photonSphere * vec3(1.6, 0.55, 0.02)
                    + isco * vec3(0.05, 0.85, 1.5);
    }
    if (debugMode == 9)
    {
        return trace.diskRadiance;
    }

    vec3 colour = environment * trace.transmittance + trace.diskRadiance;

    if (uShowHorizonGuide != 0)
    {
        // Outlines the set of captured rays: the true shadow boundary.
        float captured = trace.state == kTraceCaptured ? 1.0 : 0.0;
        float edge = clamp(length(vec2(dFdx(captured), dFdy(captured))), 0.0, 1.0);
        colour = mix(colour, vec3(2.5, 0.10, 0.02), edge);
    }
    if (uShowPhotonSphereGuide != 0)
    {
        float rs = configuredOr(uSchwarzschildRadius, 1.0);
        float guide = exp(-pow((trace.minRadius / rs - 1.5) / 0.03, 2.0));
        colour += guide * vec3(1.5, 0.25, 0.02);
    }
    return colour;
}


vec3 shadeRay(vec2 uv, out TraceResult traceOut)
{
    vec3 rayOrigin = uCameraPosition;
    vec3 rayDirection = reconstructWorldRay(uv);

    int debugMode = uDebugMode;
    bool ignoreDisk = (debugMode == 1 || debugMode == 8);

    // Without spin the planar Schwarzschild solver is both exact and much
    // cheaper, so it is kept rather than folded into the general case. The two
    // agree in the limit; the branch is purely about performance.
    TraceResult trace = abs(uSpin) > 1e-4
                            ? traceKerr(rayOrigin, rayDirection, ignoreDisk)
                            : traceSchwarzschild(rayOrigin, rayDirection, ignoreDisk);
    traceOut = trace;
    return shadeTraceResult(trace);
}

// One pixel of the HDR pass.  `pixelUv` is the centre of the pixel in 0..1;
// the fragment entry point takes it from the interpolated vUv and the compute
// one derives it from gl_GlobalInvocationID.  Everything below this line is
// identical for both.
// Sub-pixel position of one sample. Each frame and each in-frame sample gets a
// distinct Halton index, so progressive refinement keeps filling in new
// positions. Shared with the wavefront generate pass, which has to produce
// exactly the same rays.
vec2 sampleUv(vec2 pixelUv, int sampleInFrame, int spp)
{
    vec2 texelSize = 1.0 / max(uResolution, vec2(1.0));
    int index = uSampleIndex * spp + sampleInFrame;
    vec2 offset = vec2(radicalInverse2(uint(index)), halton3(index)) - 0.5;
    return pixelUv + (uJitter + offset) * texelSize;
}

vec4 renderPixel(vec2 pixelUv)
{
    int spp = clamp(uSamplesPerFrame, 1, 8);

    vec3 accumulated = vec3(0.0);
    TraceResult lastTrace;
    for (int s = 0; s < 8; ++s)
    {
        if (s >= spp) break;
        accumulated += shadeRay(sampleUv(pixelUv, s, spp), lastTrace);
    }

    return vec4(max(accumulated / float(spp), vec3(0.0)), 1.0);
}

