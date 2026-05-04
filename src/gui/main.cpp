#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "glint_qem/simplify.h"
#include "glint_qem_tool/mesh_preview_staging.h"
#include "glint_qem_tool/obj_mesh_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// NOMINMAX stops <windows.h> from #defining min/max macros, which would
// shadow std::min / std::max / glm::min / glm::max throughout this file.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#endif

namespace {

constexpr int          kInitialWindowWidth  = 1280;
constexpr int          kInitialWindowHeight = 800;
constexpr const char*  kWindowTitle         = "QEM Studio";
constexpr float        kPanelWidth          = 420.0f;

// Axis-aligned bounding box, computed whenever we load or generate a new
// mesh. Used by the orbit camera's Frame() helper and by feature-overlay
// sizing heuristics.

struct MeshBoundingBox {
    glm::vec3 min_corner{0.0f};
    glm::vec3 max_corner{0.0f};
    bool valid = false;

    glm::vec3 Center() const { return 0.5f * (min_corner + max_corner); }
    float Diagonal() const   { return glm::length(max_corner - min_corner); }
};

// Render-ready mesh: interleavable positions + smooth normals + triangle
// indices + precomputed bbox. This is the shape the viewport's draw path
// wants; we convert to/from the engine-agnostic glint_qem mesh type when
// talking to the simplifier.
struct CpuMesh {
    std::vector<glm::vec3>     positions;
    std::vector<glm::vec3>     normals;
    std::vector<std::uint32_t> indices;
    MeshBoundingBox            bounds;
    std::string                source_path;
    std::uint32_t              triangle_count = 0u;
};

// Compute area-weighted smooth per-vertex normals from triangle indices. Our
// OBJ loader only gives us positions + indices, and the simplifier collapses
// vertices, so we always rebuild normals instead of trusting input data.
void ComputeSmoothNormals(const std::vector<glm::vec3>& positions,
                          const std::vector<std::uint32_t>& indices,
                          std::vector<glm::vec3>& out_normals) {
    out_normals.assign(positions.size(), glm::vec3(0.0f));

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t index_a = indices[i + 0];
        const std::uint32_t index_b = indices[i + 1];
        const std::uint32_t index_c = indices[i + 2];
        if (index_a >= positions.size() ||
            index_b >= positions.size() ||
            index_c >= positions.size()) {
            continue;
        }

        // Non-normalized face normal; its magnitude is 2 * triangle_area,
        // which gives a natural area weighting when we sum onto each vertex.
        const glm::vec3& a = positions[index_a];
        const glm::vec3& b = positions[index_b];
        const glm::vec3& c = positions[index_c];
        const glm::vec3 face_normal = glm::cross(b - a, c - a);

        out_normals[index_a] += face_normal;
        out_normals[index_b] += face_normal;
        out_normals[index_c] += face_normal;
    }

    for (glm::vec3& normal : out_normals) {
        const float length = glm::length(normal);
        normal = (length > 0.0f)
            ? normal / length
            : glm::vec3(0.0f, 1.0f, 0.0f); // orphan vertex -> point up
    }
}

// Fill a MeshBoundingBox from a position array. Empty input leaves bounds
// marked invalid so callers know not to use it.
void ComputeBounds(const std::vector<glm::vec3>& positions,
                   MeshBoundingBox& out) {
    if (positions.empty()) {
        out.valid = false;
        out.min_corner = glm::vec3(0.0f);
        out.max_corner = glm::vec3(0.0f);
        return;
    }
    out.valid = true;
    out.min_corner = positions[0];
    out.max_corner = positions[0];
    for (const glm::vec3& position : positions) {
        out.min_corner = glm::min(out.min_corner, position);
        out.max_corner = glm::max(out.max_corner, position);
    }
}

// Load an OBJ from disk into a CpuMesh. On failure, writes a human-readable
// message into `error_out` and returns false.
bool LoadObjIntoCpuMesh(const std::string& path,
                        CpuMesh& out,
                        std::string& error_out) {
    glint_qem::IndexedTriangleMesh raw;
    std::string load_error;
    if (!glint_qem_tool::ReadObjMesh(path, raw, &load_error)) {
        error_out = load_error.empty() ? "failed to read OBJ" : load_error;
        return false;
    }
    if ((raw.indices.size() % 3u) != 0u) {
        error_out = "OBJ has non-triangle faces";
        return false;
    }

    out.positions.clear();
    out.positions.reserve(raw.positions.size());
    for (const glint_qem::Vec3f& position : raw.positions) {
        out.positions.emplace_back(position.x, position.y, position.z);
    }
    out.indices = std::move(raw.indices);
    ComputeSmoothNormals(out.positions, out.indices, out.normals);
    ComputeBounds(out.positions, out.bounds);
    out.source_path = path;
    out.triangle_count = static_cast<std::uint32_t>(out.indices.size() / 3u);
    return true;
}

// Owns a VAO/VBO/EBO. Refilled wholesale on every new mesh load.

struct GpuMesh {
    GLuint vao = 0u;
    GLuint vbo = 0u;         // interleaved position (3f) + normal (3f)
    GLuint ebo = 0u;
    GLsizei index_count = 0;
    bool ready = false;
};

// Release GL objects and reset to zero-initialized state.
void DestroyGpuMesh(GpuMesh& mesh) {
    if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    mesh = GpuMesh{};
}

// Upload positions+normals+indices to GL, replacing any previous contents.
// If the input is empty, the GpuMesh is reset to "not ready" and no draw
// call should be issued against it.
void UploadCpuMeshToGpu(const CpuMesh& cpu, GpuMesh& gpu) {
    DestroyGpuMesh(gpu);
    if (cpu.positions.empty() || cpu.indices.empty()) return;

    // Interleave: [px py pz nx ny nz] per vertex.
    std::vector<float> interleaved(cpu.positions.size() * 6u);
    for (std::size_t i = 0; i < cpu.positions.size(); ++i) {
        const glm::vec3& p = cpu.positions[i];
        const glm::vec3& n = cpu.normals[i];
        float* row = &interleaved[i * 6u];
        row[0] = p.x; row[1] = p.y; row[2] = p.z;
        row[3] = n.x; row[4] = n.y; row[5] = n.z;
    }

    glGenVertexArrays(1, &gpu.vao);
    glGenBuffers(1, &gpu.vbo);
    glGenBuffers(1, &gpu.ebo);

    glBindVertexArray(gpu.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
                 interleaved.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(cpu.indices.size() * sizeof(std::uint32_t)),
                 cpu.indices.data(),
                 GL_STATIC_DRAW);

    const GLsizei stride = 6 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));

    glBindVertexArray(0);
    gpu.index_count = static_cast<GLsizei>(cpu.indices.size());
    gpu.ready = true;
}

// The feature-analysis overlays are a *separate* rendering path from the
// mesh: we draw boundary edges, crease edges, and curvature hotspots as
// plain GL_LINES / GL_POINTS so we can refill them each time the user
// moves a detection slider, without touching the mesh VAO.

struct LineBuffer {
    GLuint vao = 0u;
    GLuint vbo = 0u;
    GLsizei vertex_count = 0; // two per line segment
};

struct PointBuffer {
    GLuint vao = 0u;
    GLuint vbo = 0u;
    GLsizei vertex_count = 0;
};

void DestroyLineBuffer(LineBuffer& buffer) {
    if (buffer.vbo) glDeleteBuffers(1, &buffer.vbo);
    if (buffer.vao) glDeleteVertexArrays(1, &buffer.vao);
    buffer = LineBuffer{};
}

