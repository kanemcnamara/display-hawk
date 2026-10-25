#version 440

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 params;  // x opacity, y blend mode (see blend.frag), z 1 = textures are Y-up, w 1 = opaque base
} ubuf;

void main()
{
    // Full-target quad as a 4-vertex triangle strip; vUV has a top-left origin.
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = ubuf.clipCorr * vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    vUV = c;
}
