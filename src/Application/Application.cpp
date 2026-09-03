#include "Application/Application.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "Renderer/ImageWriter.hpp"
#include "UI/ControlPanel.hpp"

namespace bhs {

namespace {

void glfwErrorCallback(int code, const char* description) {
    std::cerr << "GLFW error " << code << ": " << description << '\n';
}

void APIENTRY glDebugCallback(GLenum /*source*/, GLenum type, GLuint /*id*/, GLenum severity,
                              GLsizei /*length*/, const GLchar* message, const void* /*userData*/) {
    // NVIDIA's driver emits a great deal of notification-level chatter with no
    // diagnostic value here, so only significant messages are surfaced.
    if (severity != GL_DEBUG_SEVERITY_NOTIFICATION && type != GL_DEBUG_TYPE_PERFORMANCE) {
        std::cerr << "OpenGL debug: " << message << '\n';
    }
}

constexpr std::string_view kShaderFolderName = "shaders";

// FNV-1a over the raw bytes of whatever we feed it.  Used only to notice that
// something about the frame changed, never for security.
class Fingerprint {
public:
    void add(float value) { addBytes(&value, sizeof(value)); }
    void add(int value) { addBytes(&value, sizeof(value)); }
    void add(bool value) { const int v = value ? 1 : 0; addBytes(&v, sizeof(v)); }
    void add(const glm::vec3& value) { addBytes(&value, sizeof(value)); }
    void add(const glm::mat4& value) { addBytes(&value, sizeof(value)); }
    [[nodiscard]] std::uint64_t value() const { return hash_; }

private:
    void addBytes(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ ^= bytes[i];
            hash_ *= 1099511628211ull;
        }
    }
    std::uint64_t hash_ = 14695981039346656037ull;
};

// One line, one record, key=value throughout, prefixed so a benchmark script
// can pick it out of the rest of stdout with a plain string match.  Every
// duration is in milliseconds and written in the C locale, so the decimal
// separator never depends on the machine the run happened on.
std::string timingReportLine(const renderer::GpuTimer& timer, int width, int height,
                             int requestedSamples) {
    std::ostringstream line;
    line << std::fixed << std::setprecision(3);
    line << "BHS_TIMING pass=black_hole"
         << " frames=" << timer.sampleCount()
         << " median_ms=" << timer.medianMilliseconds()
         << " p95_ms=" << timer.percentileMilliseconds(0.95)
         << " min_ms=" << timer.minMilliseconds()
         << " max_ms=" << timer.maxMilliseconds()
         << " total_ms=" << timer.totalMilliseconds()
         << " width=" << width
         << " height=" << height
         << " samples=" << requestedSamples;
    return line.str();
}

std::string timestampedScreenshotName(int counter) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    std::ostringstream name;
    name << "blackhole_" << seconds << '_' << counter << ".png";
    return name.str();
}

} // namespace

Application::Application(AppOptions options) : options_(std::move(options)) {
    parameters_ = options_.parameters;
    animationTime_ = options_.animationTime;
    initialize();
}

Application::~Application() { shutdown(); }

// =============================================================================
// Lifetime
// =============================================================================

