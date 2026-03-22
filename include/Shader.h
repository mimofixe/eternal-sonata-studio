#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

class Shader {
public:
    Shader();
    ~Shader();

    bool CompileFromSource(const char* vertexSrc, const char* fragmentSrc);

    void Use() const;

    void SetMat4(const char* name, const glm::mat4& mat) const;
    void SetVec3(const char* name, const glm::vec3& vec) const;
    void SetFloat(const char* name, float value) const;

    GLuint GetProgram() const { return m_Program; }

private:
    GLuint CompileShader(GLenum type, const char* source);
    bool LinkProgram(GLuint vertex, GLuint fragment);

    GLuint m_Program;
};