void DestroyPointBuffer(PointBuffer& buffer) {
    if (buffer.vbo) glDeleteBuffers(1, &buffer.vbo);
    if (buffer.vao) glDeleteVertexArrays(1, &buffer.vao);
    buffer = PointBuffer{};
}

// Upload a flat (x, y, z, x, y, z, ...) vertex array to a VAO/VBO pair,
// creating the handles on first call. Caller chooses GL_LINES or GL_POINTS
// when issuing the draw call.
void UploadPositionsToBuffer(GLuint& vao,
                             GLuint& vbo,
                             GLsizei& vertex_count,
                             const std::vector<float>& data_xyz) {
    if (vao == 0u) glGenVertexArrays(1, &vao);
    if (vbo == 0u) glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data_xyz.size() * sizeof(float)),
                 data_xyz.empty() ? nullptr : data_xyz.data(),
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glBindVertexArray(0);

    vertex_count = static_cast<GLsizei>(data_xyz.size() / 3u);
}

// Refill the three overlay buffers (boundary lines, sharp lines, curvature
// points) from a FeatureAnalysis result. Curvature points are filtered to
// the top N% of factors above a "definitely not flat" cutoff so the overlay
// doesn't spam dots all over mostly-flat meshes.
void UploadOverlaysFromAnalysis(const CpuMesh& cpu,
                                const glint_qem::FeatureAnalysis& analysis,
                                float curvature_top_percent,
                                LineBuffer& boundary_lines,
                                LineBuffer& sharp_lines,
                                PointBuffer& curvature_points) {
    // Boundary edges -> two XYZ vertices each.
    std::vector<float> boundary_data;
    boundary_data.reserve(analysis.boundary_edges.size() * 6u);
    for (const glint_qem::FeatureEdge& edge : analysis.boundary_edges) {
        if (edge.vertex_a >= cpu.positions.size() ||
            edge.vertex_b >= cpu.positions.size()) continue;
        const glm::vec3& a = cpu.positions[edge.vertex_a];
        const glm::vec3& b = cpu.positions[edge.vertex_b];
        boundary_data.push_back(a.x); boundary_data.push_back(a.y); boundary_data.push_back(a.z);
        boundary_data.push_back(b.x); boundary_data.push_back(b.y); boundary_data.push_back(b.z);
    }
    UploadPositionsToBuffer(boundary_lines.vao, boundary_lines.vbo,
                            boundary_lines.vertex_count, boundary_data);

    // Sharp/crease edges.
    std::vector<float> sharp_data;
    sharp_data.reserve(analysis.sharp_edges.size() * 6u);
    for (const glint_qem::FeatureEdge& edge : analysis.sharp_edges) {
        if (edge.vertex_a >= cpu.positions.size() ||
            edge.vertex_b >= cpu.positions.size()) continue;
        const glm::vec3& a = cpu.positions[edge.vertex_a];
        const glm::vec3& b = cpu.positions[edge.vertex_b];
        sharp_data.push_back(a.x); sharp_data.push_back(a.y); sharp_data.push_back(a.z);
        sharp_data.push_back(b.x); sharp_data.push_back(b.y); sharp_data.push_back(b.z);
    }
    UploadPositionsToBuffer(sharp_lines.vao, sharp_lines.vbo,
                            sharp_lines.vertex_count, sharp_data);

    // Curvature: pick top-N% vertices whose factor exceeds a mild cutoff.
    constexpr float kMinMarkerFactor = 1.05f;

    std::vector<std::uint32_t> candidate_ids;
    candidate_ids.reserve(analysis.vertex_curvature_factors.size());
    for (std::uint32_t i = 0u;
         i < static_cast<std::uint32_t>(analysis.vertex_curvature_factors.size());
         ++i) {
        if (i >= cpu.positions.size()) continue;
        if (analysis.vertex_curvature_factors[i] >= kMinMarkerFactor) {
            candidate_ids.push_back(i);
        }
    }
    std::sort(candidate_ids.begin(), candidate_ids.end(),
              [&](std::uint32_t a, std::uint32_t b) {
                  return analysis.vertex_curvature_factors[a] >
                         analysis.vertex_curvature_factors[b];
              });

    const float clamped_percent = std::clamp(curvature_top_percent, 0.0f, 1.0f);
    std::size_t keep_count =
        static_cast<std::size_t>(clamped_percent * candidate_ids.size());
    if (!candidate_ids.empty() && keep_count == 0u) keep_count = 1u;
    if (keep_count > candidate_ids.size())           keep_count = candidate_ids.size();
    candidate_ids.resize(keep_count);

    std::vector<float> curvature_data;
    curvature_data.reserve(candidate_ids.size() * 3u);
    for (std::uint32_t vertex_id : candidate_ids) {
        const glm::vec3& p = cpu.positions[vertex_id];
        curvature_data.push_back(p.x);
        curvature_data.push_back(p.y);
        curvature_data.push_back(p.z);
    }
    UploadPositionsToBuffer(curvature_points.vao, curvature_points.vbo,
                            curvature_points.vertex_count, curvature_data);
}

// Shader source lives in `resources/shaders/` next to the executable. We look in
// a small list of candidate paths so the same build works both from the dev
// tree (exe sits under `builds/.../Release/`) and the submission folder
// (exe sits next to a `resources/` directory).

// Read an entire text file into a string. Returns empty on failure.
std::string ReadFileToString(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) return {};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Look up a shader by relative path under resources/shaders/. Searches a
// small list of roots so we work regardless of the exe's CWD. Returns
// the file contents or empty if none of the candidates existed.
std::string LoadShaderSource(const std::string& filename) {
    const std::array<std::string, 6> candidate_roots = {
        // 1) Next to the exe (submission layout).
        "resources/shaders/",
        // 2-5) Dev-tree layouts where the exe sits N folders deep inside
        //      builds/qem_simplifier/cmake/<Config>/ but is launched from
        //      the repo root.
        "apps/qem_simplifier/resources/shaders/",
        "../resources/shaders/",
        "../../resources/shaders/",
        "../../../resources/shaders/",
        // 6) Last-ditch absolute-in-repo path, which at least gives a
        //    friendly error mentioning the right location.
        "../../../apps/qem_simplifier/resources/shaders/",
    };

    for (const std::string& root : candidate_roots) {
        const std::string candidate = root + filename;
        std::string contents = ReadFileToString(candidate);
        if (!contents.empty()) return contents;
    }
    return {};
}

// Compile a single shader stage. On failure, copies the GL info log into
// `error_out` and returns 0.
GLuint CompileShader(GLenum stage, const char* source, std::string& error_out) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status) return shader;

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string info_log(static_cast<std::size_t>(log_length), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, info_log.data());
    error_out = info_log;
    glDeleteShader(shader);
    return 0u;
}

// Link a vertex+fragment shader pair into a program. Caller still owns the
// individual shader objects.
GLuint LinkProgram(GLuint vertex_shader,
                   GLuint fragment_shader,
                   std::string& error_out) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status) return program;

    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    std::string info_log(static_cast<std::size_t>(log_length), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, info_log.data());
    error_out = info_log;
    glDeleteProgram(program);
    return 0u;
}