void Application::initialize() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (glfwInit() == GLFW_FALSE) {
        throw std::runtime_error("GLFW could not initialize");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // A capture run still needs a real GL context, but never needs to be seen.
    if (options_.capture) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    // Clamp the requested size to the monitor's work area. Asking for a window
    // larger than the screen leaves Windows to shrink it silently, which then
    // no longer matches what the renderer and the UI were laid out for.
    int windowWidth = options_.width;
    int windowHeight = options_.height;
    if (!options_.capture) {
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
            int workX = 0;
            int workY = 0;
            int workWidth = 0;
            int workHeight = 0;
            glfwGetMonitorWorkarea(monitor, &workX, &workY, &workWidth, &workHeight);
            if (workWidth > 0 && workHeight > 0) {
                // A little margin so the title bar and borders still fit.
                windowWidth = std::min(windowWidth, static_cast<int>(workWidth * 0.96f));
                windowHeight = std::min(windowHeight, static_cast<int>(workHeight * 0.92f));
            }
        }
    }

    window_ = glfwCreateWindow(windowWidth, windowHeight, "Black Hole Sandbox", nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        throw std::runtime_error("Could not create an OpenGL 4.6 core window");
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(options_.capture ? 0 : (parameters_.vsync ? 1 : 0));

    if (gladLoadGL(static_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
        throw std::runtime_error("GLAD could not load the requested OpenGL functions");
    }
    if (GLAD_GL_VERSION_4_6 == 0) {
        throw std::runtime_error("The active driver does not expose OpenGL 4.6 core");
    }

    glRenderer_ = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    glVersion_ = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::cout << "Renderer: " << glRenderer_ << "\nOpenGL: " << glVersion_ << '\n';

    if (GLAD_GL_VERSION_4_3 != 0) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(glDebugCallback, nullptr);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glfwSetWindowUserPointer(window_, this);
    // Our callbacks are installed first; ImGui chains them when initialised
    // with install_callbacks = true, so both the UI and the camera see events.
    glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
    glfwSetCursorPosCallback(window_, cursorPositionCallback);
    glfwSetScrollCallback(window_, scrollCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwGetFramebufferSize(window_, &framebufferWidth_, &framebufferHeight_);
    if (options_.capture) {
        // A hidden window may report a zero-sized framebuffer; the capture path
        // renders at the requested resolution regardless.
        framebufferWidth_ = options_.width;
        framebufferHeight_ = options_.height;
    }

    // A capture run normally skips the UI entirely; --with-ui asks for it to be
    // drawn into the image anyway.
    if (!options_.capture || options_.captureWithUi) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        // Keyboard navigation is deliberately *off*: the arrow keys drive the
        // camera, and ImGui's nav would swallow them whenever a widget had
        // focus.
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

        // Dear ImGui's built-in font is Latin-1 only, so the Cyrillic help
        // overlay needs a real system font. Each candidate is checked before
        // loading, because AddFontFromTTF only reports a missing file through
        // an assertion, which a release build compiles away.
        //
        // If none of them exist the overlay quietly falls back to English
        // rather than drawing a grid of empty boxes.
        const std::array<std::filesystem::path, 3> fontCandidates = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/tahoma.ttf",
            "C:/Windows/Fonts/arial.ttf",
        };
        for (const std::filesystem::path& fontPath : fontCandidates) {
            std::error_code fontError;
            if (!std::filesystem::exists(fontPath, fontError)) {
                continue;
            }
            ImFontConfig fontConfig;
            fontConfig.OversampleH = 2;
            fontConfig.OversampleV = 2;
            if (io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 17.0f, &fontConfig,
                                             io.Fonts->GetGlyphRangesCyrillic()) != nullptr) {
                cyrillicFontLoaded_ = true;
                break;
            }
        }

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 7.0f;
        style.FrameRounding = 4.0f;
        style.Colors[ImGuiCol_WindowBg].w = 0.92f;
        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init("#version 460");
        imguiInitialized_ = true;
    }

    glGenVertexArrays(1, &fullscreenVao_);
    shaderDirectory_ = findShaderDirectory();
    loadShaders();

    resetCamera();
    camera_.setOrbitDistance(options_.cameraDistance);
    camera_.setOrbitAngles(options_.cameraYawDegrees, options_.cameraPitchDegrees);
    camera_.setVerticalFovDegrees(options_.fovDegrees);

    if (!options_.capture) {
        setMouseCaptured(false);
    }
    initialized_ = true;
}

void Application::shutdown() {
    if (window_ == nullptr) {
        return;
    }
    // Every GL object must be destroyed while its context is still current.
    if (imguiInitialized_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    sceneTarget_.release();
    accumulationTargets_[0].release();
    accumulationTargets_[1].release();
    bloomChain_.clear();
    captureTarget_.release();
    blackHoleTimer_.release();
    blackHoleProgram_.reset();
    accumulateProgram_.reset();
    bloomDownsampleProgram_.reset();
    bloomUpsampleProgram_.reset();
    postprocessProgram_.reset();
    if (fullscreenVao_ != 0) {
        glDeleteVertexArrays(1, &fullscreenVao_);
        fullscreenVao_ = 0;
    }
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    initialized_ = false;
}

std::filesystem::path Application::findShaderDirectory() const {
    const std::array<std::filesystem::path, 3> candidates = {
        std::filesystem::current_path() / kShaderFolderName,
        std::filesystem::path(BHS_SOURCE_DIR) / kShaderFolderName,
        std::filesystem::current_path().parent_path() / kShaderFolderName,
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "fullscreen.vert")) {
            return candidate;
        }
    }
    throw std::runtime_error("Could not locate the shaders directory. Expected fullscreen.vert "
                             "beside the executable or in the source tree.");
}

void Application::loadShaders() {
    // Compile everything before publishing any of it: a failed hot reload then
    // leaves the previous working set in place instead of half-replacing it.
    auto blackHole = renderer::ShaderProgram::fromFiles(shaderDirectory_ / "fullscreen.vert",
                                                       shaderDirectory_ / "black_hole.frag");
    auto accumulate = renderer::ShaderProgram::fromFiles(shaderDirectory_ / "fullscreen.vert",
                                                         shaderDirectory_ / "accumulate.frag");
    auto downsample = renderer::ShaderProgram::fromFiles(shaderDirectory_ / "fullscreen.vert",
                                                         shaderDirectory_ / "bloom_downsample.frag");
    auto upsample = renderer::ShaderProgram::fromFiles(shaderDirectory_ / "fullscreen.vert",
                                                       shaderDirectory_ / "bloom_upsample.frag");
    auto postprocess = renderer::ShaderProgram::fromFiles(shaderDirectory_ / "fullscreen.vert",
                                                          shaderDirectory_ / "postprocess.frag");
    blackHoleProgram_ = std::move(blackHole);
    accumulateProgram_ = std::move(accumulate);
    bloomDownsampleProgram_ = std::move(downsample);
    bloomUpsampleProgram_ = std::move(upsample);
    postprocessProgram_ = std::move(postprocess);
    resetAccumulation();
    // Timings from the previous shaders describe code that is no longer
    // running, so they must not be averaged in with the new ones.
    blackHoleTimer_.resetStatistics();
    std::cout << "Loaded GLSL shaders from " << shaderDirectory_.string() << '\n';
}

