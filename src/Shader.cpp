#include "Shader.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

Shader::Shader() : m_Program(0) {}

Shader::~Shader() {
    if (m_Program) glDeleteProgram(m_Program);
}

bool Shader::CompileFromSource(const char* vertexSrc, const char* fragmentSrc) {
    GLuint vert = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    if (!vert) return false;
    GLuint frag = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!frag) { glDeleteShader(vert); return false; }

    bool ok = LinkProgram(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    return ok;
}

void Shader::Use() const { glUseProgram(m_Program); }

void Shader::SetMat4(const char* name, const glm::mat4& mat) const {
    glUniformMatrix4fv(glGetUniformLocation(m_Program, name), 1, GL_FALSE, glm::value_ptr(mat));
}
void Shader::SetVec3(const char* name, const glm::vec3& vec) const {
    glUniform3fv(glGetUniformLocation(m_Program, name), 1, glm::value_ptr(vec));
}
void Shader::SetVec4(const char* name, const glm::vec4& vec) const {
    glUniform4fv(glGetUniformLocation(m_Program, name), 1, glm::value_ptr(vec));
}
void Shader::SetFloat(const char* name, float value) const {
    glUniform1f(glGetUniformLocation(m_Program, name), value);
}
void Shader::SetInt(const char* name, int value) const {
    glUniform1i(glGetUniformLocation(m_Program, name), value);
}

GLuint Shader::CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(shader, 512, nullptr, log);
        std::cerr << "[Shader] Compile error:\n" << log << "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool Shader::LinkProgram(GLuint vert, GLuint frag) {
    m_Program = glCreateProgram();
    glAttachShader(m_Program, vert);
    glAttachShader(m_Program, frag);
    glLinkProgram(m_Program);

    GLint ok;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(m_Program, 512, nullptr, log);
        std::cerr << "[Shader] Link error:\n" << log << "\n";
        glDeleteProgram(m_Program);
        m_Program = 0;
        return false;
    }
    return true;
}