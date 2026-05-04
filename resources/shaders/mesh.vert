#version 330 core
// Vertex shader for the lit mesh pass. Transforms world-space positions into
// clip space and passes a unit world-space normal to the fragment shader for
// diffuse lighting.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat3 uNormalMatrix;

out vec3 vWorldNormal;
out vec3 vWorldPos;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vWorldNormal = normalize(uNormalMatrix * aNormal);
    gl_Position = uViewProj * world;
}