// Compile + link the given vertex/fragment shader filenames under
// resources/shaders/. Prints any errors to stderr. Returns 0 on failure.
GLuint BuildShaderProgram(const char* vertex_filename,
                          const char* fragment_filename,
                          const char* human_label) {
    const std::string vertex_source   = LoadShaderSource(vertex_filename);
    const std::string fragment_source = LoadShaderSource(fragment_filename);
    if (vertex_source.empty()) {
        std::fprintf(stderr, "[%s] missing shader file: %s\n",
                     human_label, vertex_filename);
        return 0u;
    }
    if (fragment_source.empty()) {
        std::fprintf(stderr, "[%s] missing shader file: %s\n",
                     human_label, fragment_filename);
        return 0u;
    }

    std::string error;
    GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, vertex_source.c_str(), error);
    if (!vertex_shader) {
        std::fprintf(stderr, "[%s vs] %s\n", human_label, error.c_str());
        return 0u;
    }
    GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, fragment_source.c_str(), error);
    if (!fragment_shader) {
        std::fprintf(stderr, "[%s fs] %s\n", human_label, error.c_str());
        glDeleteShader(vertex_shader);
        return 0u;
    }

    GLuint program = LinkProgram(vertex_shader, fragment_shader, error);
    if (!program) {
        std::fprintf(stderr, "[%s link] %s\n", human_label, error.c_str());
    }
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    return program;
}

// Convenience helpers for the two programs we actually ship.
GLuint BuildMeshProgram()    { return BuildShaderProgram("mesh.vert", "mesh.frag", "mesh"); }
GLuint BuildOverlayProgram() { return BuildShaderProgram("overlay.vert", "overlay.frag", "overlay"); }

struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance       = 3.0f;
    float yaw_degrees    = 35.0f; // around world Y
    float pitch_degrees  = 25.0f; // around camera right

    // Fit the camera around a bbox so a freshly loaded mesh is fully visible.
    void Frame(const MeshBoundingBox& bbox) {
        if (!bbox.valid) {
            target = glm::vec3(0.0f);
            distance = 3.0f;
            return;
        }
        target = bbox.Center();
        // Rule of thumb for 45 deg FOV: camera distance ~= 1.5x bbox diagonal.
        const float diagonal = std::max(bbox.Diagonal(), 0.001f);
        distance = diagonal * 1.5f;
        yaw_degrees = 35.0f;
        pitch_degrees = 25.0f;
    }

    glm::vec3 EyePosition() const {
        const float yaw_radians   = glm::radians(yaw_degrees);
        const float pitch_radians = glm::radians(pitch_degrees);
        const glm::vec3 direction{
            std::cos(pitch_radians) * std::sin(yaw_radians),
            std::sin(pitch_radians),
            std::cos(pitch_radians) * std::cos(yaw_radians),
        };
        return target + direction * distance;
    }

    glm::mat4 ViewMatrix() const {
        return glm::lookAt(EyePosition(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 ProjectionMatrix(float viewport_aspect) const {
        const float near_z = std::max(0.001f, distance * 0.01f);
        const float far_z  = std::max(near_z + 0.1f, distance * 10.0f);
        return glm::perspective(glm::radians(45.0f), viewport_aspect, near_z, far_z);
    }
};

struct InputState {
    bool   left_dragging  = false;
    bool   right_dragging = false;
    double last_x = 0.0;
    double last_y = 0.0;
};

// Windows-only native file dialogs. On non-Windows platforms both helpers return
// an empty string; the GUI gracefully treats that as "user cancelled."

std::string OpenObjFileDialog(GLFWwindow* owning_window) {
#if defined(_WIN32)
    (void)owning_window;
    char file_buffer[MAX_PATH] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = "OBJ Mesh (*.obj)\0*.obj\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = file_buffer;
    ofn.nMaxFile    = sizeof(file_buffer);
    ofn.Flags       = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle  = "Open Mesh";
    if (GetOpenFileNameA(&ofn) == TRUE) return std::string(file_buffer);
    return {};
#else
    (void)owning_window;
    return {};
#endif
}

std::string SaveObjFileDialog(GLFWwindow* owning_window,
                              const std::string& default_name) {
#if defined(_WIN32)
    (void)owning_window;
    char file_buffer[MAX_PATH] = {0};
    if (!default_name.empty() && default_name.size() < MAX_PATH) {
        std::copy(default_name.begin(), default_name.end(), file_buffer);
        file_buffer[default_name.size()] = '\0';
    }
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = "OBJ Mesh (*.obj)\0*.obj\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = file_buffer;
    ofn.nMaxFile    = sizeof(file_buffer);
    ofn.Flags       = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "obj";
    ofn.lpstrTitle  = "Save Simplified Mesh";
    if (GetSaveFileNameA(&ofn) == TRUE) return std::string(file_buffer);
    return {};
#else
    (void)owning_window; (void)default_name;
    return {};
#endif
}

// Values that drive the feature analysis. The show_* booleans are purely
// visual; they don't re-run the analysis because they only toggle the
// overlay-draw pass.
struct AnalysisParams {
    float sharp_edge_angle_degrees = 40.0f;
    float curvature_strength       = 2.0f;
    float curvature_top_percent    = 0.10f;

    bool  show_boundary  = true;
    bool  show_sharp     = true;
    bool  show_curvature = true;
};

struct AnalysisState {
    glint_qem::FeatureAnalysis result;
    bool valid = false; // true once we've run analysis on the current mesh
};

struct OverlayBuffers {
    LineBuffer  boundary_lines;
    LineBuffer  sharp_lines;
    PointBuffer curvature_points;
};

// Inputs to Simplify. target_percent=30 means "keep 30% of triangles".
//
// Each preservation feature has two knobs:
//   preserve_*  -- enable the soft quadric penalty (always on when feature on).
//   *_strict    -- additionally enforce the hard rules that actually guarantee
//                  the feature is preserved. Off = penalty-only (old behavior,
//                  useful if you want holes to close). On = guaranteed.
struct SimplifyParams {
    int   target_percent       = 30;
    bool  preserve_boundary    = true;
    bool  boundary_strict      = true;
    bool  preserve_sharp       = true;
    bool  sharp_edge_strict    = true;
    bool  preserve_curvature   = true;
    float boundary_weight      = 1000.0f;
    float sharp_weight         = 1000.0f;
    float curvature_strength   = 2.0f;
};

// Output of one simplification run. The simplified mesh is kept both on
// CPU (for OBJ export) and on GPU (for the viewport's view-mode radio).
struct SimplifyOutput {
    bool                         valid          = false;
    glint_qem::SimplifyStats     stats{};
    glint_qem::SimplifyStatus    status         = glint_qem::SimplifyStatus::kInternalError;
    std::string                  status_message;
    double                       elapsed_seconds = 0.0;
    CpuMesh                      simplified_cpu;
    GpuMesh                      simplified_gpu;
};

enum class ViewMode {
    kOriginal   = 0,
    kSimplified = 1,
};

// How to draw the active mesh in the viewport.
//   kSolid     - filled triangles with a single directional light.
//   kWireframe - just triangle edges on a dark background.
//   kBoth      - lit solid pass + wireframe edges drawn on top.
enum class DisplayMode {
    kSolid     = 0,
    kWireframe = 1,
    kBoth      = 2,
};

// Simplification runs on a worker thread so the UI stays responsive on
// large meshes. State machine: kIdle -> kRunning -> kReadyToConsume -> kIdle.
enum class SimplifyJobState : int {
    kIdle           = 0,
    kRunning        = 1,
    kReadyToConsume = 2,
};

// Thread-owned inputs + outputs. The worker reads `input_mesh` + `options`,
// writes `output_mesh` + `result`, then flips `state` to kReadyToConsume.
// The main thread joins the worker before reading any outputs.
struct SimplifyJob {
    std::atomic<SimplifyJobState>             state{SimplifyJobState::kIdle};
    std::chrono::steady_clock::time_point     start_time{};
    std::chrono::steady_clock::time_point     end_time{};
    std::thread                               worker;

    glint_qem::IndexedTriangleMesh            input_mesh;  // in
    glint_qem::SimplifyOptions                options;     // in
    glint_qem::IndexedTriangleMesh            output_mesh; // out
    glint_qem::SimplifyResult                 result;      // out
};

struct AppState {
    GLFWwindow* window          = nullptr;
    GLuint      mesh_program    = 0u;
    GLuint      overlay_program = 0u;

    CpuMesh     cpu_mesh;
    GpuMesh     gpu_mesh;

    OrbitCamera camera;
    InputState  input;

    AnalysisParams analysis_params;
    AnalysisState  analysis;
    OverlayBuffers overlays;

    // Used to decide whether the analysis needs re-running: when the current
    // params equal the params we last used, skip the work.
    AnalysisParams last_applied_params;
    bool           analysis_needs_rebuild = true;

    SimplifyParams simplify_params;
    SimplifyOutput simplify_output;
    ViewMode       view_mode    = ViewMode::kOriginal;
    DisplayMode    display_mode = DisplayMode::kBoth;

    // Compare mode: when enabled, the viewport splits in half and draws the
    // compare mesh in the right pane sharing the main camera. The compare
    // slot is independent from main and from the simplifier.
    bool        compare_enabled = false;
    CpuMesh     compare_cpu;
    GpuMesh     compare_gpu;
    std::string compare_label; // user-visible label for the B slot

    SimplifyJob simplify_job;

    std::string last_error_message; // surfaced at the top of the panel
};

AppState g_app;

// Detects whether two AnalysisParams snapshots differ in any field that
// actually affects the analysis output. The show_* booleans are ignored
// because they don't influence what gets computed.
bool AnalysisInputsChanged(const AnalysisParams& a, const AnalysisParams& b) {
    return (a.sharp_edge_angle_degrees != b.sharp_edge_angle_degrees) ||
           (a.curvature_strength       != b.curvature_strength)       ||
           (a.curvature_top_percent    != b.curvature_top_percent);
}

// Convert a CpuMesh to the engine-agnostic glint_qem::IndexedTriangleMesh
// that both AnalyzeFeatures and Simplify consume.
glint_qem::IndexedTriangleMesh CpuMeshToQemMesh(const CpuMesh& mesh) {
    glint_qem::IndexedTriangleMesh out;
    out.positions.reserve(mesh.positions.size());
    for (const glm::vec3& position : mesh.positions) {
        out.positions.push_back({position.x, position.y, position.z});
    }
    out.indices = mesh.indices;
    return out;
}

// Convert a simplifier-output mesh back into a render-ready CpuMesh. Normals
// are recomputed from scratch because the simplifier moves vertices, so the
// old per-vertex normals no longer match the new surface.
CpuMesh QemMeshToCpuMesh(const glint_qem::IndexedTriangleMesh& in,
                         const std::string& source_label) {
    CpuMesh out;
    out.positions.reserve(in.positions.size());
    for (const glint_qem::Vec3f& position : in.positions) {
        out.positions.emplace_back(position.x, position.y, position.z);
    }
    out.indices = in.indices;
    ComputeSmoothNormals(out.positions, out.indices, out.normals);
    ComputeBounds(out.positions, out.bounds);
    out.source_path    = source_label;
    out.triangle_count = static_cast<std::uint32_t>(out.indices.size() / 3u);
    return out;
}

// Run AnalyzeFeatures against the current main mesh and refill the overlay
// buffers. Safe when no mesh is loaded (clears the overlays).
void RebuildAnalysis() {
    if (g_app.cpu_mesh.positions.empty()) {
        g_app.analysis.result = {};
        g_app.analysis.valid = false;
        UploadPositionsToBuffer(g_app.overlays.boundary_lines.vao,
                                g_app.overlays.boundary_lines.vbo,
                                g_app.overlays.boundary_lines.vertex_count, {});
        UploadPositionsToBuffer(g_app.overlays.sharp_lines.vao,
                                g_app.overlays.sharp_lines.vbo,
                                g_app.overlays.sharp_lines.vertex_count, {});
        UploadPositionsToBuffer(g_app.overlays.curvature_points.vao,
                                g_app.overlays.curvature_points.vbo,
                                g_app.overlays.curvature_points.vertex_count, {});
        g_app.last_applied_params     = g_app.analysis_params;
        g_app.analysis_needs_rebuild  = false;
        return;
    }

    glint_qem::SimplifyOptions options;
    options.sharp_edge_angle_degrees = g_app.analysis_params.sharp_edge_angle_degrees;
    options.curvature_strength       = g_app.analysis_params.curvature_strength;

    g_app.analysis.result = glint_qem::AnalyzeFeatures(
        CpuMeshToQemMesh(g_app.cpu_mesh), options);
    g_app.analysis.valid = true;

    UploadOverlaysFromAnalysis(g_app.cpu_mesh,
                               g_app.analysis.result,
                               g_app.analysis_params.curvature_top_percent,
                               g_app.overlays.boundary_lines,
                               g_app.overlays.sharp_lines,
                               g_app.overlays.curvature_points);

    g_app.last_applied_params    = g_app.analysis_params;
    g_app.analysis_needs_rebuild = false;
}

// Kick off a simplify run on a worker thread. Returns immediately; the main
// loop calls MaybeConsumeFinishedSimplification() each frame to pick up the
// result. No-op if nothing's loaded or a run is already in flight.
void StartSimplificationAsync() {
    if (!g_app.gpu_mesh.ready || g_app.cpu_mesh.positions.empty()) {
        g_app.last_error_message = "simplify: no mesh loaded";
        return;
    }
    if (g_app.simplify_job.state.load() != SimplifyJobState::kIdle) return;

    // Snapshot inputs onto the job. Copying the mesh decouples the worker
    // from any main-thread edits (e.g. the user loading a different mesh).
    g_app.simplify_job.input_mesh = CpuMeshToQemMesh(g_app.cpu_mesh);

    glint_qem::SimplifyOptions& options = g_app.simplify_job.options;
    options = glint_qem::SimplifyOptions{};
    options.target_ratio              = std::clamp(g_app.simplify_params.target_percent, 1, 100) / 100.0f;
    options.preserve_boundary         = g_app.simplify_params.preserve_boundary;
    options.boundary_weight           = g_app.simplify_params.boundary_weight;
    options.boundary_strict           = g_app.simplify_params.boundary_strict;
    options.preserve_sharp_edges      = g_app.simplify_params.preserve_sharp;
    options.sharp_edge_angle_degrees  = g_app.analysis_params.sharp_edge_angle_degrees;
    options.sharp_edge_weight         = g_app.simplify_params.sharp_weight;
    options.sharp_edge_strict         = g_app.simplify_params.sharp_edge_strict;
    options.preserve_curvature        = g_app.simplify_params.preserve_curvature;
    options.curvature_strength        = g_app.simplify_params.curvature_strength;
    options.compact_output            = true;

    g_app.simplify_job.output_mesh = glint_qem::IndexedTriangleMesh{};
    g_app.simplify_job.result      = glint_qem::SimplifyResult{};
    g_app.simplify_job.start_time  = std::chrono::steady_clock::now();
    g_app.simplify_job.state.store(SimplifyJobState::kRunning);

    // Worker body: pure computation -- no GL, no UI, no other globals.
    g_app.simplify_job.worker = std::thread([]() {
        SimplifyJob& job = g_app.simplify_job;
        job.result   = glint_qem::Simplify(job.input_mesh, job.options, job.output_mesh);
        job.end_time = std::chrono::steady_clock::now();
        job.state.store(SimplifyJobState::kReadyToConsume);
    });
}

// Pick up a finished simplify result and upload its mesh to the GPU. Called
// every frame from the main loop; cheap when no result is ready.
void MaybeConsumeFinishedSimplification() {
    if (g_app.simplify_job.state.load() != SimplifyJobState::kReadyToConsume) return;
    if (g_app.simplify_job.worker.joinable()) g_app.simplify_job.worker.join();

    const glint_qem::SimplifyResult& result = g_app.simplify_job.result;
    g_app.simplify_output.status          = result.status;
    g_app.simplify_output.status_message  = result.message ? result.message : "";
    g_app.simplify_output.stats           = result.stats;
    g_app.simplify_output.elapsed_seconds = std::chrono::duration<double>(
        g_app.simplify_job.end_time - g_app.simplify_job.start_time).count();

    const bool run_failed =
        (result.status == glint_qem::SimplifyStatus::kInvalidInput) ||
        (result.status == glint_qem::SimplifyStatus::kInternalError);

    if (run_failed) {
        g_app.simplify_output.valid = false;
        g_app.last_error_message =
            "simplify failed: " + g_app.simplify_output.status_message;
    } else {
        g_app.simplify_output.simplified_cpu =
            QemMeshToCpuMesh(g_app.simplify_job.output_mesh, "<simplified>");
        UploadCpuMeshToGpu(g_app.simplify_output.simplified_cpu,
                           g_app.simplify_output.simplified_gpu);
        g_app.simplify_output.valid = true;
        g_app.last_error_message.clear();
        g_app.view_mode = ViewMode::kSimplified;
    }

    // Release the large transient buffers the job was holding.
    g_app.simplify_job.input_mesh  = glint_qem::IndexedTriangleMesh{};
    g_app.simplify_job.output_mesh = glint_qem::IndexedTriangleMesh{};
    g_app.simplify_job.result      = glint_qem::SimplifyResult{};

    g_app.simplify_job.state.store(SimplifyJobState::kIdle);
}

// Load an OBJ into the compare (right-pane) slot. On failure, surfaces the
// reason via g_app.last_error_message and returns false.
bool LoadCompareMeshFromPath(const std::string& path) {
    CpuMesh next;
    std::string load_error;
    if (!LoadObjIntoCpuMesh(path, next, load_error)) {
        g_app.last_error_message = "compare load failed: " + load_error;
        return false;
    }
    g_app.compare_cpu = std::move(next);
    UploadCpuMeshToGpu(g_app.compare_cpu, g_app.compare_gpu);
    g_app.compare_label =
        std::filesystem::path(g_app.compare_cpu.source_path).filename().string();
    g_app.last_error_message.clear();
    return true;
}

// Mirror the latest simplify result into the compare slot. No-op if no
// simplify result is available.
void PopulateCompareFromSimplified() {
    if (!g_app.simplify_output.valid) return;
    g_app.compare_cpu = g_app.simplify_output.simplified_cpu;
    UploadCpuMeshToGpu(g_app.compare_cpu, g_app.compare_gpu);
    g_app.compare_label = "<simplified>";
}

// Swap main and compare slots (CPU + GPU). Stale analysis and any previous
// simplify result are dropped because they referenced the old main mesh.
void SwapMainAndCompareMeshes() {
    if (!g_app.gpu_mesh.ready || !g_app.compare_gpu.ready) return;

    std::swap(g_app.cpu_mesh, g_app.compare_cpu);
    std::swap(g_app.gpu_mesh, g_app.compare_gpu);

    // Analysis was computed against the old main mesh; invalidate.
    g_app.analysis_needs_rebuild = true;

    // Any prior simplify result was derived from the old main mesh too.
    DestroyGpuMesh(g_app.simplify_output.simplified_gpu);
    g_app.simplify_output = SimplifyOutput{};
    g_app.view_mode = ViewMode::kOriginal;

    g_app.compare_label =
        std::filesystem::path(g_app.compare_cpu.source_path).filename().string();
    g_app.camera.Frame(g_app.cpu_mesh.bounds);
}

// GLFW input callbacks. These are installed before ImGui's own GLFW callbacks
// so ImGui can chain to them (see main()). Each early-returns if ImGui wants
// the mouse so panel widgets don't also orbit the camera.

void OnMouseButton(GLFWwindow* /*window*/, int button, int action, int /*mods*/) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_app.input.left_dragging  = (action == GLFW_PRESS);
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        g_app.input.right_dragging = (action == GLFW_PRESS);
    }
}

