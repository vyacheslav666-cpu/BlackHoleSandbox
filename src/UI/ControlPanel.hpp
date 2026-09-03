#pragma once

#include <string>

#include "Camera/Camera.hpp"
#include "Physics/BlackHoleParameters.hpp"

namespace bhs::ui {

struct RuntimeStats {
    float fps = 0.0f;
    float frameMilliseconds = 0.0f;
    int renderWidth = 0;
    int renderHeight = 0;
    int accumulatedSamples = 0;
    float animationTime = 0.0f;
    bool animationPaused = false;
    // GPU cost of the black-hole pass, measured with a timer query rather than
    // inferred from the frame rate: vsync, the UI and the rest of the pipeline
    // all sit inside the frame time, and none of them belong in this number.
    int blackHoleFrames = 0;
    // False when the driver cannot give a compute stage screen-space
    // derivatives, in which case only the fragment tracer exists.
    bool computeTracerAvailable = false;
    double blackHoleLastMs = 0.0;
    double blackHoleMedianMs = 0.0;
    double blackHoleP95Ms = 0.0;
    std::string glRenderer;
    std::string glVersion;
};

struct ControlPanelActions {
    bool reloadShaders = false;
    bool resetCamera = false;
    bool toggleVsync = false;
    bool togglePause = false;
};

class ControlPanel {
public:
    // The UI writes one compact parameter struct; the renderer reads that
    // struct once per frame.  Keeping the boundary this narrow is what lets the
    // headless capture path reuse the entire renderer with no UI at all.
    [[nodiscard]] static ControlPanelActions draw(bool& visible,
                                                  physics::BlackHoleParameters& parameters,
                                                  Camera& camera,
                                                  const RuntimeStats& stats);

    // Translucent key reference pinned to a screen corner.  `useRussian` is
    // false when the Cyrillic-capable system font could not be loaded, in which
    // case the same text is shown in English rather than as empty boxes.
    static void drawHelpOverlay(bool& visible, const Camera& camera, bool useRussian);
};

} // namespace bhs::ui
