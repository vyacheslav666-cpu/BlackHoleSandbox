#include "Application/AppOptions.hpp"

#include <charconv>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bhs {

namespace {

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

float toFloat(std::string_view text, std::string_view flag) {
    try {
        return std::stof(std::string(text));
    } catch (const std::exception&) {
        fail("Expected a number after " + std::string(flag) + ", got \"" + std::string(text) + "\"");
    }
}

int toInt(std::string_view text, std::string_view flag) {
    try {
        return std::stoi(std::string(text));
    } catch (const std::exception&) {
        fail("Expected an integer after " + std::string(flag) + ", got \"" + std::string(text) + "\"");
    }
}

bool toBool(std::string_view text) {
    return text == "1" || text == "true" || text == "on" || text == "yes";
}

// Named renderer parameters reachable through --set name=value.  Keeping this
// as one table means a new slider is exposed to the capture tool for free.
void applyNamedParameter(physics::BlackHoleParameters& p, std::string_view name, std::string_view value) {
    const auto number = [&] { return toFloat(value, name); };

    if (name == "rs" || name == "schwarzschild-radius") p.schwarzschildRadius = number();
    else if (name == "spin")                p.spin = number();
    else if (name == "ray-step")            p.rayStep = number();
    else if (name == "max-steps")           p.maxRaySteps = toInt(value, name);
    else if (name == "escape-radius")       p.escapeRadius = number();
    else if (name == "weak-field")          p.weakFieldRadius = number();
    else if (name == "disk-inner")        { p.diskInnerRadius = number(); p.lockDiskToIsco = false; }
    else if (name == "disk-outer")          p.diskOuterRadius = number();
    else if (name == "disk-thickness")      p.diskHalfThickness = number();
    else if (name == "disk-flare")          p.diskFlare = number();
    else if (name == "disk-brightness")     p.diskBrightness = number();
    else if (name == "disk-temperature")    p.diskTemperature = number();
    else if (name == "disk-density")        p.diskDensity = number();
    else if (name == "disk-opacity")        p.diskOpacity = number();
    else if (name == "disk-turbulence")     p.diskTurbulence = number();
    else if (name == "disk-direction")      p.diskRotationDirection = number();
    else if (name == "orbit-speed")         p.artisticOrbitSpeed = number();
    else if (name == "jet-power")           p.jetPower = number();
    else if (name == "jet-spin-scaling")    p.jetScalesWithSpin = toBool(value);
    else if (name == "jet-length")          p.jetLength = number();
    else if (name == "jet-radius")          p.jetBaseRadius = number();
    else if (name == "jet-collimation")     p.jetCollimation = number();
    else if (name == "jet-lorentz")         p.jetLorentz = number();
    else if (name == "jet-temperature")     p.jetTemperature = number();
    else if (name == "jet-turbulence")      p.jetTurbulence = number();
    else if (name == "plunge")              p.plungeFraction = number();
    else if (name == "accretion-rate")      p.accretionRate = number();
    else if (name == "doppler")             p.dopplerStrength = number();
    else if (name == "gravitational-shift") p.gravitationalShiftStrength = number();
    else if (name == "beaming")             p.beamingStrength = number();
    else if (name == "star-density")        p.starDensity = number();
    else if (name == "nebula")              p.nebulaStrength = number();
    else if (name == "spp")                 p.samplesPerFrame = toInt(value, name);
    else if (name == "exposure")            p.exposure = number();
    else if (name == "bloom")               p.bloomStrength = number();
    else if (name == "bloom-threshold")     p.bloomThreshold = number();
    else if (name == "bloom-knee")          p.bloomKnee = number();
    else if (name == "bloom-scale")         p.bloomSampleScale = number();
    else if (name == "bloom-blend")         p.bloomLevelBlend = number();
    else if (name == "bloom-levels")        p.bloomLevels = toInt(value, name);
    else if (name == "tone-mapper")         p.toneMapper = toInt(value, name);
    else if (name == "render-scale")        p.renderScale = number();
    else if (name == "lock-isco")           p.lockDiskToIsco = toBool(value);
    else if (name == "horizon-guide")       p.showHorizonGuide = toBool(value);
    else if (name == "photon-guide")        p.showPhotonSphereGuide = toBool(value);
    else fail("Unknown --set parameter \"" + std::string(name) + "\". Run --help for the list.");

    p.sanitize();
}

physics::QualityPreset parseQuality(std::string_view text) {
    if (text == "low") return physics::QualityPreset::Low;
    if (text == "medium") return physics::QualityPreset::Medium;
    if (text == "high") return physics::QualityPreset::High;
    if (text == "ultra") return physics::QualityPreset::Ultra;
    fail("Unknown quality preset \"" + std::string(text) + "\" (low|medium|high|ultra)");
}

} // namespace