void OnCursorPos(GLFWwindow* /*window*/, double xpos, double ypos) {
    const double dx = xpos - g_app.input.last_x;
    const double dy = ypos - g_app.input.last_y;
    g_app.input.last_x = xpos;
    g_app.input.last_y = ypos;

    if (ImGui::GetIO().WantCaptureMouse) return;

    if (g_app.input.left_dragging) {
        // Left drag = orbit. Horizontal mouse = yaw, vertical mouse = pitch.
        g_app.camera.yaw_degrees   -= static_cast<float>(dx) * 0.4f;
        g_app.camera.pitch_degrees += static_cast<float>(dy) * 0.4f;
        g_app.camera.pitch_degrees = std::clamp(g_app.camera.pitch_degrees, -89.0f, 89.0f);
        return;
    }

    if (g_app.input.right_dragging) {
        // Right drag = pan along camera-aligned right/up axes. Scaled by
        // distance so pan speed feels identical at any zoom.
        const glm::vec3 eye     = g_app.camera.EyePosition();
        const glm::vec3 forward = glm::normalize(g_app.camera.target - eye);
        const glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 up      = glm::normalize(glm::cross(right, forward));
        const float     scale   = g_app.camera.distance * 0.0015f;
        g_app.camera.target -= right * (static_cast<float>(dx) * scale);
        g_app.camera.target += up    * (static_cast<float>(dy) * scale);
    }
}

void OnScroll(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    const float zoom_factor = (yoffset > 0.0) ? 0.9f : 1.1f;
    g_app.camera.distance = std::clamp(g_app.camera.distance * zoom_factor, 0.01f, 10000.0f);
}

// Describes a rectangular sub-region of the default framebuffer in pixels.
struct ViewportRect {
    int x = 0;
    int y = 0;
    int width  = 1;
    int height = 1;
};

