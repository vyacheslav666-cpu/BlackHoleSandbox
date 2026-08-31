#pragma once

#include <glm/glm.hpp>

namespace bhs {

enum class CameraMode {
    FreeFlight,
    Orbit,
};

// Small, explicit camera rather than an opaque controller.  In orbit mode it
// always looks at the origin, which is the Schwarzschild coordinate origin.
class Camera {
public:
    Camera();

    [[nodiscard]] glm::mat4 viewMatrix() const;
    [[nodiscard]] glm::vec3 position() const { return position_; }
    [[nodiscard]] glm::vec3 forward() const { return forward_; }
    [[nodiscard]] float verticalFovDegrees() const { return fovDegrees_; }
    [[nodiscard]] float movementSpeed() const { return movementSpeed_; }
    [[nodiscard]] float orbitDistance() const { return orbitDistance_; }
    [[nodiscard]] CameraMode mode() const { return mode_; }

    void setMode(CameraMode mode);
    void setPosition(const glm::vec3& position);
    void setVerticalFovDegrees(float degrees);
    void setMovementSpeed(float speed);
    void setOrbitDistance(float distance);
    // Places the orbit camera directly, in degrees.  Yaw sweeps around the
    // disk's rotation axis; pitch is the inclination above the disk plane, so
    // pitch = 0 is the dramatic edge-on view and pitch = 90 looks straight down.
    void setOrbitAngles(float yawDegrees, float pitchDegrees);
    [[nodiscard]] float yawDegrees() const { return yawDegrees_; }
    [[nodiscard]] float pitchDegrees() const { return pitchDegrees_; }

    void processMouseDelta(float xOffset, float yOffset);
    void processMovement(bool forward, bool backward, bool left, bool right,
                         bool up, bool down, float deltaSeconds);
    void processScroll(float offset);

    // Keyboard-driven equivalents of the mouse gestures, so the pointer stays
    // free for the control panel.
    //
    // rotate() turns the camera by an angular *increment* in degrees.
    // zoom() takes a signed number of steps: positive moves closer in orbit
    // mode and narrows the lens in free flight, which is what "zoom in" means
    // in each case.
    void rotate(float yawDeltaDegrees, float pitchDeltaDegrees);
    void zoom(float steps);

private:
    void rebuildFreeFlightBasis();
    void rebuildOrbitPosition();

    CameraMode mode_ = CameraMode::Orbit;
    glm::vec3 position_{0.0f, 2.3f, 12.0f};
    glm::vec3 forward_{0.0f, -0.15f, -1.0f};
    glm::vec3 right_{1.0f, 0.0f, 0.0f};
    glm::vec3 up_{0.0f, 1.0f, 0.0f};

    // Yaw/pitch describe either a free-flight direction or the orbit position.
    float yawDegrees_ = -90.0f;
    float pitchDegrees_ = -11.0f;
    float fovDegrees_ = 48.0f;
    float movementSpeed_ = 5.0f;
    float mouseSensitivity_ = 0.12f;
    float orbitDistance_ = 12.0f;
};

} // namespace bhs
