#pragma once

#include <algorithm>
#include <cmath>

namespace bhs::physics {

// ---------------------------------------------------------------------------
// Landmark radii of a Schwarzschild black hole, expressed in Schwarzschild
// radii (r_s = 2GM/c^2).  Deriving them once here keeps the magic numbers out
// of the renderer and the UI.
// ---------------------------------------------------------------------------

// Innermost stable circular orbit: r_ISCO = 6GM/c^2 = 3 r_s.  Inside it no
// stable circular orbit exists, which is why a thin accretion disk normally
// terminates there.
constexpr float kIscoInRadii = 3.0f;

// Photon sphere: r_ph = 3GM/c^2 = 1.5 r_s.  Light can (unstably) orbit here.
constexpr float kPhotonSphereInRadii = 1.5f;

// Critical impact parameter b_crit = 3*sqrt(3)/2 * r_s ~= 2.598 r_s.  A distant
// observer sees the shadow with this *apparent* radius, noticeably larger than
// the horizon itself.  (Non-rotating case only; with spin the shadow is no
// longer a circle at all.)
constexpr float kCriticalImpactParameterInRadii = 2.59807621f;

// ---------------------------------------------------------------------------
// Kerr (rotating) landmark radii.
//
// These take the dimensionless spin a* = a/M = Jc/(GM^2), which runs from -1
// (maximally retrograde) through 0 (Schwarzschild) to +1 (maximally prograde).
// All of them are returned in units of M = GM/c^2 = r_s/2, which is the
// conventional way these formulae are written; multiply by r_s/2 for scene
// units.
//
// The extreme value |a*| = 1 is a coordinate singularity in several of these,
// so callers clamp slightly inside it.
// ---------------------------------------------------------------------------

// Outer event horizon: r+ = M (1 + sqrt(1 - a*^2)).
// It shrinks from 2M at a* = 0 to M at a* = 1.
[[nodiscard]] inline float kerrHorizonInMass(float spin) {
    const float a = std::clamp(spin, -0.9999f, 0.9999f);
    return 1.0f + std::sqrt(std::max(1.0f - a * a, 0.0f));
}

// Innermost stable circular orbit (Bardeen, Press & Teukolsky 1972).
// Prograde orbits reach much closer in: 6M at a* = 0, down to ~1.24M at
// a* = 0.998.  Retrograde orbits are pushed out to 9M at a* = 1.
[[nodiscard]] inline float kerrIscoInMass(float spin) {
    const float a = std::clamp(spin, -0.9999f, 0.9999f);
    const float oneMinus = std::cbrt(1.0f - a);
    const float onePlus = std::cbrt(1.0f + a);
    const float z1 = 1.0f + std::cbrt(1.0f - a * a) * (onePlus + oneMinus);
    const float z2 = std::sqrt(3.0f * a * a + z1 * z1);
    const float root = std::sqrt(std::max((3.0f - z1) * (3.0f + z1 + 2.0f * z2), 0.0f));
    // The sign selects prograde (-) or retrograde (+); a negative spin already
    // means "disk orbits against the hole", so the sign follows a*.
    return 3.0f + z2 - (a >= 0.0f ? root : -root);
}

// Equatorial circular *photon* orbit, the rotating analogue of the photon
// sphere: r_ph = 2M [1 + cos( (2/3) arccos(-a*) )] for a prograde photon.
[[nodiscard]] inline float kerrPhotonOrbitInMass(float spin) {
    const float a = std::clamp(spin, -0.9999f, 0.9999f);
    return 2.0f * (1.0f + std::cos((2.0f / 3.0f) * std::acos(-a)));
}

// Static limit / outer ergosphere boundary in the equatorial plane is exactly
// 2M whatever the spin.  Inside it nothing can stay at rest: frame dragging
// forces every observer to co-rotate.
constexpr float kErgosphereEquatorInMass = 2.0f;

enum class QualityPreset : int { Low, Medium, High, Ultra };

enum class DebugMode : int {
    FinalRender,
    RawLensing,
    RaySteps,
    CaptureClassification,
    DiskOpticalDepth,
    GravitationalShift,
    DopplerShift,
    ImportantRadii,
    BackgroundOnly,
    DiskOnly,
};

// Distances are in Schwarzschild-radius units, which makes the whole scene
// independent of any particular black-hole mass: the dimensionless ray dynamics
// are identical for a stellar-mass and a supermassive black hole.
struct BlackHoleParameters {
    // ---- Spacetime -------------------------------------------------------
    float schwarzschildRadius = 1.0f;

