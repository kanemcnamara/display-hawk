#version 440

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;  // QRhi clip-space correction for the active backend
    vec4 size;      // target width, height, time (seconds), 1 = input texture is Y-up
    ivec4 mode;     // effect mode, pass, panel count, panel textures bound
    vec4 p0;        // effect parameters 0..3
    vec4 p1;        // effect parameters 4..7
} ubuf;

void main()
{
    // Full-target quad as a 4-vertex triangle strip; vUV has a top-left origin.
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = ubuf.clipCorr * vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    vUV = c;
}