// =============================================================================
// Render targets
// =============================================================================

void Application::createOrResizeRenderTargets() {
    parameters_.sanitize();
    const int desiredWidth =
        std::max(1, static_cast<int>(std::round(framebufferWidth_ * parameters_.renderScale)));
    const int desiredHeight =
        std::max(1, static_cast<int>(std::round(framebufferHeight_ * parameters_.renderScale)));
    const int desiredLevels = parameters_.bloomLevels;

    if (!resizePending_ && desiredWidth == renderWidth_ && desiredHeight == renderHeight_ &&
        static_cast<int>(bloomChain_.size()) == desiredLevels) {
        return;
    }

    renderWidth_ = desiredWidth;
    renderHeight_ = desiredHeight;
    sceneTarget_.resize(renderWidth_, renderHeight_, renderer::TargetFormat::Rgba16F);
    accumulationTargets_[0].resize(renderWidth_, renderHeight_, renderer::TargetFormat::Rgba32F);
    accumulationTargets_[1].resize(renderWidth_, renderHeight_, renderer::TargetFormat::Rgba32F);

    // Halve the resolution per bloom level, stopping before a level would
    // degenerate to a single pixel.
    bloomChain_.clear();
    int levelWidth = renderWidth_;
    int levelHeight = renderHeight_;
    for (int level = 0; level < desiredLevels; ++level) {
        levelWidth = std::max(1, levelWidth / 2);
        levelHeight = std::max(1, levelHeight / 2);
        if (levelWidth < 2 || levelHeight < 2) {
            break;
        }
        auto target = std::make_unique<renderer::RenderTarget>();
        target->resize(levelWidth, levelHeight, renderer::TargetFormat::Rgba16F);
        bloomChain_.push_back(std::move(target));
    }

    resizePending_ = false;
    resetAccumulation();
    // Cost scales with pixel count, so timings taken at the old resolution say
    // nothing about the new one.
    blackHoleTimer_.resetStatistics();
}

// =============================================================================
// Progressive refinement bookkeeping
// =============================================================================

std::uint64_t Application::renderStateFingerprint() const {
    Fingerprint f;
    f.add(camera_.position());
    f.add(camera_.viewMatrix());
    f.add(camera_.verticalFovDegrees());
    f.add(renderWidth_);
    f.add(renderHeight_);

    const physics::BlackHoleParameters& p = parameters_;
    f.add(p.schwarzschildRadius);
    f.add(p.spin);
    f.add(p.rayStep);
    f.add(p.maxRaySteps);
    f.add(p.escapeRadius);
    f.add(p.weakFieldRadius);
    f.add(p.diskInnerRadius);
    f.add(p.diskOuterRadius);
    f.add(p.diskHalfThickness);
    f.add(p.diskFlare);
    f.add(p.diskBrightness);
    f.add(p.diskTemperature);
    f.add(p.diskDensity);
    f.add(p.diskOpacity);
    f.add(p.diskTurbulence);
    f.add(p.diskRotationDirection);
    f.add(p.artisticOrbitSpeed);
    f.add(p.jetPower);
    f.add(p.jetScalesWithSpin);
    f.add(p.jetLength);
    f.add(p.jetBaseRadius);
    f.add(p.jetCollimation);
    f.add(p.jetLorentz);
    f.add(p.jetTemperature);
    f.add(p.jetTurbulence);
    f.add(p.plungeFraction);
    f.add(p.accretionRate);
    f.add(p.dopplerStrength);
    f.add(p.gravitationalShiftStrength);
    f.add(p.beamingStrength);
    f.add(p.starDensity);
    f.add(p.nebulaStrength);
    f.add(p.samplesPerFrame);
    f.add(static_cast<int>(p.debugMode));
    f.add(p.showHorizonGuide);
    f.add(p.showPhotonSphereGuide);

    // The disk pattern depends on the animation clock, so an advancing clock
    // must also restart the average.
    f.add(animationTime_);
    return f.value();
}

void Application::resetAccumulation() { accumulatedSamples_ = 0; }

// =============================================================================
// Render passes
// =============================================================================