    // Dimensionless spin a* = Jc/(GM^2).  Zero is Schwarzschild and selects the
    // faster planar integrator; anything else switches the renderer to the full
    // Kerr geodesic solver.  Held just inside 1 because several Kerr formulae
    // are singular exactly at the extremal value; 0.998 is also the physical
    // "Thorne limit" that accretion is thought not to exceed.
    float spin = 0.85f;

    // ---- Ray integrator --------------------------------------------------
    float rayStep = 0.040f;   // Base RK4 angular increment dphi, in radians.
    int maxRaySteps = 420;    // Integration budget per ray.
    float escapeRadius = 90.0f;
    // Closest approach, in Schwarzschild radii, past which a ray is deflected
    // by the weak-field series instead of being integrated step by step.  It
    // buys nothing on its own: a ray also has to miss the disk and the jet
    // before the shortcut is allowed.  Raising it integrates more of the frame
    // exactly; 0 switches the shortcut off altogether, which is the honest way
    // to measure what it costs in accuracy.
    float weakFieldRadius = 25.0f;
    // How fast the angular step opens up as the ray gets further out, as extra
    // step per Schwarzschild radius of distance.  0 restores the flat schedule
    // where dphi never exceeds rayStep anywhere.
    float rayStepGrowth = 0.15f;
    // Absolute ceiling on dphi.  This is an accuracy limit rather than a
    // quality dial: far from the hole u(phi) is a sinusoid, and a step much
    // larger than this stops resolving it whatever rayStep says.
    float rayStepMax = 0.25f;

    // ---- Tracer backend ---------------------------------------------------
    // The ray-tracing pass exists as both a fragment and a compute shader, built
    // from one shared body so they cannot drift.  Both are compiled every run so
    // the two can be compared on a single build; this only chooses which one the
    // frame dispatches.  Fragment is the default because it is the path every
    // reference image was made with.
    bool useComputeTracer = false;
    // Workgroup size of the compute tracer.  Applied when the shaders are
    // compiled, so changing it in the UI needs a shader reload.
    int computeGroupX = 8;
    int computeGroupY = 8;

    // ---- Accretion disk --------------------------------------------------
    bool lockDiskToIsco = true;
    float diskInnerRadius = kIscoInRadii;
    float diskOuterRadius = 13.0f;
    float diskHalfThickness = 0.075f; // Scale height H at the inner edge.
    float diskFlare = 0.28f;         // H(r) grows as (r/r_in)^diskFlare.
    float diskBrightness = 8.5f;
    float diskTemperature = 4900.0f; // Peak *emitted* colour temperature, K.
    float diskDensity = 1.0f;
    float diskOpacity = 2.4f;        // Extinction per unit length.
    float diskTurbulence = 0.85f;
    float diskRotationDirection = 1.0f;
    float artisticOrbitSpeed = 0.65f;

    // ---- Relativistic jet -------------------------------------------------
    // A pair of collimated outflows along the spin axis.  Blandford-Znajek
    // extracts rotational energy from the hole itself, with power going as
    // a*^2, so by default the jet fades away entirely at zero spin.
    float jetPower = 0.26f;
    bool jetScalesWithSpin = true;   // Blandford-Znajek a*^2 scaling.
    float jetLength = 46.0f;         // How far the outflow is drawn.
    float jetBaseRadius = 0.13f;     // Width where it leaves the hole.
    float jetCollimation = 0.80f;    // Radius grows as height^collimation.
    float jetLorentz = 3.0f;         // Bulk Lorentz factor of the flow.
    float jetTemperature = 16000.0f; // Colour only; synchrotron is not thermal.
    float jetTurbulence = 0.7f;

    // ---- Accretion dynamics ----------------------------------------------
    // How far inside the ISCO the plunging gas keeps radiating, as a fraction
    // of the gap between the ISCO and the horizon.  Zero reproduces the
    // classical sharp-edged thin disk.
    float plungeFraction = 0.7f;
    // Radial drift speed of the accreting gas, as a fraction of the local
    // orbital speed.  Real thin disks accrete very slowly; this is what makes
    // the turbulence spiral inwards rather than circle forever.
    float accretionRate = 0.10f;