// Draw one mesh pane: optional solid pass, optional wireframe pass, optional
// feature overlays. Takes the target rect, which mesh to draw, and whether
// to render the analysis overlays (only true for the main-mesh pane).
void DrawMeshPane(const ViewportRect& rect,
                  const GpuMesh& mesh,
                  bool draw_feature_overlays) {
    if (!mesh.ready || g_app.mesh_program == 0u) return;

    glViewport(rect.x, rect.y, rect.width, rect.height);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    const float aspect =
        static_cast<float>(rect.width) / static_cast<float>(std::max(1, rect.height));
    const glm::mat4 view          = g_app.camera.ViewMatrix();
    const glm::mat4 projection    = g_app.camera.ProjectionMatrix(aspect);
    const glm::mat4 model{1.0f};
    const glm::mat4 view_proj     = projection * view;
    const glm::mat3 normal_matrix = glm::mat3(glm::transpose(glm::inverse(model)));

    // -- Solid pass ----------------------------------------------------------
    const bool draw_solid_pass =
        (g_app.display_mode == DisplayMode::kSolid) ||
        (g_app.display_mode == DisplayMode::kBoth);
    if (draw_solid_pass) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(g_app.mesh_program);
        glUniformMatrix4fv(glGetUniformLocation(g_app.mesh_program, "uModel"),
                           1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix4fv(glGetUniformLocation(g_app.mesh_program, "uViewProj"),
                           1, GL_FALSE, glm::value_ptr(view_proj));
        glUniformMatrix3fv(glGetUniformLocation(g_app.mesh_program, "uNormalMatrix"),
                           1, GL_FALSE, glm::value_ptr(normal_matrix));
        glUniform3f(glGetUniformLocation(g_app.mesh_program, "uBaseColor"),
                    0.80f, 0.80f, 0.85f);
        glUniform3f(glGetUniformLocation(g_app.mesh_program, "uLightDir"),
                    0.5f, 1.0f, 0.6f);
        glUniform3f(glGetUniformLocation(g_app.mesh_program, "uAmbient"),
                    0.18f, 0.18f, 0.20f);

        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.index_count, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

    // -- Wireframe pass ------------------------------------------------------
    const bool draw_wireframe_pass =
        (g_app.display_mode == DisplayMode::kWireframe) ||
        (g_app.display_mode == DisplayMode::kBoth);
    if (draw_wireframe_pass && g_app.overlay_program != 0u) {
        glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(1.0f);

        glUseProgram(g_app.overlay_program);
        glUniformMatrix4fv(glGetUniformLocation(g_app.overlay_program, "uViewProj"),
                           1, GL_FALSE, glm::value_ptr(view_proj));
        // In "Both" mode the wires sit on the surface, so a small NDC bias
        // prevents z-fighting. In wireframe-only mode there's nothing behind
        // them, so no bias is needed.
        const float depth_bias =
            (g_app.display_mode == DisplayMode::kBoth) ? 0.0015f : 0.0f;
        glUniform1f(glGetUniformLocation(g_app.overlay_program, "uDepthBias"),
                    depth_bias);
        glUniform3f(glGetUniformLocation(g_app.overlay_program, "uColor"),
                    0.85f, 0.85f, 0.88f);

        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.index_count, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // -- Feature overlays (main-pane only) -----------------------------------
    glDisable(GL_CULL_FACE);
    const bool overlays_visible =
        draw_feature_overlays &&
        g_app.overlay_program != 0u &&
        g_app.analysis.valid;
    if (overlays_visible) {
        glUseProgram(g_app.overlay_program);
        glUniformMatrix4fv(glGetUniformLocation(g_app.overlay_program, "uViewProj"),
                           1, GL_FALSE, glm::value_ptr(view_proj));
        glUniform1f(glGetUniformLocation(g_app.overlay_program, "uDepthBias"), 0.0015f);
        glLineWidth(2.0f);

        const GLint loc_color =
            glGetUniformLocation(g_app.overlay_program, "uColor");

        if (g_app.analysis_params.show_boundary &&
            g_app.overlays.boundary_lines.vertex_count > 0) {
            glUniform3f(loc_color, 0.95f, 0.15f, 0.15f); // red
            glBindVertexArray(g_app.overlays.boundary_lines.vao);
            glDrawArrays(GL_LINES, 0, g_app.overlays.boundary_lines.vertex_count);
        }
        if (g_app.analysis_params.show_sharp &&
            g_app.overlays.sharp_lines.vertex_count > 0) {
            glUniform3f(loc_color, 1.00f, 0.55f, 0.05f); // orange
            glBindVertexArray(g_app.overlays.sharp_lines.vao);
            glDrawArrays(GL_LINES, 0, g_app.overlays.sharp_lines.vertex_count);
        }
        if (g_app.analysis_params.show_curvature &&
            g_app.overlays.curvature_points.vertex_count > 0) {
            glEnable(GL_PROGRAM_POINT_SIZE);
            glPointSize(6.0f);
            glUniform3f(loc_color, 1.00f, 0.90f, 0.10f); // yellow
            glBindVertexArray(g_app.overlays.curvature_points.vao);
            glDrawArrays(GL_POINTS, 0, g_app.overlays.curvature_points.vertex_count);
        }
        glBindVertexArray(0);
        glLineWidth(1.0f);
    }

    glDisable(GL_DEPTH_TEST);
}

// Pick which mesh the "main" pane should show, respecting the View radio.
const GpuMesh* SelectMainPaneMesh() {
    const bool show_simplified =
        (g_app.view_mode == ViewMode::kSimplified) &&
        g_app.simplify_output.valid &&
        g_app.simplify_output.simplified_gpu.ready;
    return show_simplified ? &g_app.simplify_output.simplified_gpu
                           : &g_app.gpu_mesh;
}

// Per-frame viewport dispatch. One pane in normal mode, two side-by-side
// panes in compare mode; camera is shared in both cases.
void RenderViewport(int window_width, int window_height) {
    const int viewport_width  = std::max(1, window_width - static_cast<int>(kPanelWidth));
    const int viewport_height = std::max(1, window_height);

    // Single clear for the whole viewport so the thin gap between split
    // panes doesn't leave garbage pixels.
    glViewport(0, 0, viewport_width, viewport_height);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const GpuMesh* main_mesh = SelectMainPaneMesh();

    const bool single_pane_mode =
        !g_app.compare_enabled || !g_app.compare_gpu.ready;
    if (single_pane_mode) {
        ViewportRect full{};
        full.x = 0; full.y = 0;
        full.width = viewport_width; full.height = viewport_height;
        DrawMeshPane(full, *main_mesh, /*draw_feature_overlays=*/true);
        return;
    }

    // Split mode: left = main mesh (with overlays), right = compare mesh.
    constexpr int kGapPixels = 1;
    const int half_width = viewport_width / 2;

    ViewportRect left{};
    left.x = 0; left.y = 0;
    left.width  = half_width - kGapPixels;
    left.height = viewport_height;
    DrawMeshPane(left, *main_mesh, /*draw_feature_overlays=*/true);

    ViewportRect right{};
    right.x = half_width + kGapPixels; right.y = 0;
    right.width  = viewport_width - half_width - kGapPixels;
    right.height = viewport_height;
    DrawMeshPane(right, g_app.compare_gpu, /*draw_feature_overlays=*/false);
}

// Each DrawXxxSection function is the body of one collapsing header in the
// side panel. DrawControlPanel (below) just begins/ends the ImGui window
// and calls each section in order.

// ---- Mesh section ----------------------------------------------------------
void DrawMeshSection() {
    // Disable Open while a simplify run is happening: the worker holds a
    // copy of the main mesh, so changing it mid-run isn't crashy, just
    // confusing (the result would be associated with the previous mesh).
    const bool can_open_mesh =
        (g_app.simplify_job.state.load() == SimplifyJobState::kIdle);

    if (!can_open_mesh) ImGui::BeginDisabled();
    if (ImGui::Button("Open OBJ...")) {
        const std::string picked = OpenObjFileDialog(g_app.window);
        if (!picked.empty()) {
            CpuMesh next;
            std::string load_error;
            if (LoadObjIntoCpuMesh(picked, next, load_error)) {
                g_app.cpu_mesh = std::move(next);
                UploadCpuMeshToGpu(g_app.cpu_mesh, g_app.gpu_mesh);
                g_app.camera.Frame(g_app.cpu_mesh.bounds);
                g_app.analysis_needs_rebuild = true;

                // Drop any stale simplify result from the previous mesh.
                DestroyGpuMesh(g_app.simplify_output.simplified_gpu);
                g_app.simplify_output = SimplifyOutput{};
                g_app.view_mode = ViewMode::kOriginal;
                g_app.last_error_message.clear();
            } else {
                g_app.last_error_message = "load failed: " + load_error;
            }
        }
    }
    if (!can_open_mesh) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame View")) {
        g_app.camera.Frame(g_app.cpu_mesh.bounds);
    }

    if (g_app.gpu_mesh.ready) {
        const std::string filename =
            std::filesystem::path(g_app.cpu_mesh.source_path).filename().string();
        ImGui::Text("File: %s",       filename.c_str());
        ImGui::Text("Triangles: %u",  g_app.cpu_mesh.triangle_count);
        ImGui::Text("Vertices: %u",
                    static_cast<std::uint32_t>(g_app.cpu_mesh.positions.size()));
        if (g_app.cpu_mesh.bounds.valid) {
            ImGui::Text("Bounding diagonal: %.3f", g_app.cpu_mesh.bounds.Diagonal());
        }
    } else {
        ImGui::TextDisabled("(no mesh loaded)");
    }

    // Display-mode radio. Lives in this section because it applies to
    // whichever mesh is currently rendered.
    ImGui::Spacing();
    ImGui::TextUnformatted("Display:");
    ImGui::SameLine();
    int display_mode_index = static_cast<int>(g_app.display_mode);
    ImGui::RadioButton("Solid",     &display_mode_index, 0); ImGui::SameLine();
    ImGui::RadioButton("Wireframe", &display_mode_index, 1); ImGui::SameLine();
    ImGui::RadioButton("Both",      &display_mode_index, 2);
    g_app.display_mode = static_cast<DisplayMode>(display_mode_index);

    if (!g_app.last_error_message.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "%s", g_app.last_error_message.c_str());
    }
}