void Application::renderScene(float animationTime) {
    // Measures the whole pass, state setup included.  The timer only observes;
    // it issues no draw of its own and changes nothing about the image.
    blackHoleTimer_.begin();
    sceneTarget_.bindForFullWrite();
    glDisable(GL_BLEND);

    const glm::mat4 projection =
        glm::perspective(glm::radians(camera_.verticalFovDegrees()),
                         static_cast<float>(renderWidth_) / static_cast<float>(renderHeight_),
                         0.01f, 1000.0f);
    const glm::mat4 view = camera_.viewMatrix();

    const physics::BlackHoleParameters& p = parameters_;
    blackHoleProgram_.bind();
    blackHoleProgram_.setMat4("uInvProjection", glm::inverse(projection));
    blackHoleProgram_.setMat4("uInvView", glm::inverse(view));
    blackHoleProgram_.setVec3("uCameraPosition", camera_.position());
    blackHoleProgram_.setVec2("uResolution", glm::vec2(renderWidth_, renderHeight_));
    blackHoleProgram_.setFloat("uTime", animationTime);
    blackHoleProgram_.setVec2("uJitter", glm::vec2(0.0f));
    blackHoleProgram_.setInt("uSampleIndex", accumulatedSamples_);
    blackHoleProgram_.setInt("uSamplesPerFrame", p.samplesPerFrame);

    blackHoleProgram_.setFloat("uSchwarzschildRadius", p.schwarzschildRadius);
    blackHoleProgram_.setFloat("uSpin", p.spin);
    blackHoleProgram_.setFloat("uRayStep", p.rayStep);
    blackHoleProgram_.setInt("uMaxRaySteps", p.maxRaySteps);
    blackHoleProgram_.setFloat("uEscapeRadius", p.escapeRadius);
    blackHoleProgram_.setFloat("uWeakFieldRadius", p.weakFieldRadius);

    blackHoleProgram_.setFloat("uDiskInnerRadius", p.diskInnerRadius);
    blackHoleProgram_.setFloat("uDiskOuterRadius", p.diskOuterRadius);
    blackHoleProgram_.setFloat("uDiskHalfThickness", p.diskHalfThickness);
    blackHoleProgram_.setFloat("uDiskFlare", p.diskFlare);
    blackHoleProgram_.setFloat("uDiskBrightness", p.diskBrightness);
    blackHoleProgram_.setFloat("uDiskTemperature", p.diskTemperature);
    blackHoleProgram_.setFloat("uDiskDensity", p.diskDensity);
    blackHoleProgram_.setFloat("uDiskOpacity", p.diskOpacity);
    blackHoleProgram_.setFloat("uDiskTurbulence", p.diskTurbulence);
    blackHoleProgram_.setFloat("uDiskRotationDirection", p.diskRotationDirection);
    blackHoleProgram_.setFloat("uArtisticOrbitSpeed", p.artisticOrbitSpeed);

    blackHoleProgram_.setFloat("uJetPower", p.jetPower);
    blackHoleProgram_.setInt("uJetScalesWithSpin", p.jetScalesWithSpin ? 1 : 0);
    blackHoleProgram_.setFloat("uJetLength", p.jetLength);
    blackHoleProgram_.setFloat("uJetBaseRadius", p.jetBaseRadius);
    blackHoleProgram_.setFloat("uJetCollimation", p.jetCollimation);
    blackHoleProgram_.setFloat("uJetLorentz", p.jetLorentz);
    blackHoleProgram_.setFloat("uJetTemperature", p.jetTemperature);
    blackHoleProgram_.setFloat("uJetTurbulence", p.jetTurbulence);

    blackHoleProgram_.setFloat("uPlungeFraction", p.plungeFraction);
    blackHoleProgram_.setFloat("uAccretionRate", p.accretionRate);
    blackHoleProgram_.setFloat("uIscoRadius", p.iscoRadius());

    blackHoleProgram_.setFloat("uDopplerStrength", p.dopplerStrength);
    blackHoleProgram_.setFloat("uGravitationalShiftStrength", p.gravitationalShiftStrength);
    blackHoleProgram_.setFloat("uBeamingStrength", p.beamingStrength);

    blackHoleProgram_.setFloat("uStarDensity", p.starDensity);
    blackHoleProgram_.setFloat("uNebulaStrength", p.nebulaStrength);

    blackHoleProgram_.setInt("uDebugMode", static_cast<int>(p.debugMode));
    blackHoleProgram_.setInt("uShowHorizonGuide", p.showHorizonGuide ? 1 : 0);
    blackHoleProgram_.setInt("uShowPhotonSphereGuide", p.showPhotonSphereGuide ? 1 : 0);

    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    blackHoleTimer_.end();
}

unsigned int Application::accumulateScene() {
    if (!parameters_.progressiveRefinement) {
        return sceneTarget_.texture();
    }

    renderer::RenderTarget& destination = accumulationTargets_[accumulationWriteIndex_];
    const renderer::RenderTarget& history = accumulationTargets_[1 - accumulationWriteIndex_];

    destination.bindForFullWrite();
    glDisable(GL_BLEND);
    accumulateProgram_.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, history.texture());
    accumulateProgram_.setInt("uHistory", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneTarget_.texture());
    accumulateProgram_.setInt("uCurrent", 1);
    accumulateProgram_.setInt("uSampleIndex", accumulatedSamples_);
    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    const unsigned int result = destination.texture();
    accumulationWriteIndex_ ^= 1;
    ++accumulatedSamples_;
    return result;
}

