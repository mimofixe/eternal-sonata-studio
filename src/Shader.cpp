#include "Shader.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

Shader::Shader() : m_Program(0) {
}

Shader::~Shader() {
    if (m_Program) {
        glDeleteProgram(m_Program);
    }
}

bool Shader::CompileFromSource(const char* vertexSrc, const char* fragmentSrc) {
    GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    if (!vertex) return false;

    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!fragment) {
        glDeleteShader(vertex);
        return false;
    }

    bool success = LinkProgram(vertex, fragment);

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    return success;
}

void Shader::Use() const {
    glUseProgram(m_Program);
}

void Shader::SetMat4(const char* name, const glm::mat4& mat) const {
    GLint loc = glGetUniformLocation(m_Program, name);
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::SetVec3(const char* name, const glm::vec3& vec) const {
    GLint loc = glGetUniformLocation(m_Program, name);
    glUniform3fv(loc, 1, glm::value_ptr(vec));
}

void Shader::SetFloat(const char* name, float value) const {
    GLint loc = glGetUniformLocation(m_Program, name);
    glUniform1f(loc, value);
}

GLuint Shader::CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::cerr << "Shader compilation error:\n" << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool Shader::LinkProgram(GLuint vertex, GLuint fragment) {
    m_Program = glCreateProgram();
    glAttachShader(m_Program, vertex);
    glAttachShader(m_Program, fragment);
    glLinkProgram(m_Program);

    GLint success;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &success);

    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_Program, 512, nullptr, log);
        std::cerr << "Shader linking error:\n" << log << std::endl;
        glDeleteProgram(m_Program);
        m_Program = 0;
        return false;
    }

    return true;
}