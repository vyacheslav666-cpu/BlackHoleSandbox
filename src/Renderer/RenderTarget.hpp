#pragma once

#include <glad/gl.h>

namespace bhs::renderer {

// Pixel formats used by the renderer's offscreen passes.
//   Rgba16F  HDR working format: enough range/precision for one frame.
//   Rgba32F  accumulation format: a running mean over hundreds of samples
//            would visibly quantise in half precision.
//   Rgba8    LDR readback format for the final, display-encoded image.
enum class TargetFormat {
    Rgba16F,
    Rgba32F,
    Rgba8,
};

// A single colour attachment plus its framebuffer.  HDR is retained until the
// final postprocess pass so bloom and the tone mapper see linear radiance.
class RenderTarget {
public:
    RenderTarget() = default;
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    void resize(int width, int height, TargetFormat format = TargetFormat::Rgba16F);
    void release();
    void bind() const;

    // Binds this target and sets the viewport to its full extent.  Every pass
    // writes the whole attachment, so the two always belong together.
    void bindForFullWrite() const;

    [[nodiscard]] GLuint texture() const { return colorTexture_; }
    [[nodiscard]] GLuint framebuffer() const { return framebuffer_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] TargetFormat format() const { return format_; }

private:
    GLuint framebuffer_ = 0;
    GLuint colorTexture_ = 0;
    int width_ = 0;
    int height_ = 0;
    TargetFormat format_ = TargetFormat::Rgba16F;
};

} // namespace bhs::renderer
