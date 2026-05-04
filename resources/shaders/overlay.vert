#version 330 core
// Vertex shader shared by the wireframe pass and the feature-analysis
// overlays (boundary lines, crease lines, curvature points). Applies an
// optional NDC-space depth bias so overlay geometry drawn on top of the mesh
// doesn't z-fight with the underlying surface.

layout(location = 0) in vec3 aPos;

uniform mat4 uViewProj;
uniform float uDepthBias; // subtracted from NDC z; 0.0 means no bias

void main() {
    vec4 clip = uViewProj * vec4(aPos, 1.0);
    // Pull the fragment toward the camera a touch so the overlay draws on top
    // of the surface without the lines sinking into it.
    clip.z -= uDepthBias * clip.w;
    gl_Position = clip;
}