void Application::renderBloom(unsigned int sourceTexture) {
    if (bloomChain_.empty()) {
        return;
    }
    glDisable(GL_BLEND);
    glBindVertexArray(fullscreenVao_);

    // ---- Downsample chain -------------------------------------------------
    // Level 0 also applies the bright-pass threshold.  Every later level just
    // halves resolution, progressively widening the effective blur.
    bloomDownsampleProgram_.bind();
    unsigned int source = sourceTexture;
    int sourceWidth = renderWidth_;
    int sourceHeight = renderHeight_;
    for (std::size_t level = 0; level < bloomChain_.size(); ++level) {
        renderer::RenderTarget& target = *bloomChain_[level];
        target.bindForFullWrite();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        bloomDownsampleProgram_.setInt("uSource", 0);
        bloomDownsampleProgram_.setVec2("uTexelSize",
                                        glm::vec2(1.0f / static_cast<float>(sourceWidth),
                                                  1.0f / static_cast<float>(sourceHeight)));
        bloomDownsampleProgram_.setFloat("uThreshold", parameters_.bloomThreshold);
        bloomDownsampleProgram_.setFloat("uKnee", parameters_.bloomKnee);
        bloomDownsampleProgram_.setInt("uApplyThreshold", level == 0 ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        source = target.texture();
        sourceWidth = target.width();
        sourceHeight = target.height();
    }

    // ---- Upsample chain ---------------------------------------------------
    // Walking back up, each coarse level is tent-filtered and added into the
    // next finer one.  The sum of many differently sized blurs is what gives
    // bloom a bright core with a very long, soft skirt.
    bloomUpsampleProgram_.bind();
    for (std::size_t level = bloomChain_.size() - 1; level > 0; --level) {
        renderer::RenderTarget& fine = *bloomChain_[level - 1];
        const renderer::RenderTarget& coarse = *bloomChain_[level];

        // The finer level is simultaneously a source and the destination, which
        // a single pass cannot do by sampling it. Fixed-function blending solves
        // it: the shader outputs only the blurred coarse level, and the blend
        // unit mixes it into what `fine` already holds.
        fine.bindForFullWrite();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, coarse.texture());
        bloomUpsampleProgram_.setInt("uLow", 0);
        bloomUpsampleProgram_.setVec2("uLowTexelSize",
                                      glm::vec2(1.0f / static_cast<float>(coarse.width()),
                                                1.0f / static_cast<float>(coarse.height())));
        bloomUpsampleProgram_.setFloat("uSampleScale", parameters_.bloomSampleScale);
        bloomUpsampleProgram_.setFloat("uBlendWeight", parameters_.bloomLevelBlend);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
    }
}

void Application::renderPostprocess(unsigned int sceneTexture, unsigned int targetFramebuffer,
                                    int viewportWidth, int viewportHeight) {
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, viewportWidth, viewportHeight);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    postprocessProgram_.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneTexture);
    postprocessProgram_.setInt("uScene", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloomChain_.empty() ? sceneTexture : bloomChain_.front()->texture());
    postprocessProgram_.setInt("uBloom", 1);
    postprocessProgram_.setFloat("uExposure", parameters_.exposure);
    postprocessProgram_.setFloat("uBloomStrength",
                                 bloomChain_.empty() ? 0.0f : parameters_.bloomStrength);
    postprocessProgram_.setInt("uToneMapper", parameters_.toneMapper);
    postprocessProgram_.setVec2("uResolution", glm::vec2(viewportWidth, viewportHeight));
    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// =============================================================================
// Main loops
// =============================================================================

int Application::run() {
    if (options_.capture) {
        return runCapture();
    }

    previousTime_ = static_cast<float>(glfwGetTime());
    previousFingerprint_ = renderStateFingerprint();

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();
        const float now = static_cast<float>(glfwGetTime());
        const float deltaSeconds = std::clamp(now - previousTime_, 0.0f, 0.1f);
        previousTime_ = now;
        frameMilliseconds_ = deltaSeconds * 1000.0f;
        const float instantaneousFps = deltaSeconds > 0.00001f ? 1.0f / deltaSeconds : 0.0f;
        smoothedFps_ = smoothedFps_ == 0.0f ? instantaneousFps
                                            : std::lerp(smoothedFps_, instantaneousFps, 0.08f);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        processInput(deltaSeconds);

        if (framebufferWidth_ <= 0 || framebufferHeight_ <= 0) {
            ImGui::EndFrame();
            continue;
        }

        createOrResizeRenderTargets();

        // The animation clock only advances when the image is not being
        // refined, so a converging still frame stays coherent.  Once the sample
        // budget is reached the clock is released again and the disk animates
        // at a lower effective frame quality -- unless the user paused it.
        const bool refining = parameters_.progressiveRefinement &&
                              accumulatedSamples_ < parameters_.maxAccumulatedSamples;
        const bool holdClock = animationPaused_ ||
                               (parameters_.freezeAnimationWhileRefining && refining &&
                                accumulatedSamples_ > 0);
        if (!holdClock) {
            animationTime_ += deltaSeconds;
        }

        // Any change to the camera or the parameters invalidates the average.
        const std::uint64_t fingerprint = renderStateFingerprint();
        if (fingerprint != previousFingerprint_) {
            resetAccumulation();
            previousFingerprint_ = fingerprint;
        }

        const bool budgetLeft = !parameters_.progressiveRefinement ||
                                accumulatedSamples_ < parameters_.maxAccumulatedSamples;
        unsigned int displayTexture;
        if (budgetLeft) {
            renderScene(animationTime_);
            displayTexture = accumulateScene();
        } else {
            // Converged: keep showing the finished average without re-tracing.
            displayTexture = accumulationTargets_[1 - accumulationWriteIndex_].texture();
        }

        renderBloom(displayTexture);
        renderPostprocess(displayTexture, 0, framebufferWidth_, framebufferHeight_);

        // A converged frame re-traces nothing, so nothing would otherwise pick
        // up the timings still in flight from the last frame that did.
        blackHoleTimer_.poll();
        drawControls();
        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }
    return 0;
}