    // ---- Relativistic optics (1 = full physical strength) ----------------
    float dopplerStrength = 1.0f;
    float gravitationalShiftStrength = 1.0f;
    float beamingStrength = 1.0f;

    // ---- Environment -----------------------------------------------------
    float starDensity = 0.55f;
    float nebulaStrength = 0.8f;

    // ---- Sampling / anti-aliasing ---------------------------------------
    int samplesPerFrame = 1;             // Jittered rays per pixel per frame.
    bool progressiveRefinement = true;   // Average frames while nothing moves.
    int maxAccumulatedSamples = 256;
    bool freezeAnimationWhileRefining = true;

    // ---- Display ---------------------------------------------------------
    float exposure = 0.62f;
    float bloomThreshold = 1.15f;
    float bloomKnee = 0.7f;
    float bloomStrength = 0.44f;
    float bloomSampleScale = 1.25f;
    float bloomLevelBlend = 0.62f; // Mix weight per pyramid level; see the shader.
    int bloomLevels = 6;
    int toneMapper = 0;
    float renderScale = 1.0f;
    bool vsync = true;

    // ---- Debug -----------------------------------------------------------
    bool showHorizonGuide = false;
    bool showPhotonSphereGuide = false;
    DebugMode debugMode = DebugMode::FinalRender;
    QualityPreset qualityPreset = QualityPreset::High;

    // True when the renderer must use the full Kerr solver rather than the
    // faster planar Schwarzschild one.
    [[nodiscard]] bool isRotating() const { return std::abs(spin) > 1e-4f; }

    // Mass in scene units.  M = GM/c^2 = r_s / 2; most Kerr formulae are
    // written in terms of it rather than r_s.
    [[nodiscard]] float mass() const { return 0.5f * schwarzschildRadius; }

    void sanitize() {
        schwarzschildRadius = std::clamp(schwarzschildRadius, 0.2f, 3.0f);
        spin = std::clamp(spin, -0.998f, 0.998f);
        rayStep = std::clamp(rayStep, 0.002f, 0.12f);
        maxRaySteps = std::clamp(maxRaySteps, 16, 2048);
        escapeRadius = std::clamp(escapeRadius, 20.0f, 400.0f);
        // Zero is meaningful (shortcut off); anything positive is held above
        // the photon sphere, below which the weak-field series is nonsense.
        weakFieldRadius = weakFieldRadius <= 0.0f ? 0.0f
                                                  : std::clamp(weakFieldRadius, 2.0f, 400.0f);
        rayStepGrowth = std::clamp(rayStepGrowth, 0.0f, 2.0f);
        // Never below rayStep: the ceiling restrains growth, it does not
        // override the quality preset.
        rayStepMax = std::clamp(rayStepMax, rayStep, 1.0f);
        // 1024 is the guaranteed minimum for the product of the local sizes.
        computeGroupX = std::clamp(computeGroupX, 1, 64);
        computeGroupY = std::clamp(computeGroupY, 1, 64);
        while (computeGroupX * computeGroupY > 1024) {
            computeGroupY = computeGroupY > 1 ? computeGroupY / 2 : computeGroupY;
            if (computeGroupY == 1 && computeGroupX > 1) computeGroupX /= 2;
        }

        if (lockDiskToIsco) {
            // The ISCO moves with the spin: prograde orbits reach far closer
            // to a rapidly rotating hole, which is what makes the inner disk
            // brighter and more strongly beamed at high spin.
            diskInnerRadius = iscoRadius();
        }
        // The disk must stay outside the horizon whatever the user asks for.
        // (The plunging region below the ISCO is handled in the shader, which
        // fades emission out as it approaches the horizon.)
        diskInnerRadius = std::max(diskInnerRadius, 1.05f * horizonRadius());
        diskOuterRadius = std::max(diskOuterRadius, diskInnerRadius + 0.25f);
        diskHalfThickness = std::clamp(diskHalfThickness, 0.005f, 2.0f);
        diskFlare = std::clamp(diskFlare, 0.0f, 2.0f);
        diskBrightness = std::clamp(diskBrightness, 0.0f, 40.0f);
        diskTemperature = std::clamp(diskTemperature, 1200.0f, 40000.0f);
        diskDensity = std::clamp(diskDensity, 0.0f, 8.0f);
        diskOpacity = std::clamp(diskOpacity, 0.0f, 40.0f);
        diskTurbulence = std::clamp(diskTurbulence, 0.0f, 1.0f);
        artisticOrbitSpeed = std::clamp(artisticOrbitSpeed, 0.0f, 5.0f);

        dopplerStrength = std::clamp(dopplerStrength, 0.0f, 1.0f);
        gravitationalShiftStrength = std::clamp(gravitationalShiftStrength, 0.0f, 1.0f);
        beamingStrength = std::clamp(beamingStrength, 0.0f, 1.0f);

        jetPower = std::clamp(jetPower, 0.0f, 8.0f);
        jetLength = std::clamp(jetLength, 2.0f, 400.0f);
        jetBaseRadius = std::clamp(jetBaseRadius, 0.05f, 8.0f);
        jetCollimation = std::clamp(jetCollimation, 0.0f, 1.2f);
        jetLorentz = std::clamp(jetLorentz, 1.0f, 30.0f);
        jetTemperature = std::clamp(jetTemperature, 1500.0f, 40000.0f);
        jetTurbulence = std::clamp(jetTurbulence, 0.0f, 1.0f);

        plungeFraction = std::clamp(plungeFraction, 0.0f, 1.0f);
        accretionRate = std::clamp(accretionRate, 0.0f, 0.9f);

        starDensity = std::clamp(starDensity, 0.0f, 3.0f);
        nebulaStrength = std::clamp(nebulaStrength, 0.0f, 4.0f);

        samplesPerFrame = std::clamp(samplesPerFrame, 1, 8);
        maxAccumulatedSamples = std::clamp(maxAccumulatedSamples, 1, 4096);

        exposure = std::clamp(exposure, 0.02f, 12.0f);
        bloomThreshold = std::clamp(bloomThreshold, 0.0f, 10.0f);
        bloomKnee = std::clamp(bloomKnee, 0.0f, 2.0f);
        bloomStrength = std::clamp(bloomStrength, 0.0f, 1.0f);
        bloomSampleScale = std::clamp(bloomSampleScale, 0.5f, 3.0f);
        bloomLevelBlend = std::clamp(bloomLevelBlend, 0.05f, 1.0f);
        bloomLevels = std::clamp(bloomLevels, 2, 8);
        toneMapper = std::clamp(toneMapper, 0, 2);
        renderScale = std::clamp(renderScale, 0.35f, 2.0f);
    }

