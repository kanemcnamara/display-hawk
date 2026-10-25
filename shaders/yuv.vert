#version 440

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    mat4 colorMatrix; // YUV (+1) -> RGB, including range expansion
    ivec4 mode;       // x: 0 = semi-planar UV, 1 = planar U,V, 2 = semi-planar VU
} ubuf;

void main()
{
    // Full-target quad as a 4-vertex strip; uv (0,0) is the top-left of the uploaded planes.
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vUV = c;
    gl_Position = ubuf.clipCorr * vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
}