int Application::runCapture() {
    framebufferWidth_ = options_.width;
    framebufferHeight_ = options_.height;
    resizePending_ = true;
    createOrResizeRenderTargets();
    captureTarget_.resize(framebufferWidth_, framebufferHeight_, renderer::TargetFormat::Rgba8);

    const int samples = std::max(1, options_.captureSamples);
    resetAccumulation();
    unsigned int displayTexture = sceneTarget_.texture();
    for (int sample = 0; sample < samples; ++sample) {
        renderScene(options_.animationTime);
        displayTexture = accumulateScene();
    }
    // The last few queries are still in flight; without this they would be
    // missing from the statistics reported below.
    blackHoleTimer_.flush();

    renderBloom(displayTexture);
    renderPostprocess(displayTexture, captureTarget_.framebuffer(), framebufferWidth_,
                      framebufferHeight_);

    if (options_.captureWithUi && imguiInitialized_) {
        // ImGui draws into whatever framebuffer is bound, so pointing it at the
        // capture target renders the whole interface offscreen. That is why
        // this works with a hidden window and never depends on the window being
        // visible or unobscured, which a screen grab would.
        //
        // Two details are needed to make it work headless:
        //  * A hidden window can report a zero-sized framebuffer, and ImGui
        //    skips every window when DisplaySize is zero. The size is therefore
        //    overridden after the backend has filled it in.
        //  * Auto-resizing windows only know their true size once they have
        //    been laid out, so the first frames are built and thrown away; only
        //    the last one is actually rasterised.
        constexpr int kWarmUpFrames = 3;
        for (int frame = 0; frame < kWarmUpFrames; ++frame) {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(framebufferWidth_),
                                    static_cast<float>(framebufferHeight_));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            drawControls();
            ImGui::Render();
            if (frame + 1 == kWarmUpFrames) {
                glBindFramebuffer(GL_FRAMEBUFFER, captureTarget_.framebuffer());
                glViewport(0, 0, framebufferWidth_, framebufferHeight_);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            }
        }
    }
    glFinish();

    // Read back the LDR result.  OpenGL's origin is bottom-left and PNG's is
    // top-left, so the rows are reversed while copying into the RGB buffer.
    const std::size_t pixelCount =
        static_cast<std::size_t>(framebufferWidth_) * static_cast<std::size_t>(framebufferHeight_);
    std::vector<std::uint8_t> rgba(pixelCount * 4u);
    glBindFramebuffer(GL_FRAMEBUFFER, captureTarget_.framebuffer());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, framebufferWidth_, framebufferHeight_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    std::vector<std::uint8_t> rgb(pixelCount * 3u);
    for (int y = 0; y < framebufferHeight_; ++y) {
        const int sourceRow = framebufferHeight_ - 1 - y;
        for (int x = 0; x < framebufferWidth_; ++x) {
            const std::size_t src = (static_cast<std::size_t>(sourceRow) * framebufferWidth_ + x) * 4u;
            const std::size_t dst = (static_cast<std::size_t>(y) * framebufferWidth_ + x) * 3u;
            rgb[dst + 0] = rgba[src + 0];
            rgb[dst + 1] = rgba[src + 1];
            rgb[dst + 2] = rgba[src + 2];
        }
    }

    if (!renderer::writePng(options_.capturePath, framebufferWidth_, framebufferHeight_, rgb)) {
        std::cerr << "Could not write " << options_.capturePath.string() << '\n';
        return 1;
    }
    std::cout << "Wrote " << options_.capturePath.string() << " (" << framebufferWidth_ << 'x'
              << framebufferHeight_ << ", " << samples << " accumulated samples)\n";
    std::cout << timingReportLine(blackHoleTimer_, framebufferWidth_, framebufferHeight_, samples)
              << std::endl;
    return 0;
}

// =============================================================================
// Input and UI
// =============================================================================

bool Application::risingKeyPress(int key, bool& priorState) const {
    const bool isDown = glfwGetKey(window_, key) == GLFW_PRESS;
    const bool rising = isDown && !priorState;
    priorState = isDown;
    return rising;
}

