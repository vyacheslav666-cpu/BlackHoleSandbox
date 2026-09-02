cmake_minimum_required(VERSION 3.24)

# BLACK HOLE SANDBOX is intentionally a native Windows / MSVC C++20 project.
# All third-party source dependencies are fetched during configuration; no DLLs
# need to be copied into the repository by hand.
project(BlackHoleSandbox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(FetchContent)

# Keep dependency configuration quiet and deterministic for a small application.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)

# glad 2 generates a reproducible OpenGL 4.6 core loader as a normal CMake
# target.  This avoids committing platform-specific generated loader code.
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glfw glm glad imgui)

# glad's generator CMake package lives below its source root rather than being
# exported by the root project itself.  Glad 2 uses Jinja2 at generation time;
# provision it in a build-local virtual environment rather than asking the
# developer to mutate a global Python install.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GLAD_PYTHON_VENV "${CMAKE_BINARY_DIR}/_tooling/glad-python")
if(WIN32)
    set(GLAD_PYTHON_EXECUTABLE "${GLAD_PYTHON_VENV}/Scripts/python.exe")
else()
    set(GLAD_PYTHON_EXECUTABLE "${GLAD_PYTHON_VENV}/bin/python")
endif()

if(NOT EXISTS "${GLAD_PYTHON_EXECUTABLE}")
    message(STATUS "Creating isolated Python environment for GLAD generation")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m venv "${GLAD_PYTHON_VENV}"
        RESULT_VARIABLE GLAD_VENV_RESULT
    )
    if(NOT GLAD_VENV_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not create the build-local Python environment required by GLAD")
    endif()
endif()

execute_process(
    COMMAND "${GLAD_PYTHON_EXECUTABLE}" -c "import jinja2"
    RESULT_VARIABLE GLAD_JINJA_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
)
if(NOT GLAD_JINJA_RESULT EQUAL 0)
    message(STATUS "Installing GLAD's Jinja2 build dependency into ${GLAD_PYTHON_VENV}")
    execute_process(
        COMMAND "${GLAD_PYTHON_EXECUTABLE}" -m pip install --disable-pip-version-check --no-input --quiet "jinja2>=3.1,<3.2"
        RESULT_VARIABLE GLAD_PIP_RESULT
    )
    if(NOT GLAD_PIP_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not install Jinja2 for GLAD generation. Check network access or provide a Python environment with Jinja2.")
    endif()
endif()

# GladConfig uses find_package(Python), which respects this cache entry.
set(Python_EXECUTABLE "${GLAD_PYTHON_EXECUTABLE}" CACHE FILEPATH "Python used by the GLAD generator" FORCE)
add_subdirectory("${glad_SOURCE_DIR}/cmake" "${glad_BINARY_DIR}/cmake")
glad_add_library(glad REPRODUCIBLE API gl:core=4.6)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(imgui PUBLIC glfw glad)

find_package(OpenGL REQUIRED)

set(BHS_SOURCES
    src/main.cpp
    src/Application/AppOptions.cpp
    src/Application/Application.cpp
    src/Camera/Camera.cpp
    src/Renderer/GpuTimer.cpp
    src/Renderer/ImageWriter.cpp
    src/Renderer/RenderTarget.cpp
    src/Renderer/ShaderProgram.cpp
    src/UI/ControlPanel.cpp
)

add_executable(BlackHoleSandbox ${BHS_SOURCES})

# Declare the header dependencies explicitly instead of trusting the compiler
# to report them.
#
# With the Ninja generator, CMake discovers which headers a source file uses by
# parsing cl.exe's /showIncludes output.  On a localized Visual Studio those
# lines are translated, and the encoding CMake records does not reliably match
# the code page cl.exe writes -- so Ninja ends up recording only *some* of the
# dependencies.  The failure is silent and genuinely dangerous: editing a struct
# in a header rebuilds some translation units but not others, and the link then
# succeeds against objects that disagree about that struct's layout.  It shows
# up as parameters that read back as nonsense.
#
# This project has eight source files and builds in seconds, so the blunt fix is
# the right one: every object depends on every project header.  Touching a
# header rebuilds everything, and the result is always consistent.
file(GLOB_RECURSE BHS_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.hpp")
set_source_files_properties(${BHS_SOURCES} PROPERTIES OBJECT_DEPENDS "${BHS_HEADERS}")

target_include_directories(BlackHoleSandbox PRIVATE src)
target_link_libraries(BlackHoleSandbox PRIVATE OpenGL::GL glfw glad imgui glm::glm)
target_compile_definitions(BlackHoleSandbox PRIVATE GLFW_INCLUDE_NONE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(BlackHoleSandbox PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
    target_compile_options(BlackHoleSandbox PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The executable can run straight from the build folder.  The source-dir macro
# is a development fallback for launching from an IDE working directory.
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" BHS_SOURCE_DIR_FORWARD)
target_compile_definitions(BlackHoleSandbox PRIVATE BHS_SOURCE_DIR="${BHS_SOURCE_DIR_FORWARD}")
file(GLOB BHS_SHADER_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*")
add_custom_target(copy_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:BlackHoleSandbox>/shaders"
    DEPENDS ${BHS_SHADER_SOURCES}
    COMMENT "Copying GLSL shaders beside BlackHoleSandbox"
)
add_dependencies(BlackHoleSandbox copy_shaders)
