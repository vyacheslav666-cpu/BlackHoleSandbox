#include "Camera/Camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace bhs {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float radians(float degrees) { return degrees * kPi / 180.0f; }
} // namespace

Camera::Camera() {
    rebuildOrbitPosition();
}

glm::mat4 Camera::viewMatrix() const {
    if (mode_ == CameraMode::Orbit) {
        return glm::lookAt(position_, glm::vec3(0.0f), up_);
    }
    return glm::lookAt(position_, position_ + forward_, up_);
}

void Camera::setMode(CameraMode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    if (mode_ == CameraMode::Orbit) {
        orbitDistance_ = std::max(1.6f, glm::length(position_));
        rebuildOrbitPosition();
    } else {
        rebuildFreeFlightBasis();
    }
}

void Camera::setPosition(const glm::vec3& position) {
    position_ = position;
    if (mode_ == CameraMode::Orbit) {
        orbitDistance_ = std::max(1.6f, glm::length(position_));
        const glm::vec3 direction = glm::normalize(position_);
        pitchDegrees_ = std::asin(direction.y) * 180.0f / kPi;
        yawDegrees_ = std::atan2(direction.z, direction.x) * 180.0f / kPi;
        rebuildOrbitPosition();
    }
}

void Camera::setVerticalFovDegrees(float degrees) {
    fovDegrees_ = std::clamp(degrees, 18.0f, 100.0f);
}

void Camera::setMovementSpeed(float speed) {
    movementSpeed_ = std::clamp(speed, 0.05f, 100.0f);
}

void Camera::setOrbitDistance(float distance) {
    orbitDistance_ = std::clamp(distance, 1.6f, 400.0f);
    if (mode_ == CameraMode::Orbit) {
        rebuildOrbitPosition();
    }
}

void Camera::setOrbitAngles(float yawDegrees, float pitchDegrees) {
    yawDegrees_ = yawDegrees;
    pitchDegrees_ = std::clamp(pitchDegrees, -89.0f, 89.0f);
    if (mode_ == CameraMode::Orbit) {
        rebuildOrbitPosition();
    } else {
        rebuildFreeFlightBasis();
    }
}

void Camera::processMouseDelta(float xOffset, float yOffset) {
    yawDegrees_ += xOffset * mouseSensitivity_;
    pitchDegrees_ = std::clamp(pitchDegrees_ + yOffset * mouseSensitivity_, -88.0f, 88.0f);

    if (mode_ == CameraMode::Orbit) {
        rebuildOrbitPosition();
    } else {
        rebuildFreeFlightBasis();
    }
}

void Camera::processMovement(bool moveForward, bool moveBackward, bool moveLeft, bool moveRight,
                             bool moveUp, bool moveDown, float deltaSeconds) {
    const float distance = movementSpeed_ * deltaSeconds;
    if (mode_ == CameraMode::Orbit) {
        if (moveForward) {
            orbitDistance_ -= distance;
        }
        if (moveBackward) {
            orbitDistance_ += distance;
        }
        orbitDistance_ = std::clamp(orbitDistance_, 1.6f, 400.0f);
        rebuildOrbitPosition();
        return;
    }

    if (moveForward) {
        position_ += forward_ * distance;
    }
    if (moveBackward) {
        position_ -= forward_ * distance;
    }
    if (moveLeft) {
        position_ -= right_ * distance;
    }
    if (moveRight) {
        position_ += right_ * distance;
    }
    if (moveUp) {
        position_ += glm::vec3(0.0f, 1.0f, 0.0f) * distance;
    }
    if (moveDown) {
        position_ -= glm::vec3(0.0f, 1.0f, 0.0f) * distance;
    }
}

void Camera::rotate(float yawDeltaDegrees, float pitchDeltaDegrees) {
    setOrbitAngles(yawDegrees_ + yawDeltaDegrees, pitchDegrees_ + pitchDeltaDegrees);
}

void Camera::zoom(float steps) {
    if (mode_ == CameraMode::Orbit) {
        // Exponential so each step changes the distance by the same *fraction*.
        // A linear step would crawl when far away and lurch when close in.
        setOrbitDistance(orbitDistance_ * std::exp(-steps * 0.10f));
    } else {
        setVerticalFovDegrees(fovDegrees_ - steps * 2.0f);
    }
}

void Camera::processScroll(float offset) { zoom(offset); }

void Camera::rebuildFreeFlightBasis() {
    const float yaw = radians(yawDegrees_);
    const float pitch = radians(pitchDegrees_);
    forward_ = glm::normalize(glm::vec3(
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch)));
    right_ = glm::normalize(glm::cross(forward_, glm::vec3(0.0f, 1.0f, 0.0f)));
    up_ = glm::normalize(glm::cross(right_, forward_));
}

void Camera::rebuildOrbitPosition() {
    const float yaw = radians(yawDegrees_);
    const float pitch = radians(pitchDegrees_);
    position_ = orbitDistance_ * glm::vec3(
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch));
    forward_ = glm::normalize(-position_);
    right_ = glm::normalize(glm::cross(forward_, glm::vec3(0.0f, 1.0f, 0.0f)));
    up_ = glm::normalize(glm::cross(right_, forward_));
}

} // namespace bhs
