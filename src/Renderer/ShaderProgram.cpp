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

// Resolves #include "name" against the directory the including file lives in.
//
// GLSL has no include of its own, and the alternative was two copies of a
// two-thousand-line tracer that would silently drift apart. Only the forms this
// project actually writes are supported: a double-quoted relative path, one
// directive per line, nesting allowed but bounded.
//
// Each file gets a source-string number and a #line directive, so a compiler
// error still points at a real line in a real file; assembleSource fills in
// `sources` with the mapping, which the error path prints.
void assembleSource(const std::filesystem::path& path, std::string& out,
                    std::vector<std::filesystem::path>& sources, int depth) {
    if (depth > 8) {
        throw std::runtime_error("#include nested too deeply at " + path.string());
    }
    const int sourceIndex = static_cast<int>(sources.size());
    sources.push_back(path);

    const std::string text = readTextFile(path);
    std::istringstream stream(text);
    std::string line;
    int lineNumber = 0;
    bool lineDirectivePending = true;

    while (std::getline(stream, line)) {
        ++lineNumber;
        // Tolerate CRLF: the shaders are edited on Windows.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::size_t firstNonSpace = line.find_first_not_of(" \t");
        const bool isInclude =
            firstNonSpace != std::string::npos &&
            line.compare(firstNonSpace, 8, "#include") == 0;
        if (!isInclude) {
            if (lineDirectivePending) {
                // #version has to come first in the translation unit, so the
                // marker is emitted after it rather than before.
                const bool isVersion = firstNonSpace != std::string::npos &&
                                       line.compare(firstNonSpace, 8, "#version") == 0;
                out += line;
                out += '\n';
                if (!isVersion) {
                    // Nothing emitted yet needed a marker; put one in now so the
                    // remainder of this file is numbered correctly.
                    out += "#line " + std::to_string(lineNumber + 1) + " " +
                           std::to_string(sourceIndex) + "\n";
                    lineDirectivePending = false;
                }
                continue;
            }
            out += line;
            out += '\n';
            continue;
        }

        const std::size_t open = line.find('"', firstNonSpace + 8);
        const std::size_t close = open == std::string::npos ? std::string::npos
                                                            : line.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos) {
            throw std::runtime_error("Malformed #include in " + path.string() + " line " +
                                     std::to_string(lineNumber) +
                                     ": expected #include \"file\"");
        }
        const std::string name = line.substr(open + 1, close - open - 1);
        assembleSource(path.parent_path() / name, out, sources, depth + 1);
        // Back to this file, on the line after the directive.
        out += "#line " + std::to_string(lineNumber + 1) + " " +
               std::to_string(sourceIndex) + "\n";
        lineDirectivePending = false;
    }
}

std::string describeSources(const std::vector<std::filesystem::path>& sources) {
    std::string text = "source strings:";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        text += "\n  " + std::to_string(i) + " = " + sources[i].string();
    }
    return text;
}

// Inserts `defines` immediately after the #version line, which must stay first.
std::string withDefines(const std::string& source, const std::string& defines) {
    if (defines.empty()) {
        return source;
    }
    const std::size_t versionAt = source.find("#version");
    if (versionAt == std::string::npos) {
        return defines + source;
    }
    const std::size_t lineEnd = source.find('\n', versionAt);
    if (lineEnd == std::string::npos) {
        return source + "\n" + defines;
    }
    return source.substr(0, lineEnd + 1) + defines + source.substr(lineEnd + 1);
}

GLuint compileStage(GLenum stage, const std::string& source, const std::filesystem::path& path,
                    const std::vector<std::filesystem::path>& sources) {
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
    throw std::runtime_error("GLSL compilation failed for " + path.string() + "\n" + log +
                             "\n" + describeSources(sources));
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
    std::vector<std::filesystem::path> vertexSources;
    std::string vertexSource;
    assembleSource(vertexPath, vertexSource, vertexSources, 0);
    const GLuint vertex = compileStage(GL_VERTEX_SHADER, vertexSource, vertexPath, vertexSources);
    GLuint fragment = 0;
    GLuint program = 0;
    try {
        std::vector<std::filesystem::path> fragmentSources;
        std::string fragmentSource;
        assembleSource(fragmentPath, fragmentSource, fragmentSources, 0);
        fragment = compileStage(GL_FRAGMENT_SHADER, fragmentSource, fragmentPath, fragmentSources);
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

ShaderProgram ShaderProgram::fromComputeFile(const std::filesystem::path& computePath,
                                             const std::string& defines) {
    std::vector<std::filesystem::path> sources;
    std::string source;
    assembleSource(computePath, source, sources, 0);
    const GLuint compute =
        compileStage(GL_COMPUTE_SHADER, withDefines(source, defines), computePath, sources);

    GLuint program = 0;
    try {
        program = glCreateProgram();
        glAttachShader(program, compute);
        glLinkProgram(program);

        GLint succeeded = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &succeeded);
        if (succeeded != GL_TRUE) {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(program, length, nullptr, log.data());
            throw std::runtime_error("GLSL linking failed for " + computePath.string() +
                                     "\n" + log);
        }
    } catch (...) {
        glDeleteShader(compute);
        if (program != 0) {
            glDeleteProgram(program);
        }
        throw;
    }

    glDetachShader(program, compute);
    glDeleteShader(compute);
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
