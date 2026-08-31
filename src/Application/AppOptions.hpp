#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Physics/BlackHoleParameters.hpp"

namespace bhs {

// Command-line configuration.
//
// The interactive application ignores most of this; it exists mainly so the
// renderer can be driven headlessly to produce a deterministic PNG, which makes
// visual changes reviewable without a human watching the window.
struct AppOptions {
    bool showHelp = false;

    // Offscreen capture.
    bool capture = false;
    std::filesystem::path capturePath;
    int captureSamples = 96; // Progressive-refinement frames before readback.
    // Draw the ImGui panel and help overlay into the captured image too.
    // Useful for documentation shots, and it renders offscreen, so it does not
    // depend on the window being visible or unobscured.
    bool captureWithUi = false;

    // Window / capture resolution.
    int width = 1600;
    int height = 900;

    // Orbit camera placement.
    float cameraDistance = 26.0f;
    float cameraYawDegrees = -90.0f;
    float cameraPitchDegrees = -12.0f;
    float fovDegrees = 42.0f;

    // Value of the animation clock used for the capture.
    float animationTime = 12.0f;

    physics::BlackHoleParameters parameters{};
};

// Parses argv.  Throws std::runtime_error with a human-readable message on a
// malformed argument rather than silently ignoring it.
AppOptions parseCommandLine(int argc, char** argv);

// The text printed by --help.
std::string commandLineHelp();

} // namespace bhs
