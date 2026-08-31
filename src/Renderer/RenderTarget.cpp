#include "Renderer/RenderTarget.hpp"

#include <stdexcept>

namespace bhs::renderer {

namespace {

struct FormatTriplet {
    GLenum internalFormat;
    GLenum pixelFormat;
    GLenum pixelType;
};

FormatTriplet describe(TargetFormat format) {
    switch (format) {
    case TargetFormat::Rgba32F: return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
    case TargetFormat::Rgba8:   return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case TargetFormat::Rgba16F:
    default:                    return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
    }
}

} // namespace

RenderTarget::~RenderTarget() { release(); }

void RenderTarget::resize(int width, int height, TargetFormat format) {
    if (width == width_ && height == height_ && format == format_ && framebuffer_ != 0) {
        return;
    }
    width_ = width;
    height_ = height;
    format_ = format;

    if (framebuffer_ == 0) glGenFramebuffers(1, &framebuffer_);
    if (colorTexture_ == 0) glGenTextures(1, &colorTexture_);

    const FormatTriplet triplet = describe(format_);
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(triplet.internalFormat), width_, height_, 0,
                 triplet.pixelFormat, triplet.pixelType, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamping matters for the bloom chain: a wrapped sample would smear the
    // opposite screen edge into a bright halo.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture_, 0);
    constexpr GLenum drawBuffers[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, drawBuffers);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Offscreen framebuffer is incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderTarget::release() {
    if (colorTexture_ != 0) {
        glDeleteTextures(1, &colorTexture_);
        colorTexture_ = 0;
    }
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

void RenderTarget::bind() const { glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_); }

void RenderTarget::bindForFullWrite() const {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width_, height_);
}

} // namespace bhs::renderer
