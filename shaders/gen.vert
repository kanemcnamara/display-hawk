#version 440

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 size;   // target width, height, time (seconds)
    ivec4 mode;  // generator mode
    vec4 p0;     // parameters 0..3
    vec4 p1;     // parameters 4..7
    vec4 color1; // front (straight alpha)
    vec4 color2; // back
} ubuf;

void main()
{
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = ubuf.clipCorr * vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    vUV = c;
}
