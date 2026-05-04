#pragma once

#include <cstdint>
#include <vector>

namespace glint_qem {

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct IndexedTriangleMesh {
    std::vector<Vec3f> positions;
    std::vector<std::uint32_t> indices; // triangle list: 3 * triangle_count
};

enum class SimplifyStatus : std::uint32_t {
    kSuccess = 0,
    kInvalidInput,
    kNoReductionPossible,
    kStoppedByTarget,
    kStoppedByErrorLimit,
    kStoppedByMaxCollapses,
    kInternalError
};

} // namespace glint_qem
