#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "glint_qem/types.h"

namespace glint_qem {

struct EpsilonPolicy {
    float area_epsilon = 1e-12f;
    float determinant_epsilon = 1e-10f;
    float position_merge_epsilon = 1e-9f;
    float normal_flip_cos_epsilon = -0.999f;
};

struct DeterminismPolicy {
    bool deterministic = true;
    std::uint64_t stable_seed = 0;
    bool stable_priority_queue_tiebreak = true;
    bool stable_edge_enumeration = true;
    bool stable_vertex_reindex_on_output = true;
};

enum class SimplifyProgressStage : std::uint8_t {
    kUnknown = 0,
    kInitializing,
    kRebuildDerivedState, // normals/quadrics/edge candidate rebuild
    kEvaluateCandidates,  // error evaluation + collapse validation
    kApplyCollapse,
    kFinalizeOutput,
    kComplete
};

struct SimplifyProgressEvent {
    SimplifyProgressStage stage = SimplifyProgressStage::kUnknown;
    std::uint32_t input_triangle_count = 0;
    std::uint32_t current_triangle_count = 0;
    std::uint32_t target_triangle_count = 0;
    bool has_target_triangle_count = false;
    std::uint32_t attempted_collapses = 0;
    std::uint32_t accepted_collapses = 0;
    std::uint32_t rejected_collapses = 0;
    double latest_edge_cost = 0.0;
    double max_edge_cost = 0.0;
    double elapsed_seconds = 0.0;
    bool is_final = false;
};

using SimplifyProgressCallback = void(*)(const SimplifyProgressEvent* event, void* user_data);

struct SimplifyProgressSink {
    SimplifyProgressCallback callback = nullptr;
    void* user_data = nullptr;
    std::uint32_t accepted_collapse_interval = 100u;
    bool emit_initial = true;
    bool emit_final = true;
};

struct SimplifyOptions {
    std::optional<std::uint32_t> target_triangle_count;
    std::optional<float> target_ratio;
    std::optional<double> max_error;
    std::optional<std::uint32_t> max_collapses;
    bool compact_output = true;
    bool emit_collapse_trace = false;
    // Boundary preservation: when true, open edges (incidence == 1) add a heavy
    // constraint plane to their endpoint quadrics, pinning boundaries in place.
    bool preserve_boundary = false;
    float boundary_weight = 1000.0f;
    // Strict boundary preservation: when true (the default), the validator
    // also forbids collapsing boundary edges and merging boundary vertices
    // across interior edges, and pins merged vertices to the boundary so
    // holes cannot close. When false, only the soft quadric penalty above
    // applies -- useful when the caller actually wants small holes to close.
    bool boundary_strict = true;
    // Sharp-edge (crease) preservation: when true, interior edges whose two
    // adjacent face normals diverge by more than sharp_edge_angle_degrees are
    // treated as creases. Each adjacent face plane is added as a penalty to the
    // edge's endpoint quadrics, which pins the merged vertex near the crease.
    bool preserve_sharp_edges = false;
    float sharp_edge_angle_degrees = 60.0f;
    float sharp_edge_weight = 1000.0f;
    // Strict sharp-edge preservation: same idea as boundary_strict but for
    // creases -- forbid collapsing crease edges, forbid braiding two creases
    // together, pin merged vertices to the crease line.
    bool sharp_edge_strict = true;
    // Curvature awareness: when true, each vertex's quadric is scaled up by
    // (1 + curvature_strength * bend_amount), where bend_amount is in [0, 1]
    // and measures how much the incident face normals disagree. Flat regions
    // keep factor = 1 (no change); curved regions become more expensive to
    // collapse, so simplification prefers flat areas first. Set
    // curvature_strength = 0 to disable the boost while leaving the flag on.
    bool preserve_curvature = false;
    float curvature_strength = 2.0f;
    SimplifyProgressSink progress{};
    EpsilonPolicy epsilon{};
    DeterminismPolicy determinism{};
};

struct SimplifyStats {
    std::uint32_t input_vertex_count = 0;
    std::uint32_t input_triangle_count = 0;
    std::uint32_t output_vertex_count = 0;
    std::uint32_t output_triangle_count = 0;
    std::uint32_t attempted_collapses = 0;
    std::uint32_t accepted_collapses = 0;
    std::uint32_t rejected_collapses = 0;
    double final_max_edge_error = 0.0;
    double accumulated_error = 0.0;
};

struct CollapseEvent {
    std::uint32_t step = 0;
    std::uint32_t kept_vertex = 0;
    std::uint32_t removed_vertex = 0;
    Vec3f new_position{};
    double cost = 0.0;
};

struct SimplifyResult {
    SimplifyStatus status = SimplifyStatus::kInternalError;
    SimplifyStats stats{};
    std::vector<std::uint32_t> old_to_new_vertex_map;
    std::vector<CollapseEvent> collapse_trace;
    const char* message = "";
};

const char* ToString(SimplifyStatus status);

SimplifyResult Simplify(const IndexedTriangleMesh& input,
                        const SimplifyOptions& options,
                        IndexedTriangleMesh& output);

SimplifyResult SimplifyInPlace(IndexedTriangleMesh& mesh,
                               const SimplifyOptions& options);

// ---- Feature analysis (non-destructive) ---------------------------------------------------------
//
// AnalyzeFeatures inspects the mesh with the same logic the simplifier uses
// internally and returns the raw "what counts as special" information. Nothing
// is collapsed; this is purely read-only and is meant for UI overlays that
// visualize the regions preserve_boundary / preserve_sharp_edges /
// preserve_curvature will protect.

struct FeatureEdge {
    std::uint32_t vertex_a = 0;
    std::uint32_t vertex_b = 0;
};

struct FeatureAnalysis {
    // Edges on an open border (incidence == 1). preserve_boundary uses these.
    std::vector<FeatureEdge> boundary_edges;

    // Interior edges whose two adjacent face normals disagree by more than
    // options.sharp_edge_angle_degrees. preserve_sharp_edges uses these.
    std::vector<FeatureEdge> sharp_edges;

    // Per-vertex curvature factor, computed the same way preserve_curvature
    // uses it (1.0 = flat, higher = more curved). Size matches input.positions.
    // If the vertex is unreferenced or has < 2 incident faces, factor = 1.0.
    std::vector<float> vertex_curvature_factors;

    // The largest curvature factor observed, handy for normalizing colors in
    // an overlay (so yellow->red maps to [1, max_curvature_factor_seen]).
    float max_curvature_factor_seen = 1.0f;
};

// Run the feature analysis. The same options that drive preservation during
// simplify also drive detection here: sharp_edge_angle_degrees controls the
// crease threshold, curvature_strength controls how aggressively curved regions
// score. The preserve_* booleans are ignored (we always report everything) so
// callers can visualize all three regardless of what will be preserved.
FeatureAnalysis AnalyzeFeatures(const IndexedTriangleMesh& input,
                                const SimplifyOptions& options);

} // namespace glint_qem