void Application::processInput(float deltaSeconds) {
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
        return;
    }
    // Two different questions, deliberately asked separately.
    //
    // WantTextInput means the user is genuinely typing a value (Ctrl+click on a
    // slider). Nothing else may claim the keyboard then.
    //
    // WantCaptureKeyboard is broader: ImGui also raises it while a widget is
    // being dragged *and for as long as a popup is open*. Gating everything on
    // it looked right but was a trap -- one click that opened a combo box left
    // the popup up, and from then on the screenshot key and the camera were
    // both dead with no visible reason. So only the camera waits on it; the
    // function keys stay live.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }

    if (risingKeyPress(GLFW_KEY_F1, f1WasDown_)) {
        controlsVisible_ = !controlsVisible_;
    }
    if (risingKeyPress(GLFW_KEY_TAB, tabWasDown_)) {
        setMouseCaptured(!mouseCaptured_);
    }
    if (risingKeyPress(GLFW_KEY_M, mWasDown_)) {
        camera_.setMode(camera_.mode() == CameraMode::Orbit ? CameraMode::FreeFlight
                                                            : CameraMode::Orbit);
    }
    if (risingKeyPress(GLFW_KEY_R, rWasDown_)) {
        resetCamera();
    }
    if (risingKeyPress(GLFW_KEY_F2, f2WasDown_)) {
        animationPaused_ = !animationPaused_;
    }
    if (risingKeyPress(GLFW_KEY_H, hWasDown_)) {
        helpVisible_ = !helpVisible_;
    }
    if (risingKeyPress(GLFW_KEY_F5, f5WasDown_)) {
        saveScreenshot();
    }

    // ---- Camera -----------------------------------------------------------
    // Held keys, so these are read as levels rather than edges and scaled by
    // the frame time: the camera then moves at the same rate whatever the
    // frame rate happens to be.
    //
    // The camera alone defers to ImGui here, so dragging a slider or having a
    // combo box open never also flies the view.
    if (io.WantCaptureKeyboard) {
        return;
    }
    const auto held = [this](int key) { return glfwGetKey(window_, key) == GLFW_PRESS; };

    const bool wantLeft  = held(GLFW_KEY_LEFT);
    const bool wantRight = held(GLFW_KEY_RIGHT);
    const bool wantUp    = held(GLFW_KEY_UP);
    const bool wantDown  = held(GLFW_KEY_DOWN);
    const bool wantW = held(GLFW_KEY_W);
    const bool wantS = held(GLFW_KEY_S);
    const bool wantA = held(GLFW_KEY_A);
    const bool wantD = held(GLFW_KEY_D);

    // Zoom: both the main row and the numeric keypad, since either is a
    // reasonable thing to reach for.
    const bool wantZoomIn  = held(GLFW_KEY_EQUAL) || held(GLFW_KEY_KP_ADD);
    const bool wantZoomOut = held(GLFW_KEY_MINUS) || held(GLFW_KEY_KP_SUBTRACT);

    constexpr float kRotateDegreesPerSecond = 55.0f;
    constexpr float kZoomStepsPerSecond = 7.0f;
    const float rotateStep = kRotateDegreesPerSecond * deltaSeconds;
    const float zoomStep = kZoomStepsPerSecond * deltaSeconds;

    if (camera_.mode() == CameraMode::Orbit) {
        // Orbiting is the only meaningful motion here, so the arrows and WASD
        // are deliberately synonyms rather than two different controls.
        float yawDelta = 0.0f;
        float pitchDelta = 0.0f;
        if (wantLeft  || wantA) yawDelta -= rotateStep;
        if (wantRight || wantD) yawDelta += rotateStep;
        if (wantUp    || wantW) pitchDelta += rotateStep;
        if (wantDown  || wantS) pitchDelta -= rotateStep;
        if (yawDelta != 0.0f || pitchDelta != 0.0f) {
            camera_.rotate(yawDelta, pitchDelta);
        }
        // Q/E duplicate the zoom keys so one hand can fly the whole camera.
        if (held(GLFW_KEY_E)) camera_.zoom(zoomStep);
        if (held(GLFW_KEY_Q)) camera_.zoom(-zoomStep);
    } else {
        // Free flight separates the two: WASD translates, the arrows look.
        camera_.processMovement(wantW, wantS, wantA, wantD, held(GLFW_KEY_E), held(GLFW_KEY_Q),
                                deltaSeconds);
        float yawDelta = 0.0f;
        float pitchDelta = 0.0f;
        if (wantLeft)  yawDelta -= rotateStep;
        if (wantRight) yawDelta += rotateStep;
        if (wantUp)    pitchDelta += rotateStep;
        if (wantDown)  pitchDelta -= rotateStep;
        if (yawDelta != 0.0f || pitchDelta != 0.0f) {
            camera_.rotate(yawDelta, pitchDelta);
        }
    }

    if (wantZoomIn)  camera_.zoom(zoomStep);
    if (wantZoomOut) camera_.zoom(-zoomStep);
}

