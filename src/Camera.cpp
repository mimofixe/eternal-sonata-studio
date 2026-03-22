#include "Camera.h"
#include <algorithm>

Camera::Camera()
    : m_Target(0.0f, 0.0f, 0.0f),
    m_Distance(100.0f),
    m_Yaw(0.785f),
    m_Pitch(0.523f),
    m_FOV(50.0f) {
    UpdatePosition();
}

void Camera::SetPosition(const glm::vec3& pos) {
    m_Position = pos;
}

void Camera::SetTarget(const glm::vec3& target) {
    m_Target = target;
    UpdatePosition();
}

void Camera::SetDistance(float distance) {
    m_Distance = std::max(0.1f, distance);
    UpdatePosition();
}

void Camera::SetFOV(float fov) {
    m_FOV = std::clamp(fov, 10.0f, 120.0f);
}

void Camera::Rotate(float yaw, float pitch) {
    m_Yaw += yaw;
    m_Pitch += pitch;
    m_Pitch = std::clamp(m_Pitch, -1.57f, 1.57f);
    UpdatePosition();
}

void Camera::Zoom(float delta) {
    m_Distance -= delta;
    m_Distance = std::clamp(m_Distance, 0.1f, 1000.0f);
    UpdatePosition();
}

void Camera::Pan(float dx, float dy) {
    glm::vec3 forward = glm::normalize(m_Target - m_Position);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 up = glm::cross(right, forward);

    float panSpeed = m_Distance * 0.002f;

    m_Target += right * dx * panSpeed;
    m_Target += up * dy * panSpeed;

    UpdatePosition();
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::lookAt(m_Position, m_Target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::GetProjectionMatrix(float aspect) const {
    return glm::perspective(glm::radians(m_FOV), aspect, 0.01f, 10000.0f);
}

glm::vec3 Camera::GetPosition() const {
    return m_Position;
}

void Camera::UpdatePosition() {
    float x = m_Distance * cos(m_Pitch) * sin(m_Yaw);
    float y = m_Distance * sin(m_Pitch);
    float z = m_Distance * cos(m_Pitch) * cos(m_Yaw);

    m_Position = m_Target + glm::vec3(x, y, z);
}