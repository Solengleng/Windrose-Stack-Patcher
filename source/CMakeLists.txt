cmake_minimum_required(VERSION 3.20)
project(WindroseStackPatcher CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime — no external DLL dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# Use cached sources if already downloaded — avoids re-clone errors
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------- GLFW ----------
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ---------- Dear ImGui ----------
FetchContent_Declare(
    imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# ---------- stb_image (header-only) ----------
FetchContent_Declare(
    stb
    URL      https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb)

# ---------- Executable ----------
add_executable(WindroseStackPatcher WIN32
    main.cpp
    ${IMGUI_SOURCES}
    resources.rc
)

target_include_directories(WindroseStackPatcher PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${stb_SOURCE_DIR}
)

target_link_libraries(WindroseStackPatcher PRIVATE
    glfw
    opengl32
    ole32        # CoInitialize / IFileDialog
    uuid         # CLSID_FileOpenDialog
    shell32      # ShellExecute
)

# Allow int main() with WIN32 subsystem
set_target_properties(WindroseStackPatcher PROPERTIES
    LINK_FLAGS "/ENTRY:mainCRTStartup"
)

# Put the .exe directly in the build folder root
set_target_properties(WindroseStackPatcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