std::string commandLineHelp() {
    std::ostringstream out;
    out << "Black Hole Sandbox\n\n"
           "  BlackHoleSandbox.exe [options]\n\n"
           "Run with no options for the interactive renderer.\n\n"
           "Offscreen capture:\n"
           "  --shot <file.png>     Render one image and exit.  Also prints one\n"
           "                        BHS_TIMING key=value line giving the GPU cost of\n"
           "                        the ray-tracing pass, in milliseconds.\n"
           "  --samples N           Progressive-refinement frames before readback (default 96).\n"
           "  --with-ui             Draw the control panel and help overlay into the image.\n"
           "  --width N --height N  Output resolution (default 1600x900).\n\n"
           "Camera (orbit mode):\n"
           "  --distance R          Orbit radius in scene units (default 14).\n"
           "  --yaw D --pitch D     Orbit angles in degrees (default -90 / -9).\n"
           "  --fov D               Vertical field of view (default 46).\n"
           "  --time T              Animation clock used for the disk pattern.\n\n"
           "Renderer:\n"
           "  --quality low|medium|high|ultra\n"
           "  --debug N             Debug view 0..9 (0 = final render).\n"
           "  --set name=value      Any renderer parameter; repeatable.\n\n"
           "  --set names: rs ray-step max-steps escape-radius weak-field disk-inner\n"
           "    disk-outer\n"
           "    disk-thickness disk-flare disk-brightness disk-temperature disk-density\n"
           "    disk-opacity disk-turbulence disk-direction orbit-speed doppler\n"
           "    gravitational-shift beaming star-density nebula spp exposure bloom\n"
           "    bloom-threshold bloom-knee bloom-scale bloom-blend bloom-levels\n"
           "    tone-mapper\n"
           "    render-scale lock-isco horizon-guide photon-guide\n";
    return out.str();
}

AppOptions parseCommandLine(int argc, char** argv) {
    AppOptions options;
    // Quality is applied first so an explicit --set can override a preset value
    // regardless of argument order.
    bool qualityRequested = false;
    physics::QualityPreset requestedQuality = physics::QualityPreset::High;
    std::vector<std::pair<std::string, std::string>> deferredSets;

    const auto next = [&](int& i, std::string_view flag) -> std::string_view {
        if (i + 1 >= argc) {
            fail("Missing value after " + std::string(flag));
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--shot") {
            options.capture = true;
            options.capturePath = std::filesystem::path(next(i, arg));
        } else if (arg == "--with-ui") {
            options.captureWithUi = true;
        } else if (arg == "--samples") {
            options.captureSamples = toInt(next(i, arg), arg);
        } else if (arg == "--width") {
            options.width = toInt(next(i, arg), arg);
        } else if (arg == "--height") {
            options.height = toInt(next(i, arg), arg);
        } else if (arg == "--distance") {
            options.cameraDistance = toFloat(next(i, arg), arg);
        } else if (arg == "--yaw") {
            options.cameraYawDegrees = toFloat(next(i, arg), arg);
        } else if (arg == "--pitch") {
            options.cameraPitchDegrees = toFloat(next(i, arg), arg);
        } else if (arg == "--fov") {
            options.fovDegrees = toFloat(next(i, arg), arg);
        } else if (arg == "--time") {
            options.animationTime = toFloat(next(i, arg), arg);
        } else if (arg == "--quality") {
            requestedQuality = parseQuality(next(i, arg));
            qualityRequested = true;
        } else if (arg == "--debug") {
            options.parameters.debugMode = static_cast<physics::DebugMode>(toInt(next(i, arg), arg));
        } else if (arg == "--set") {
            const std::string_view assignment = next(i, arg);
            const std::size_t equals = assignment.find('=');
            if (equals == std::string_view::npos) {
                fail("--set expects name=value, got \"" + std::string(assignment) + "\"");
            }
            deferredSets.emplace_back(std::string(assignment.substr(0, equals)),
                                      std::string(assignment.substr(equals + 1)));
        } else {
            fail("Unknown option \"" + std::string(arg) + "\". Run --help.");
        }
    }

    if (qualityRequested) {
        physics::applyQualityPreset(options.parameters, requestedQuality);
    }
    for (const auto& [name, value] : deferredSets) {
        applyNamedParameter(options.parameters, name, value);
    }

    options.width = options.width < 16 ? 16 : options.width;
    options.height = options.height < 16 ? 16 : options.height;
    options.captureSamples = options.captureSamples < 1 ? 1 : options.captureSamples;
    options.parameters.sanitize();
    return options;
}

} // namespace bhs