    // Radii that the renderer derives rather than draws.  Each reduces to the
    // familiar Schwarzschild value at spin = 0.
    [[nodiscard]] float horizonRadius() const {
        return kerrHorizonInMass(spin) * mass();
    }
    [[nodiscard]] float photonSphereRadius() const {
        return kerrPhotonOrbitInMass(spin) * mass();
    }
    [[nodiscard]] float iscoRadius() const { return kerrIscoInMass(spin) * mass(); }
    [[nodiscard]] float ergosphereRadius() const { return kErgosphereEquatorInMass * mass(); }
    // Only meaningful without spin: a rotating hole's shadow is not a circle.
    [[nodiscard]] float shadowApparentRadius() const {
        return kCriticalImpactParameterInRadii * schwarzschildRadius;
    }
};

// Quality presets trade integration budget and sampling for frame rate.  They
// never change the physical model, only how finely it is resolved.
inline void applyQualityPreset(BlackHoleParameters& p, QualityPreset preset) {
    p.qualityPreset = preset;
    switch (preset) {
    case QualityPreset::Low:
        p.maxRaySteps = 180; p.rayStep = 0.085f; p.renderScale = 0.65f;
        p.samplesPerFrame = 1; p.bloomLevels = 4; p.maxAccumulatedSamples = 64;
        break;
    case QualityPreset::Medium:
        p.maxRaySteps = 280; p.rayStep = 0.058f; p.renderScale = 0.85f;
        p.samplesPerFrame = 1; p.bloomLevels = 5; p.maxAccumulatedSamples = 128;
        break;
    case QualityPreset::High:
        p.maxRaySteps = 420; p.rayStep = 0.040f; p.renderScale = 1.0f;
        p.samplesPerFrame = 1; p.bloomLevels = 6; p.maxAccumulatedSamples = 256;
        break;
    case QualityPreset::Ultra:
        p.maxRaySteps = 820; p.rayStep = 0.024f; p.renderScale = 1.0f;
        p.samplesPerFrame = 2; p.bloomLevels = 6; p.maxAccumulatedSamples = 512;
        break;
    }
    p.sanitize();
}

} // namespace bhs::physics
