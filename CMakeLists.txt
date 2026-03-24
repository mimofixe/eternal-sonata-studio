cmake_minimum_required(VERSION 3.15)
project(EternalSonataStudio VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UTF-8 support
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    libs/imgui/imgui.cpp
    libs/imgui/imgui_draw.cpp
    libs/imgui/imgui_tables.cpp
    libs/imgui/imgui_widgets.cpp
    libs/imgui/imgui_demo.cpp
    libs/imgui/backends/imgui_impl_glfw.cpp
    libs/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC 
    libs/imgui
    libs/imgui/backends
)

target_link_libraries(imgui PUBLIC glfw)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libs/spdlog/include
    ${CMAKE_SOURCE_DIR}/libs/minimp3
)

add_executable(es_studio
    src/main.cpp
    src/Application.cpp
    src/EFileParser.cpp
    src/FileDialog.cpp
    src/FileSystemBrowser.cpp
    src/ChunkInspector.cpp
    src/NSHPParser.cpp
    src/Viewport3D.cpp
    src/Camera.cpp
    src/Shader.cpp
    src/P3TexParser.cpp
    src/TextureCache.cpp
    src/P3TexViewer.cpp
    src/EFileTextureViewer.cpp
    src/EFileTextExtractor.cpp
    src/CSFParser.cpp
    src/ATRAC3Writer.cpp
    src/CSFViewer.cpp
)

target_link_libraries(es_studio PRIVATE
    imgui
    glfw
    glad::glad
    glm::glm
    OpenGL::GL
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT es_studio)
set_property(TARGET es_studio PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")