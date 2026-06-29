#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera();

    void SetPosition(const glm::vec3& pos);
    void SetTarget(const glm::vec3& target);
    // Place the camera at a world eye looking along a world yaw (radians), matching
    // the engine's field cameras: forward = (sin yaw, 0, cos yaw). Keeps the orbital
    // model usable afterwards by deriving a target a fixed distance ahead.
    void SetFromMapShot(const glm::vec3& eye, float yaw, float pitch = 0.0f);
    void SetDistance(float distance);
    void SetFOV(float fov);

    void Rotate(float yaw, float pitch);
    void Zoom(float delta);
    void Pan(float dx, float dy);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspect) const;

    glm::vec3 GetPosition() const;
    float GetDistance() const { return m_Distance; }

private:
    void UpdatePosition();

    glm::vec3 m_Target;
    float m_Distance;
    float m_Yaw;
    float m_Pitch;
    float m_FOV;

    glm::vec3 m_Position;
};