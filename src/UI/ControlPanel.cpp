#include "UI/ControlPanel.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <imgui.h>

namespace bhs::ui {

namespace {

constexpr std::array<const char*, 4> kQualityLabels = {"Low", "Medium", "High", "Ultra"};

constexpr std::array<const char*, 10> kDebugLabels = {
    "0  Final render",
    "1  Lensed background only",
    "2  Ray step count",
    "3  Escape / capture classification",
    "4  Disk optical depth",
    "5  Combined frequency shift g",
    "6  Doppler factor only",
    "7  Closest approach / landmark radii",
    "8  Background only (no disk)",
    "9  Accretion disk only",
};

constexpr std::array<const char*, 10> kDebugDescriptions = {
    "The full HDR image: lensed sky, volumetric relativistic disk, shadow.",
    "The disk is skipped so only gravitational lensing of the sky is visible.",
    "How many RK4 steps each ray needed. Hot pixels ring the photon sphere.",
    "Red = fell through the horizon, blue = escaped, yellow = crossed the disk,\n"
    "magenta = ran out of integration budget (raise Max RK4 steps if common).",
    "Accumulated extinction through the disk, 1 - exp(-tau).",
    "g = g_gravity * doppler. Red is redshifted, blue is blueshifted.",
    "The Doppler factor alone, with gravitational redshift removed.",
    "Closest approach of each ray, log-scaled. Bright bands mark rays that\n"
    "grazed the photon sphere (1.5 r_s, orange) or the ISCO (3 r_s, blue).",
    "The unobstructed lensed starfield.",
    "Only the disk's own emission, with the sky removed.",
};

constexpr std::array<const char*, 3> kToneMapperLabels = {"ACES (fitted)", "Reinhard",
                                                          "Uncharted 2 filmic"};

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A cinematic viewpoint preset: distance, yaw, pitch, field of view.
struct CameraShot {
    const char* label;
    float distance;
    float yaw;
    float pitch;
    float fov;
    const char* tooltip;
};

constexpr std::array<CameraShot, 4> kCameraShots = {{
    {"Edge-on", 30.0f, -90.0f, 1.8f, 40.0f,
     "Almost in the disk plane. This is the view that shows the far side of the\n"
     "disk bent up over the top of the black hole and down underneath it."},
    {"Cinematic", 26.0f, -90.0f, -12.0f, 42.0f,
     "Slightly below the plane: direct disk in front, lensed arc behind."},
    {"High angle", 34.0f, -75.0f, -38.0f, 42.0f,
     "Looking down on the disk. Good for seeing the shadow and the inner edge."},
    {"Close pass", 8.5f, -90.0f, -7.0f, 66.0f,
     "Near the photon sphere, wide lens. Extreme lensing and strong beaming."},
}};

} // namespace

ControlPanelActions ControlPanel::draw(bool& visible, physics::BlackHoleParameters& p,
                                       Camera& camera, const RuntimeStats& stats) {
    ControlPanelActions actions;
    if (!visible) {
        return actions;
    }

    // Never taller than the window itself; on a small screen the panel would
    // otherwise run off the bottom and only be reachable by scrolling.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float panelHeight = std::min(860.0f, std::max(320.0f, viewport->WorkSize.y - 90.0f));
    ImGui::SetNextWindowSize(ImVec2(430.0f, panelHeight), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Black Hole Sandbox", &visible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return actions;
    }

    // ---- Status -----------------------------------------------------------
    ImGui::Text("%.1f FPS  |  %.2f ms", stats.fps, stats.frameMilliseconds);
    if (stats.blackHoleFrames > 0) {
        ImGui::Text("black_hole pass: %.2f ms", stats.blackHoleLastMs);
        ImGui::TextDisabled("  median %.2f  |  p95 %.2f  (%d frames)", stats.blackHoleMedianMs,
                            stats.blackHoleP95Ms, stats.blackHoleFrames);
        helpMarker("GPU time spent tracing rays, measured with a GL_TIME_ELAPSED query.\n"
                   "The FPS line above is the whole frame: vsync, accumulation, bloom,\n"
                   "tone mapping and this panel are all in it, and none of them respond\n"
                   "to the ray-marching settings. This line does.\n\n"
                   "Median and p95 are taken over a rolling window of recent traced\n"
                   "frames. The window restarts on a resize or a shader reload, since\n"
                   "older timings then describe something that is no longer running.");
    }
    ImGui::TextDisabled("Internal HDR buffer: %d x %d", stats.renderWidth, stats.renderHeight);
    if (p.progressiveRefinement) {
        const float progress =
            static_cast<float>(stats.accumulatedSamples) / static_cast<float>(p.maxAccumulatedSamples);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f),
                           stats.accumulatedSamples >= p.maxAccumulatedSamples ? "converged" : nullptr);
        ImGui::TextDisabled("Refinement: %d / %d samples", stats.accumulatedSamples,
                            p.maxAccumulatedSamples);
    }
    if (!stats.glRenderer.empty()) {
        ImGui::TextDisabled("%s", stats.glRenderer.c_str());
    }

    // ---- Quality ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
        int quality = static_cast<int>(p.qualityPreset);
        if (ImGui::Combo("Preset", &quality, kQualityLabels.data(),
                         static_cast<int>(kQualityLabels.size()))) {
            physics::applyQualityPreset(p, static_cast<physics::QualityPreset>(quality));
        }
        helpMarker("Presets change only how finely the physics is resolved:\n"
                   "integration budget, step size, resolution scale and sampling.\n"
                   "The physical model itself never changes.");
        ImGui::SliderFloat("Render scale", &p.renderScale, 0.35f, 2.0f, "%.2fx");
        helpMarker("Internal resolution relative to the window. Above 1.0 is\n"
                   "supersampling; the result is downsampled on display.");

        ImGui::Checkbox("Progressive refinement", &p.progressiveRefinement);
        helpMarker("While the camera and every parameter hold still, each frame\n"
                   "re-traces with a different sub-pixel offset and is averaged in.\n"
                   "The image cleans itself up over a second or two. Any change\n"
                   "restarts the average, so there is never any ghosting.");
        if (p.progressiveRefinement) {
            ImGui::SliderInt("Refinement budget", &p.maxAccumulatedSamples, 1, 1024);
            ImGui::Checkbox("Freeze animation while refining", &p.freezeAnimationWhileRefining);
        }
        ImGui::SliderInt("Rays per pixel per frame", &p.samplesPerFrame, 1, 8);
        helpMarker("Extra jittered rays traced every frame. Costs proportionally\n"
                   "but improves quality while the camera is moving.");
    }

    // ---- Black hole -------------------------------------------------------
    if (ImGui::CollapsingHeader("Black hole", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Schwarzschild radius", &p.schwarzschildRadius, 0.2f, 3.0f, "%.2f units");
        helpMarker("r_s = 2GM/c^2. Every other length in the scene is measured\n"
                   "against it, so this is effectively the black hole's mass.");
        ImGui::SliderFloat("Spin  a*", &p.spin, -0.998f, 0.998f, "%.3f");
        helpMarker("Dimensionless spin a* = Jc/GM^2, from -1 (maximally\n"
                   "retrograde) through 0 (Schwarzschild) to +1 (maximally\n"
                   "prograde).\n\n"
                   "Any non-zero value switches the renderer from the fast planar\n"
                   "Schwarzschild solver to the full Kerr geodesic integrator, so\n"
                   "expect it to cost noticeably more. Watch the shadow go\n"
                   "lopsided: frame dragging pushes it sideways, which is a\n"
                   "genuinely different shape, not a distortion of a circle.");
        if (p.isRotating()) {
            ImGui::TextDisabled("Kerr solver active (frame dragging on)");
        } else {
            ImGui::TextDisabled("Schwarzschild solver (no rotation)");
        }

        ImGui::TextDisabled("Horizon        r+   = %.2f", static_cast<double>(p.horizonRadius()));
        ImGui::TextDisabled("Photon orbit        = %.2f", static_cast<double>(p.photonSphereRadius()));
        ImGui::TextDisabled("ISCO                = %.2f", static_cast<double>(p.iscoRadius()));
        helpMarker("All three move with the spin. A prograde disk around a rapidly\n"
                   "rotating hole reaches far closer in -- the ISCO falls from 6M\n"
                   "at a* = 0 to about 1.24M at a* = 0.998 -- which makes the inner\n"
                   "disk hotter, faster and much more strongly beamed.");
        if (p.isRotating()) {
            ImGui::TextDisabled("Ergosphere (equator) = %.2f",
                                static_cast<double>(p.ergosphereRadius()));
            helpMarker("Inside this surface nothing can stay at rest: frame dragging\n"
                       "forces every observer to co-rotate with the hole.");
        } else {
            ImGui::TextDisabled("Shadow radius  2.60 r_s = %.2f (apparent)",
                                static_cast<double>(p.shadowApparentRadius()));
            helpMarker("The shadow you see is larger than the horizon: light with an\n"
                       "impact parameter below b_crit = 3*sqrt(3)/2 r_s has no turning\n"
                       "point and must fall in. This renderer does not draw that circle,\n"
                       "it emerges from the integrated trajectories. With spin the\n"
                       "shadow stops being a circle, so no single radius describes it.");
        }
        ImGui::Checkbox("Outline captured rays", &p.showHorizonGuide);
        ImGui::Checkbox("Highlight photon sphere grazes", &p.showPhotonSphereGuide);
    }

    // ---- Ray integration --------------------------------------------------
    if (ImGui::CollapsingHeader("Ray integration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderInt("Max RK4 steps", &p.maxRaySteps, 16, 2048);
        helpMarker("Budget per ray. Rays near the critical impact parameter wind\n"
                   "many times around the photon sphere; too small a budget makes\n"
                   "the ring region unstable (see debug mode 3, magenta pixels).");
        ImGui::SliderFloat("Angular step dphi", &p.rayStep, 0.002f, 0.12f, "%.4f rad");
        helpMarker("Base Runge-Kutta step in the orbital angle. The shader shrinks\n"
                   "it automatically in high curvature and near the disk.");
        ImGui::SliderFloat("Escape radius", &p.escapeRadius, 20.0f, 400.0f, "%.0f units");
        helpMarker("Radius at which remaining curvature is treated as negligible\n"
                   "and the ray direction is handed to the starfield.");
        const float totalSweep = static_cast<float>(p.maxRaySteps) * p.rayStep;
        ImGui::TextDisabled("Budget covers up to %.1f rad (%.2f full orbits)",
                            static_cast<double>(totalSweep),
                            static_cast<double>(totalSweep / 6.2831853f));
    }

    // ---- Accretion disk ---------------------------------------------------
    if (ImGui::CollapsingHeader("Accretion disk", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Lock inner edge to the ISCO", &p.lockDiskToIsco);
        helpMarker("A thin disk normally ends at the innermost stable circular\n"
                   "orbit, r = 3 r_s. Unlocking is an artistic override: inside the\n"
                   "ISCO the orbital-velocity model is extrapolated, not physical.");
        if (p.lockDiskToIsco) {
            ImGui::TextDisabled("r_in = 3 r_s = %.2f", static_cast<double>(p.diskInnerRadius));
        } else {
            ImGui::SliderFloat("Inner radius", &p.diskInnerRadius, 1.05f, 14.0f, "%.2f");
        }
        ImGui::SliderFloat("Outer radius", &p.diskOuterRadius, 3.5f, 60.0f, "%.1f");
        ImGui::SliderFloat("Scale height H", &p.diskHalfThickness, 0.005f, 1.5f, "%.3f");
        helpMarker("Vertical density is a Gaussian with this standard deviation.");
        ImGui::SliderFloat("Flare exponent", &p.diskFlare, 0.0f, 1.5f, "%.2f");
        helpMarker("H(r) = H0 (r/r_in)^q. Real thin disks flare outwards; q = 0 is\n"
                   "a flat slab.");
        ImGui::SliderFloat("Brightness", &p.diskBrightness, 0.0f, 40.0f, "%.2f");
        ImGui::SliderFloat("Peak temperature", &p.diskTemperature, 1500.0f, 25000.0f, "%.0f K");
        helpMarker("Temperature at the hottest radius. The rest follows the\n"
                   "thin-disk law T(r) = T_peak (F(r)/F_peak)^(1/4), so the inner\n"
                   "disk really is bluer than the outer disk.");
        ImGui::SliderFloat("Density", &p.diskDensity, 0.0f, 6.0f, "%.2f");
        ImGui::SliderFloat("Opacity", &p.diskOpacity, 0.0f, 30.0f, "%.2f");
        helpMarker("Extinction per unit length. Near zero the disk glows through\n"
                   "itself; large values converge to an opaque thin disk that\n"
                   "hides whatever is behind it.");
        ImGui::SliderFloat("Turbulence", &p.diskTurbulence, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Pattern rotation rate", &p.artisticOrbitSpeed, 0.0f, 3.0f, "%.2f");
        helpMarker("Artistic: how fast the turbulence pattern is advected. The\n"
                   "radial dependence is the real Keplerian law, Omega ~ r^-3/2,\n"
                   "so the inner disk shears past the outer disk correctly.");
        if (ImGui::RadioButton("Prograde", p.diskRotationDirection > 0.0f)) {
            p.diskRotationDirection = 1.0f;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Retrograde", p.diskRotationDirection < 0.0f)) {
            p.diskRotationDirection = -1.0f;
        }
    }

    // ---- Relativistic optics ---------------------------------------------
    if (ImGui::CollapsingHeader("Relativistic optics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Each slider fades an effect out towards 1.0 (no shift) so it can be "
                           "isolated. At 1.0 the full physical value is used.");
        ImGui::SliderFloat("Doppler shift", &p.dopplerStrength, 0.0f, 1.0f, "%.2f");
        helpMarker("delta = 1 / (gamma (1 - beta . n)) evaluated with the local\n"
                   "orbital velocity and the *lensed* photon direction. At the ISCO\n"
                   "the orbital speed is 0.5 c.");
        ImGui::SliderFloat("Gravitational redshift", &p.gravitationalShiftStrength, 0.0f, 1.0f,
                           "%.2f");
        helpMarker("sqrt(1 - r_s/r) for an emitter at radius r.");
        ImGui::SliderFloat("Relativistic beaming", &p.beamingStrength, 0.0f, 1.0f, "%.2f");
        helpMarker("The g^4 intensity factor that follows from I_nu/nu^3 being\n"
                   "invariant along a null geodesic. It is not a separate artistic\n"
                   "term: for thermal emission it is exactly sigma (g T)^4.");
    }

    // ---- Visual -----------------------------------------------------------
    if (ImGui::CollapsingHeader("Visual / HDR", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Star density", &p.starDensity, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Nebula strength", &p.nebulaStrength, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Exposure", &p.exposure, 0.05f, 6.0f, "%.2f");
        ImGui::SliderFloat("Bloom strength", &p.bloomStrength, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Bloom threshold", &p.bloomThreshold, 0.0f, 6.0f, "%.2f");
        ImGui::SliderFloat("Bloom knee", &p.bloomKnee, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Bloom spread", &p.bloomSampleScale, 0.5f, 3.0f, "%.2f");
        ImGui::SliderFloat("Bloom level blend", &p.bloomLevelBlend, 0.05f, 1.0f, "%.2f");
        helpMarker("How strongly each coarser pyramid level is mixed into the next\n"
                   "finer one. Higher values push energy into the widest, softest\n"
                   "halo; lower values keep the glow tight around bright pixels.");
        ImGui::SliderInt("Bloom levels", &p.bloomLevels, 2, 8);
        helpMarker("Levels in the bloom pyramid. More levels means a wider, softer\n"
                   "glow around the very bright inner disk.");
        ImGui::Combo("Tone mapper", &p.toneMapper, kToneMapperLabels.data(),
                     static_cast<int>(kToneMapperLabels.size()));
        if (ImGui::Checkbox("V-sync", &p.vsync)) {
            actions.toggleVsync = true;
        }
    }

    // ---- Camera -----------------------------------------------------------
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = camera.mode() == CameraMode::Orbit ? 1 : 0;
        if (ImGui::RadioButton("Free flight", mode == 0)) camera.setMode(CameraMode::FreeFlight);
        ImGui::SameLine();
        if (ImGui::RadioButton("Orbit", mode == 1)) camera.setMode(CameraMode::Orbit);

        float fov = camera.verticalFovDegrees();
        if (ImGui::SliderFloat("Vertical FOV", &fov, 18.0f, 100.0f, "%.0f deg")) {
            camera.setVerticalFovDegrees(fov);
        }
        float speed = camera.movementSpeed();
        if (ImGui::SliderFloat("Move speed", &speed, 0.1f, 40.0f, "%.1f")) {
            camera.setMovementSpeed(speed);
        }
        if (camera.mode() == CameraMode::Orbit) {
            float distance = camera.orbitDistance();
            if (ImGui::SliderFloat("Orbit radius", &distance, 1.6f, 120.0f, "%.1f")) {
                camera.setOrbitDistance(distance);
            }
            float pitch = camera.pitchDegrees();
            if (ImGui::SliderFloat("Inclination", &pitch, -89.0f, 89.0f, "%.1f deg")) {
                camera.setOrbitAngles(camera.yawDegrees(), pitch);
            }
            helpMarker("0 degrees is exactly edge-on to the disk, which is the most\n"
                       "dramatic view: the far side of the disk appears bent up over\n"
                       "the top of the black hole and down beneath it.");
        }

        ImGui::TextDisabled("Shots:");
        for (std::size_t i = 0; i < kCameraShots.size(); ++i) {
            const CameraShot& shot = kCameraShots[i];
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(shot.label)) {
                camera.setMode(CameraMode::Orbit);
                camera.setOrbitDistance(shot.distance);
                camera.setOrbitAngles(shot.yaw, shot.pitch);
                camera.setVerticalFovDegrees(shot.fov);
            }
            if (ImGui::BeginItemTooltip()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                ImGui::TextUnformatted(shot.tooltip);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        if (ImGui::Button("Reset camera")) actions.resetCamera = true;
    }

    // ---- Debug ------------------------------------------------------------
    if (ImGui::CollapsingHeader("Debug visualisation", ImGuiTreeNodeFlags_DefaultOpen)) {
        int debugMode = static_cast<int>(p.debugMode);
        if (ImGui::Combo("Mode", &debugMode, kDebugLabels.data(),
                         static_cast<int>(kDebugLabels.size()))) {
            p.debugMode = static_cast<physics::DebugMode>(debugMode);
        }
        const int index = static_cast<int>(p.debugMode);
        if (index >= 0 && index < static_cast<int>(kDebugDescriptions.size())) {
            ImGui::TextWrapped("%s", kDebugDescriptions[static_cast<std::size_t>(index)]);
        }
        ImGui::TextDisabled("Debug modes only change how the trace result is displayed; "
                            "the integrator itself is untouched.");
    }

    // ---- Footer -----------------------------------------------------------
    ImGui::Separator();
    if (ImGui::Button("Reload GLSL shaders")) actions.reloadShaders = true;
    ImGui::SameLine();
    if (ImGui::Button(stats.animationPaused ? "Resume time" : "Pause time")) {
        actions.togglePause = true;
    }
    ImGui::TextDisabled("H help | F1 panel | F2 pause | F5 shot | M mode | R reset | Tab mouse");

    ImGui::End();

    p.sanitize();
    return actions;
}

// =============================================================================
// Help overlay
// =============================================================================
//
// The text below is written literally rather than escaped.  The target is
// compiled with /utf-8, so a plain narrow literal in this UTF-8 file already
// holds UTF-8 bytes -- which is exactly what ImGui wants.  (A u8"" literal
// would be char8_t in C++20 and would not convert.)
//
// The application loads a Cyrillic-capable system font at start-up.  If that
// font is missing, `useRussian` is false and the English column is drawn
// instead; otherwise every Cyrillic glyph would come out as an empty box.
// For the same reason the key names spell out "Стрелки" rather than using the
// arrow characters, which are outside the font's Cyrillic glyph range.

namespace {

struct Binding {
    const char* keysRussian;
    const char* keysEnglish;
    const char* textRussian;
    const char* textEnglish;
};

// Orbit mode has only one meaningful motion, so the arrows and WASD are
// deliberately synonyms rather than two separate controls.
constexpr std::array<Binding, 4> kOrbitBindings = {{
    {"Стрелки", "Arrows", "Вращать камеру", "Orbit the camera"},
    {"W A S D", "W A S D", "То же самое", "The same"},
    {"+  /  -", "+  /  -", "Приблизить / отдалить", "Zoom in / out"},
    {"E  /  Q", "E  /  Q", "То же самое", "The same"},
}};

constexpr std::array<Binding, 4> kFreeFlightBindings = {{
    {"W A S D", "W A S D", "Полёт вперёд / назад / вбок", "Fly forward / back / strafe"},
    {"Стрелки", "Arrows", "Осматриваться", "Look around"},
    {"Q  /  E", "Q  /  E", "Вниз / вверх", "Down / up"},
    {"+  /  -", "+  /  -", "Угол обзора", "Field of view"},
}};

constexpr std::array<Binding, 9> kCommonBindings = {{
    {"ЛКМ + тянуть", "Drag LMB", "Вращать камеру", "Orbit the camera"},
    {"Колесо", "Wheel", "Зум", "Zoom"},
    {"M", "M", "Орбита / свободный полёт", "Orbit / free flight"},
    {"R", "R", "Сброс камеры", "Reset camera"},
    {"F1", "F1", "Панель настроек", "Settings panel"},
    {"F2", "F2", "Пауза анимации", "Pause animation"},
    {"F5", "F5", "Снимок экрана (PNG)", "Screenshot (PNG)"},
    {"H", "H", "Скрыть эту подсказку", "Hide this help"},
    {"Esc", "Esc", "Выход", "Quit"},
}};

} // namespace

void ControlPanel::drawHelpOverlay(bool& visible, const Camera& camera, bool useRussian) {
    if (!visible) {
        return;
    }

    const bool orbit = camera.mode() == CameraMode::Orbit;

    // Pinned to the bottom-right of the work area, clear of the control panel.
    constexpr float kMargin = 12.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kMargin,
               viewport->WorkPos.y + viewport->WorkSize.y - kMargin),
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.38f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##help", nullptr, flags)) {
        const char* title;
        if (useRussian) {
            title = orbit ? "ОРБИТА" : "СВОБОДНЫЙ ПОЛЁТ";
        } else {
            title = orbit ? "ORBIT" : "FREE FLIGHT";
        }
        ImGui::TextDisabled("%s", title);
        ImGui::Separator();

        const auto row = [&](const Binding& binding) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(useRussian ? binding.keysRussian : binding.keysEnglish);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(useRussian ? binding.textRussian : binding.textEnglish);
        };

        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##helpmode", 2, tableFlags)) {
            for (const Binding& binding : orbit ? kOrbitBindings : kFreeFlightBindings) {
                row(binding);
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        if (ImGui::BeginTable("##helpcommon", 2, tableFlags)) {
            for (const Binding& binding : kCommonBindings) {
                row(binding);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

} // namespace bhs::ui
