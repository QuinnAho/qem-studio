#pragma once

#include <string>

#include "glint_qem/types.h"

namespace glint_qem_tool {

// Loads OBJ positions + triangle indices into qem_core mesh format.
// Requires tinyobjloader header to be available at build time; otherwise returns false with an explanatory error.
bool ReadObjMesh(const std::string& obj_path,
                 glint_qem::IndexedTriangleMesh& out_mesh,
                 std::string* error = nullptr);

} // namespace glint_qem_tool

