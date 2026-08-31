#include "Renderer/ShaderProgram.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <glm/gtc/type_ptr.hpp>

namespace bhs::renderer {

namespace {

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open shader: " + path.string());
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

GLuint compileStage(GLenum stage, const std::string& source, const std::filesystem::path& path) {
    const GLuint shader = glCreateShader(stage);
    const char* sourcePtr = source.c_str();
    glShaderSource(shader, 1, &sourcePtr, nullptr);
    glCompileShader(shader);

    GLint succeeded = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &succeeded);
    if (succeeded == GL_TRUE) {
        return shader;
    }

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    throw std::runtime_error("GLSL compilation failed for " + path.string() + "\n" + log);
}

} // namespace

ShaderProgram::~ShaderProgram() { reset(); }

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : program_(std::exchange(other.program_, 0)) {}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        reset();
        program_ = std::exchange(other.program_, 0);
    }
    return *this;
}

ShaderProgram ShaderProgram::fromFiles(const std::filesystem::path& vertexPath,
                                       const std::filesystem::path& fragmentPath) {
    const GLuint vertex = compileStage(GL_VERTEX_SHADER, readTextFile(vertexPath), vertexPath);
    GLuint fragment = 0;
    GLuint program = 0;
    try {
        fragment = compileStage(GL_FRAGMENT_SHADER, readTextFile(fragmentPath), fragmentPath);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);

        GLint succeeded = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &succeeded);
        if (succeeded != GL_TRUE) {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(program, length, nullptr, log.data());
            throw std::runtime_error("GLSL linking failed for " + vertexPath.string() +
                                     " + " + fragmentPath.string() + "\n" + log);
        }
    } catch (...) {
        glDeleteShader(vertex);
        if (fragment != 0) {
            glDeleteShader(fragment);
        }
        if (program != 0) {
            glDeleteProgram(program);
        }
        throw;
    }

    glDetachShader(program, vertex);
    glDetachShader(program, fragment);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return ShaderProgram(program);
}

void ShaderProgram::bind() const { glUseProgram(program_); }

void ShaderProgram::reset() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

GLint ShaderProgram::location(const char* name) const { return glGetUniformLocation(program_, name); }

void ShaderProgram::setInt(const char* name, int value) const {
    const GLint uniformLocation = location(name);
    if (uniformLocation >= 0) glUniform1i(uniformLocation, value);
}

void ShaderProgram::setFloat(const char* name, float value) const {
    const GLint uniformLocation = location(name);
    if (uniformLocation >= 0) glUniform1f(uniformLocation, value);
}

void ShaderProgram::setVec2(const char* name, const glm::vec2& value) const {
    const GLint uniformLocation = location(name);
    if (uniformLocation >= 0) glUniform2fv(uniformLocation, 1, glm::value_ptr(value));
}

void ShaderProgram::setVec3(const char* name, const glm::vec3& value) const {
    const GLint uniformLocation = location(name);
    if (uniformLocation >= 0) glUniform3fv(uniformLocation, 1, glm::value_ptr(value));
}

void ShaderProgram::setMat4(const char* name, const glm::mat4& value) const {
    const GLint uniformLocation = location(name);
    if (uniformLocation >= 0) glUniformMatrix4fv(uniformLocation, 1, GL_FALSE, glm::value_ptr(value));
}

} // namespace bhs::renderer