// ---- Feature Detection section --------------------------------------------
void DrawFeatureDetectionSection() {
    bool params_changed = false;
    params_changed |= ImGui::SliderFloat("Sharp angle (deg)",
                                         &g_app.analysis_params.sharp_edge_angle_degrees,
                                         5.0f, 170.0f, "%.0f deg");
    params_changed |= ImGui::SliderFloat("Curvature top %",
                                         &g_app.analysis_params.curvature_top_percent,
                                         0.0f, 1.0f, "%.2f");
    if (params_changed) g_app.analysis_needs_rebuild = true;

    ImGui::Separator();
    ImGui::Checkbox("Show boundary (red)",     &g_app.analysis_params.show_boundary);
    ImGui::Checkbox("Show sharp (orange)",     &g_app.analysis_params.show_sharp);
    ImGui::Checkbox("Show curvature (yellow)", &g_app.analysis_params.show_curvature);
    ImGui::Separator();

    if (!g_app.analysis.valid) {
        ImGui::TextDisabled("(load a mesh to run analysis)");
        return;
    }
    ImGui::Text("Boundary edges: %u",
                static_cast<std::uint32_t>(g_app.analysis.result.boundary_edges.size()));
    ImGui::Text("Sharp edges:    %u",
                static_cast<std::uint32_t>(g_app.analysis.result.sharp_edges.size()));
    ImGui::Text("Curvature points: %d",
                static_cast<int>(g_app.overlays.curvature_points.vertex_count));
    ImGui::Text("Max curvature factor: %.2f",
                g_app.analysis.result.max_curvature_factor_seen);
}

// ---- Simplification section -----------------------------------------------
void DrawSimplificationSection() {
    ImGui::SliderInt("Target %", &g_app.simplify_params.target_percent, 1, 100);
    ImGui::TextDisabled("(percent of original triangles to keep)");

    ImGui::Spacing();
    ImGui::Checkbox("Preserve boundaries",  &g_app.simplify_params.preserve_boundary);
    ImGui::Checkbox("Preserve sharp edges", &g_app.simplify_params.preserve_sharp);
    ImGui::Checkbox("Preserve curvature",   &g_app.simplify_params.preserve_curvature);

    if (ImGui::TreeNode("Advanced tuning")) {
        ImGui::DragFloat("Boundary weight",
                         &g_app.simplify_params.boundary_weight,
                         10.0f, 1.0f, 100000.0f, "%.0f");
        ImGui::Checkbox("Strict boundary", &g_app.simplify_params.boundary_strict);
        ImGui::DragFloat("Sharp weight",
                         &g_app.simplify_params.sharp_weight,
                         10.0f, 1.0f, 100000.0f, "%.0f");
        ImGui::Checkbox("Strict sharp edges", &g_app.simplify_params.sharp_edge_strict);
        ImGui::DragFloat("Curvature strength",
                         &g_app.simplify_params.curvature_strength,
                         0.1f, 0.0f, 20.0f, "%.2f");
        ImGui::TreePop();
    }

    ImGui::Spacing();

    const SimplifyJobState job_state = g_app.simplify_job.state.load();
    const bool is_running =
        (job_state == SimplifyJobState::kRunning) ||
        (job_state == SimplifyJobState::kReadyToConsume);
    const bool can_run = g_app.gpu_mesh.ready && !is_running;

    if (!can_run) ImGui::BeginDisabled();
    const char* run_label = is_running ? "Simplifying..." : "Run Simplification";
    if (ImGui::Button(run_label, ImVec2(-FLT_MIN, 32.0f))) {
        StartSimplificationAsync();
    }
    if (!can_run) ImGui::EndDisabled();

    if (is_running) {
        // Live elapsed-time readout with a bouncing-dots animation so the
        // user knows the app isn't frozen during multi-second runs.
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_app.simplify_job.start_time).count();
        const int phase = static_cast<int>(elapsed * 3.0) % 4;
        const char* dots[] = {"", ".", "..", "..."};
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f),
                           "Simplifying%s  %.1f s elapsed",
                           dots[phase], elapsed);
    }

    if (g_app.simplify_output.valid) {
        ImGui::Spacing();
        ImGui::TextUnformatted("View:");
        ImGui::SameLine();
        int view_index = static_cast<int>(g_app.view_mode);
        ImGui::RadioButton("Original",   &view_index, 0); ImGui::SameLine();
        ImGui::RadioButton("Simplified", &view_index, 1);
        g_app.view_mode = static_cast<ViewMode>(view_index);
    }
}

// ---- Results section -------------------------------------------------------
void DrawResultsSection() {
    if (!g_app.simplify_output.valid) {
        ImGui::TextDisabled("(run simplification to see results)");
        return;
    }

    const glint_qem::SimplifyStats& stats = g_app.simplify_output.stats;
    const double reduction_percent =
        (stats.input_triangle_count > 0u)
            ? (100.0 * (1.0 - static_cast<double>(stats.output_triangle_count) /
                               static_cast<double>(stats.input_triangle_count)))
            : 0.0;

    ImGui::Text("Triangles: %u -> %u (%.1f%% reduction)",
                stats.input_triangle_count, stats.output_triangle_count,
                reduction_percent);
    ImGui::Text("Vertices:  %u -> %u",
                stats.input_vertex_count, stats.output_vertex_count);
    ImGui::Text("Collapses: %u accepted / %u rejected",
                stats.accepted_collapses, stats.rejected_collapses);
    ImGui::Text("Elapsed: %.3f s", g_app.simplify_output.elapsed_seconds);
    ImGui::Text("Max edge error: %.6f", stats.final_max_edge_error);
    if (!g_app.simplify_output.status_message.empty()) {
        ImGui::TextWrapped("Status: %s",
                           g_app.simplify_output.status_message.c_str());
    }

    ImGui::Spacing();
    if (!ImGui::Button("Export simplified OBJ...")) return;

    // Suggest a filename derived from the original mesh's stem.
    std::string default_name = "simplified.obj";
    if (!g_app.cpu_mesh.source_path.empty()) {
        default_name = std::filesystem::path(g_app.cpu_mesh.source_path)
                           .stem().string() + "_simplified.obj";
    }
    const std::string save_path = SaveObjFileDialog(g_app.window, default_name);
    if (save_path.empty()) return;

    std::string write_error;
    const bool ok = glint_qem_tool::WriteObjMesh(
        CpuMeshToQemMesh(g_app.simplify_output.simplified_cpu),
        save_path, &write_error);
    if (!ok) {
        g_app.last_error_message = "export failed: " + write_error;
    } else {
        g_app.last_error_message.clear();
    }
}

