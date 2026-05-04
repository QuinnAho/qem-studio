#version 330 core
// Fragment shader for the lit mesh pass. Single directional light plus a
// constant ambient term. The uniforms are set per-frame from the CPU.

in vec3 vWorldNormal;
in vec3 vWorldPos;

uniform vec3 uBaseColor;
uniform vec3 uLightDir;   // world-space direction FROM surface TO light
uniform vec3 uAmbient;

out vec4 FragColor;

void main() {
    vec3 n = normalize(vWorldNormal);
    float lambert = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 lit = uAmbient + uBaseColor * lambert;
    FragColor = vec4(lit, 1.0);
}