void Application::saveScreenshot() {
    // Saves exactly what is on screen, including the accumulated refinement.
    captureTarget_.resize(framebufferWidth_, framebufferHeight_, renderer::TargetFormat::Rgba8);
    const unsigned int texture = accumulationTargets_[1 - accumulationWriteIndex_].texture();
    renderPostprocess(parameters_.progressiveRefinement ? texture : sceneTarget_.texture(),
                      captureTarget_.framebuffer(), framebufferWidth_, framebufferHeight_);

    const std::size_t pixelCount =
        static_cast<std::size_t>(framebufferWidth_) * static_cast<std::size_t>(framebufferHeight_);
    std::vector<std::uint8_t> rgba(pixelCount * 4u);
    glBindFramebuffer(GL_FRAMEBUFFER, captureTarget_.framebuffer());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, framebufferWidth_, framebufferHeight_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // OpenGL's origin is bottom-left and PNG's is top-left, so rows are
    // reversed on the way into the RGB buffer.
    std::vector<std::uint8_t> rgb(pixelCount * 3u);
    for (int y = 0; y < framebufferHeight_; ++y) {
        const int sourceRow = framebufferHeight_ - 1 - y;
        for (int x = 0; x < framebufferWidth_; ++x) {
            const std::size_t src = (static_cast<std::size_t>(sourceRow) * framebufferWidth_ + x) * 4u;
            const std::size_t dst = (static_cast<std::size_t>(y) * framebufferWidth_ + x) * 3u;
            rgb[dst + 0] = rgba[src + 0];
            rgb[dst + 1] = rgba[src + 1];
            rgb[dst + 2] = rgba[src + 2];
        }
    }

    const std::string name = timestampedScreenshotName(screenshotCounter_++);
    if (renderer::writePng(name, framebufferWidth_, framebufferHeight_, rgb)) {
        std::cout << "Saved " << name << '\n';
    } else {
        std::cerr << "Could not save " << name << '\n';
    }
}

void Application::drawControls() {
    ui::RuntimeStats stats;
    stats.fps = smoothedFps_;
    stats.frameMilliseconds = frameMilliseconds_;
    stats.renderWidth = renderWidth_;
    stats.renderHeight = renderHeight_;
    stats.accumulatedSamples = accumulatedSamples_;
    stats.animationTime = animationTime_;
    stats.animationPaused = animationPaused_;
    stats.blackHoleFrames = static_cast<int>(blackHoleTimer_.sampleCount());
    stats.blackHoleLastMs = blackHoleTimer_.lastMilliseconds();
    stats.blackHoleMedianMs = blackHoleTimer_.medianMilliseconds();
    stats.blackHoleP95Ms = blackHoleTimer_.percentileMilliseconds(0.95);
    stats.glRenderer = glRenderer_;
    stats.glVersion = glVersion_;

    const ui::ControlPanelActions actions =
        ui::ControlPanel::draw(controlsVisible_, parameters_, camera_, stats);
    ui::ControlPanel::drawHelpOverlay(helpVisible_, camera_, cyrillicFontLoaded_);
    if (actions.toggleVsync) {
        glfwSwapInterval(parameters_.vsync ? 1 : 0);
    }
    if (actions.resetCamera) {
        resetCamera();
    }
    if (actions.togglePause) {
        animationPaused_ = !animationPaused_;
    }
    if (actions.reloadShaders) {
        try {
            shaderDirectory_ = findShaderDirectory();
            loadShaders();
        } catch (const std::exception& error) {
            std::cerr << "Shader reload rejected: " << error.what() << '\n';
        }
    }
}

void Application::resetCamera() {
    camera_ = Camera{};
    camera_.setMode(CameraMode::Orbit);
    camera_.setOrbitDistance(options_.cameraDistance);
    camera_.setOrbitAngles(options_.cameraYawDegrees, options_.cameraPitchDegrees);
    camera_.setVerticalFovDegrees(options_.fovDegrees);
    resetAccumulation();
}

void Application::setMouseCaptured(bool captured) {
    mouseCaptured_ = captured;
    glfwSetInputMode(window_, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    firstMouseSample_ = true;
}

// =============================================================================
// GLFW callbacks
// =============================================================================

void Application::framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    if (auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window))) {
        app->framebufferWidth_ = width;
        app->framebufferHeight_ = height;
        app->resizePending_ = true;
    }
}

void Application::cursorPositionCallback(GLFWwindow* window, double x, double y) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app == nullptr) {
        return;
    }

    const double previousX = app->previousMouseX_;
    const double previousY = app->previousMouseY_;
    // The last position is tracked unconditionally, so beginning a drag never
    // starts with a stale delta and snaps the camera.
    app->previousMouseX_ = x;
    app->previousMouseY_ = y;

    if (!app->mouseCaptured_ && !app->draggingCamera_) {
        return;
    }
    if (app->firstMouseSample_) {
        app->firstMouseSample_ = false;
        return;
    }
    app->camera_.processMouseDelta(static_cast<float>(x - previousX),
                                   static_cast<float>(previousY - y));
}

void Application::mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }
    if (action == GLFW_PRESS) {
        // A press that lands on the panel belongs to the panel; only a press on
        // the render itself starts an orbit drag.
        if (!ImGui::GetIO().WantCaptureMouse) {
            app->draggingCamera_ = true;
            app->firstMouseSample_ = true;
        }
    } else if (action == GLFW_RELEASE) {
        app->draggingCamera_ = false;
    }
}

void Application::scrollCallback(GLFWwindow* window, double /*xOffset*/, double yOffset) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app == nullptr) {
        return;
    }
    // Scrolling over a slider adjusts the slider, not the camera.
    if (app->mouseCaptured_ || !ImGui::GetIO().WantCaptureMouse) {
        app->camera_.processScroll(static_cast<float>(yOffset));
    }
}

} // namespace bhs