// ---- Compare section -------------------------------------------------------
void DrawCompareSection() {
    const bool compare_was_enabled = g_app.compare_enabled;
    ImGui::Checkbox("Enable split comparison", &g_app.compare_enabled);

    // Auto-fill: if the user just flipped Enable on and there's a simplify
    // result but no B mesh, drop the result into the compare slot so the
    // A/B story works without extra clicks.
    const bool just_enabled =
        g_app.compare_enabled && !compare_was_enabled;
    if (just_enabled && !g_app.compare_gpu.ready && g_app.simplify_output.valid) {
        PopulateCompareFromSimplified();
    }

    ImGui::Spacing();

    // Buttons act on the compare slot regardless of whether the toggle is
    // on, so "load B then flip on" also works naturally.
    const bool simplify_result_exists = g_app.simplify_output.valid;
    if (ImGui::Button("Load B...")) {
        const std::string picked = OpenObjFileDialog(g_app.window);
        if (!picked.empty()) LoadCompareMeshFromPath(picked);
    }
    ImGui::SameLine();
    if (!simplify_result_exists) ImGui::BeginDisabled();
    if (ImGui::Button("Use simplified")) PopulateCompareFromSimplified();
    if (!simplify_result_exists) ImGui::EndDisabled();

    const bool can_swap = g_app.gpu_mesh.ready && g_app.compare_gpu.ready;
    if (!can_swap) ImGui::BeginDisabled();
    if (ImGui::Button("Swap A <-> B")) SwapMainAndCompareMeshes();
    if (!can_swap) ImGui::EndDisabled();
    ImGui::SameLine();
    const bool b_loaded = g_app.compare_gpu.ready;
    if (!b_loaded) ImGui::BeginDisabled();
    if (ImGui::Button("Clear B")) {
        DestroyGpuMesh(g_app.compare_gpu);
        g_app.compare_cpu = CpuMesh{};
        g_app.compare_label.clear();
    }
    if (!b_loaded) ImGui::EndDisabled();

    ImGui::Spacing();

    const std::string a_label = g_app.gpu_mesh.ready
        ? std::filesystem::path(g_app.cpu_mesh.source_path).filename().string()
        : std::string("(none)");
    const std::string b_label = g_app.compare_gpu.ready
        ? (g_app.compare_label.empty() ? std::string("(loaded)") : g_app.compare_label)
        : std::string("(none)");
    ImGui::Text("A (left):  %s", a_label.c_str());
    ImGui::Text("B (right): %s", b_label.c_str());

    if (g_app.compare_enabled && !g_app.compare_gpu.ready) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "Split mode is on but no B mesh is loaded.");
    }
}

void DrawControlPanel(int window_width, int window_height) {
    ImGui::SetNextWindowPos(
        ImVec2(static_cast<float>(window_width) - kPanelWidth, 0.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(kPanelWidth, static_cast<float>(window_height)),
        ImGuiCond_Always);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove   | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    ImGui::Begin("QEM Studio", nullptr, flags);
    ImGui::TextUnformatted("QEM Studio");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawMeshSection();
    }
    if (ImGui::CollapsingHeader("Feature Detection",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawFeatureDetectionSection();
    }
    if (ImGui::CollapsingHeader("Simplification",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawSimplificationSection();
    }
    const ImGuiTreeNodeFlags results_flags =
        g_app.simplify_output.valid ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    if (ImGui::CollapsingHeader("Results", results_flags)) {
        DrawResultsSection();
    }
    if (ImGui::CollapsingHeader("Compare")) {
        DrawCompareSection();
    }

    ImGui::End();
}

void GlfwErrorCallback(int error_code, const char* description) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", error_code, description);
}

GLFWwindow* CreateWindowAndContext() {
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "[glfw] init failed\n");
        return nullptr;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(
        kInitialWindowWidth, kInitialWindowHeight, kWindowTitle, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "[glfw] window creation failed\n");
        glfwTerminate();
        return nullptr;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        std::fprintf(stderr, "[glad] failed to load OpenGL functions\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return nullptr;
    }
    return window;
}

void InitImGui(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
}

void ShutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    glfwSetErrorCallback(GlfwErrorCallback);

    g_app.window = CreateWindowAndContext();
    if (!g_app.window) return EXIT_FAILURE;

    // Install our input callbacks BEFORE initializing the ImGui GLFW
    // backend. The backend records whatever callback is installed at init
    // time and chains to it from its own handler; installing after would
    // clobber the backend's handler and break ImGui input.
    glfwSetMouseButtonCallback(g_app.window, OnMouseButton);
    glfwSetCursorPosCallback  (g_app.window, OnCursorPos);
    glfwSetScrollCallback     (g_app.window, OnScroll);

    InitImGui(g_app.window);

    g_app.mesh_program    = BuildMeshProgram();
    g_app.overlay_program = BuildOverlayProgram();

    while (!glfwWindowShouldClose(g_app.window)) {
        glfwPollEvents();

        // Lazy analysis rebuild: only when something that affects the
        // result changed since last rebuild.
        if (g_app.analysis_needs_rebuild ||
            AnalysisInputsChanged(g_app.analysis_params, g_app.last_applied_params)) {
            RebuildAnalysis();
        }

        MaybeConsumeFinishedSimplification();

        int window_width = 0;
        int window_height = 0;
        glfwGetWindowSize(g_app.window, &window_width, &window_height);

        RenderViewport(window_width, window_height);

        // ImGui frame on top of the viewport.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawControlPanel(window_width, window_height);
        ImGui::Render();

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(g_app.window, &framebuffer_width, &framebuffer_height);
        glViewport(0, 0, framebuffer_width, framebuffer_height);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(g_app.window);
    }

    // Finish any in-flight simplify job before tearing down. std::thread
    // calls terminate() on destruction of a joinable thread.
    if (g_app.simplify_job.worker.joinable()) g_app.simplify_job.worker.join();

    DestroyLineBuffer (g_app.overlays.boundary_lines);
    DestroyLineBuffer (g_app.overlays.sharp_lines);
    DestroyPointBuffer(g_app.overlays.curvature_points);
    DestroyGpuMesh(g_app.simplify_output.simplified_gpu);
    DestroyGpuMesh(g_app.compare_gpu);
    DestroyGpuMesh(g_app.gpu_mesh);
    if (g_app.overlay_program) glDeleteProgram(g_app.overlay_program);
    if (g_app.mesh_program)    glDeleteProgram(g_app.mesh_program);

    ShutdownImGui();
    glfwDestroyWindow(g_app.window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
