cmake_minimum_required(VERSION 3.15)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # C is required because glad.c (the OpenGL loader impl) is a .c file.
    project(glint_qem_tool_skeleton LANGUAGES C CXX)
endif()

set(GLINT_QEM_TOOL_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GLINT_QEM_TOOL_INCLUDE_DIR ${GLINT_QEM_TOOL_ROOT}/include)

# ---- qem_core: pure simplification library (no graphics deps) --------------
add_library(glint_qem_core_stub STATIC
    src/core/simplify_baseline.cpp
)
target_include_directories(glint_qem_core_stub PUBLIC ${GLINT_QEM_TOOL_INCLUDE_DIR})
target_compile_features(glint_qem_core_stub PUBLIC cxx_std_17)

# ---- qem_backends: OBJ IO + preview-staging helpers used by the GUI --------
add_library(glint_qem_tool_backends STATIC
    src/backends/mesh_preview_staging.cpp
    src/backends/obj_mesh_io_tinyobj.cpp
)
target_include_directories(glint_qem_tool_backends PUBLIC ${GLINT_QEM_TOOL_INCLUDE_DIR})
set(GLINT_QEM_TINYOBJ_HINT_DIR "${GLINT_QEM_TOOL_ROOT}/third_party/tinyobjloader")
if (EXISTS "${GLINT_QEM_TINYOBJ_HINT_DIR}")
    target_include_directories(glint_qem_tool_backends PUBLIC "${GLINT_QEM_TINYOBJ_HINT_DIR}")
endif()
target_compile_features(glint_qem_tool_backends PUBLIC cxx_std_17)
target_link_libraries(glint_qem_tool_backends PUBLIC glint_qem_core_stub)

# ---- Standalone GUI target -------------------------------------------------
# Uses GLFW + OpenGL + ImGui, picked up from the repo's vendored third_party/
# tree. The GUI is fully self-contained.
set(GLINT_QEM_THIRD_PARTY_ROOT "${GLINT_QEM_TOOL_ROOT}/third_party")
set(GLINT_QEM_IMGUI_DIR    "${GLINT_QEM_THIRD_PARTY_ROOT}/imgui")
set(GLINT_QEM_GLFW_DIR     "${GLINT_QEM_THIRD_PARTY_ROOT}/glfw")

# Only build the GUI if its required deps are actually present. This lets
# lightweight CI checks configure against a partial vendored tree without
# failing the whole project.
set(_GUI_DEPS_OK TRUE)
if (NOT EXISTS "${GLINT_QEM_IMGUI_DIR}/imgui.cpp")
    message(STATUS "glint_qem_studio_gui: imgui sources not found; skipping GUI target")
    set(_GUI_DEPS_OK FALSE)
endif()
if (NOT EXISTS "${GLINT_QEM_GLFW_DIR}/include/GLFW/glfw3.h")
    message(STATUS "glint_qem_studio_gui: managed GLFW headers not found; skipping GUI target")
    set(_GUI_DEPS_OK FALSE)
endif()

if (_GUI_DEPS_OK)
    # Prefer glfw3.lib (default MSVC /MD runtime) over glfw3_mt.lib (/MT).
    set(GLINT_QEM_GLFW_LIB "")
    if (EXISTS "${GLINT_QEM_GLFW_DIR}/lib/glfw3.lib")
        set(GLINT_QEM_GLFW_LIB "${GLINT_QEM_GLFW_DIR}/lib/glfw3.lib")
    elseif (EXISTS "${GLINT_QEM_GLFW_DIR}/lib/glfw3_mt.lib")
        set(GLINT_QEM_GLFW_LIB "${GLINT_QEM_GLFW_DIR}/lib/glfw3_mt.lib")
    endif()

    if (NOT GLINT_QEM_GLFW_LIB)
        message(STATUS "glint_qem_studio_gui: no GLFW .lib under ${GLINT_QEM_GLFW_DIR}/lib; skipping GUI target")
    else()
        # ImGui built as its own static lib so we can apply /W3 selectively
        # without polluting the GUI executable's warning level.
        add_library(glint_qem_imgui STATIC
            "${GLINT_QEM_IMGUI_DIR}/imgui.cpp"
            "${GLINT_QEM_IMGUI_DIR}/imgui_draw.cpp"
            "${GLINT_QEM_IMGUI_DIR}/imgui_tables.cpp"
            "${GLINT_QEM_IMGUI_DIR}/imgui_widgets.cpp"
            "${GLINT_QEM_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
            "${GLINT_QEM_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
        )
        target_include_directories(glint_qem_imgui PUBLIC
            "${GLINT_QEM_IMGUI_DIR}"
            "${GLINT_QEM_IMGUI_DIR}/backends"
            "${GLINT_QEM_GLFW_DIR}/include"
        )
        target_compile_features(glint_qem_imgui PUBLIC cxx_std_17)
        if (MSVC)
            target_compile_options(glint_qem_imgui PRIVATE /W3 /wd4996)
        endif()

        # Wrap glad.c in its own C static lib. That guarantees the compiler
        # treats it as C (some MSVC toolchains need the explicit language
        # declaration) and keeps it out of the C++ source list below.
        add_library(glint_qem_glad STATIC
            "${GLINT_QEM_THIRD_PARTY_ROOT}/glad/glad.c"
        )
        target_include_directories(glint_qem_glad PUBLIC
            "${GLINT_QEM_THIRD_PARTY_ROOT}"
        )

        add_executable(glint_qem_studio_gui
            src/gui/main.cpp
        )
        target_include_directories(glint_qem_studio_gui PRIVATE
            ${GLINT_QEM_TOOL_INCLUDE_DIR}
            # third_party/ is the include root that resolves <glad/glad.h> and
            # <glm/glm.hpp> in a single entry.
            "${GLINT_QEM_THIRD_PARTY_ROOT}"
            "${GLINT_QEM_GLFW_DIR}/include"
        )
        target_compile_features(glint_qem_studio_gui PRIVATE cxx_std_17)
        target_link_libraries(glint_qem_studio_gui PRIVATE
            glint_qem_tool_backends
            glint_qem_imgui
            glint_qem_glad
            ${GLINT_QEM_GLFW_LIB}
        )
        if (WIN32)
            target_link_libraries(glint_qem_studio_gui PRIVATE opengl32 gdi32 user32 shell32)
        else()
            find_package(OpenGL REQUIRED)
            target_link_libraries(glint_qem_studio_gui PRIVATE OpenGL::GL dl pthread)
        endif()
    endif()
endif()

if (MSVC)
    target_compile_options(glint_qem_core_stub     PRIVATE /W4)
    target_compile_options(glint_qem_tool_backends PRIVATE /W4)
    if (TARGET glint_qem_studio_gui)
        target_compile_options(glint_qem_studio_gui PRIVATE /W3)
    endif()
else()
    target_compile_options(glint_qem_core_stub     PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(glint_qem_tool_backends PRIVATE -Wall -Wextra -Wpedantic)
    if (TARGET glint_qem_studio_gui)
        target_compile_options(glint_qem_studio_gui PRIVATE -Wall -Wextra)
    endif()
endif()
