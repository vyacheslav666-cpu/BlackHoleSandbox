#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Application/AppOptions.hpp"
#include "Camera/Camera.hpp"
#include "Physics/BlackHoleParameters.hpp"
#include "Renderer/GpuTimer.hpp"
#include "Renderer/RenderTarget.hpp"
#include "Renderer/ShaderProgram.hpp"

struct GLFWwindow;

namespace bhs {

class Application {
public:
    explicit Application(AppOptions options = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Interactive loop, or a single offscreen capture when options.capture set.
    int run();

private:
    void initialize();
    void shutdown();
    void loadShaders();
    void createOrResizeRenderTargets();
    void processInput(float deltaSeconds);

    // ---- Render passes, in pipeline order --------------------------------
    void renderScene(float animationTime);
    void setTracerUniforms(renderer::ShaderProgram& program, float animationTime);
    void dispatchOverImage(int groupX, int groupY);
    void renderSceneWavefront(float animationTime);
    void ensureWavefrontBuffers(std::size_t rayCount);
    void releaseWavefrontBuffers();
    // Falls back to Compute where Wavefront cannot run: it implements the Kerr
    // integrator only, and the compute stage needs the derivatives extension.
    [[nodiscard]] physics::TracerBackend activeTracerBackend() const;
    // Returns the texture holding the image to display: either the raw scene
    // or the progressively refined average.
    unsigned int accumulateScene();
    void renderBloom(unsigned int sourceTexture);
    void renderPostprocess(unsigned int sceneTexture, unsigned int targetFramebuffer,
                           int viewportWidth, int viewportHeight);

    void drawControls();
    void resetCamera();
    void setMouseCaptured(bool captured);

    // Progressive refinement bookkeeping.  Any change that alters the image
    // must restart the average, or stale frames would ghost into the new one.
    [[nodiscard]] std::uint64_t renderStateFingerprint() const;
    void resetAccumulation();

    int runCapture();

    [[nodiscard]] std::filesystem::path findShaderDirectory() const;
    [[nodiscard]] bool risingKeyPress(int key, bool& priorState) const;

    void saveScreenshot();

    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void cursorPositionCallback(GLFWwindow* window, double x, double y);
    static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);

    AppOptions options_;

    GLFWwindow* window_ = nullptr;
    bool initialized_ = false;
    bool imguiInitialized_ = false;
    bool cyrillicFontLoaded_ = false;
    bool controlsVisible_ = true;
    bool helpVisible_ = true;
    // The pointer is free by default so the control panel is directly usable.
    // Tab switches to a captured, first-person style look instead.
    bool mouseCaptured_ = false;
    bool draggingCamera_ = false;
    bool firstMouseSample_ = true;
    bool resizePending_ = true;
    bool tabWasDown_ = false;
    bool f1WasDown_ = false;
    bool mWasDown_ = false;
    bool rWasDown_ = false;
    bool f2WasDown_ = false;
    bool f5WasDown_ = false;
    bool hWasDown_ = false;
    double previousMouseX_ = 0.0;
    double previousMouseY_ = 0.0;

    int framebufferWidth_ = 1;
    int framebufferHeight_ = 1;
    int renderWidth_ = 1;
    int renderHeight_ = 1;
    float previousTime_ = 0.0f;
    float smoothedFps_ = 0.0f;
    float frameMilliseconds_ = 0.0f;

    // Animation clock, advanced separately from wall time so it can be frozen
    // while the image refines.
    float animationTime_ = 0.0f;
    bool animationPaused_ = false;

    int accumulatedSamples_ = 0;
    int accumulationWriteIndex_ = 0;
    std::uint64_t previousFingerprint_ = 0;
    int screenshotCounter_ = 0;

    Camera camera_;
    physics::BlackHoleParameters parameters_;

    renderer::ShaderProgram blackHoleProgram_;
    // The same tracer compiled as a compute shader.  Both are built every run so
    // that a single binary can be measured either way; computeTracerAvailable_
    // is false when the driver cannot supply derivatives to a compute stage, in
    // which case only the fragment path exists.
    renderer::ShaderProgram blackHoleComputeProgram_;
    bool computeTracerAvailable_ = false;
    int computeGroupXCompiled_ = 8;
    int computeGroupYCompiled_ = 8;

    // The wavefront scheduler: four small programs and the buffers the ray state
    // is parked in between chunks.
    renderer::ShaderProgram wavefrontGenerateProgram_;
    renderer::ShaderProgram wavefrontTraceProgram_;
    renderer::ShaderProgram wavefrontPrepareProgram_;
    renderer::ShaderProgram wavefrontShadeProgram_;
    unsigned int rayStateBuffer_ = 0;
    unsigned int rayListBuffers_[2] = {0, 0};
    unsigned int wavefrontControlBuffer_ = 0;
    std::size_t wavefrontRayCapacity_ = 0;
    bool wavefrontFallbackReported_ = false;
    renderer::ShaderProgram accumulateProgram_;
    renderer::ShaderProgram bloomDownsampleProgram_;
    renderer::ShaderProgram bloomUpsampleProgram_;
    renderer::ShaderProgram postprocessProgram_;

    renderer::RenderTarget sceneTarget_;               // One frame of raw HDR.
    renderer::RenderTarget accumulationTargets_[2];    // Ping-ponged running mean.
    // Bloom pyramid.  Held by pointer because RenderTarget owns GL handles and
    // is deliberately non-copyable.
    std::vector<std::unique_ptr<renderer::RenderTarget>> bloomChain_;
    renderer::RenderTarget captureTarget_;             // LDR readback surface.

    // GPU cost of the black-hole pass alone.  That pass is where essentially
    // all the frame time goes, and it is the only one whose cost changes with
    // the scene, so it is the one worth measuring on its own.
    renderer::GpuTimer blackHoleTimer_;

    unsigned int fullscreenVao_ = 0;

    std::filesystem::path shaderDirectory_;
    std::string glRenderer_;
    std::string glVersion_;
};

} // namespace bhs
