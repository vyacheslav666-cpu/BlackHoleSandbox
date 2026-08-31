#pragma once

#include <filesystem>
#include <string>

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace bhs::renderer {

// Move-only RAII wrapper around a linked GLSL program.  Keeping shader loading
// here makes hot reload errors actionable rather than buried in Application.
class ShaderProgram {
public:
    ShaderProgram() = default;
    ~ShaderProgram();
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    [[nodiscard]] static ShaderProgram fromFiles(const std::filesystem::path& vertexPath,
                                                  const std::filesystem::path& fragmentPath);
    void bind() const;
    void reset();

    void setInt(const char* name, int value) const;
    void setFloat(const char* name, float value) const;
    void setVec2(const char* name, const glm::vec2& value) const;
    void setVec3(const char* name, const glm::vec3& value) const;
    void setMat4(const char* name, const glm::mat4& value) const;

private:
    explicit ShaderProgram(GLuint program) : program_(program) {}
    [[nodiscard]] GLint location(const char* name) const;
    GLuint program_ = 0;
};

} // namespace bhs::renderer
