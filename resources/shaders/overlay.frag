#version 330 core
// Flat-color fragment shader shared by wireframe and overlay passes. The
// color is set per draw call by the CPU.

uniform vec3 uColor;

out vec4 FragColor;

void main() {
    FragColor = vec4(uColor, 1.0);
}
