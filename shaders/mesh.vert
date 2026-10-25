#version 440

// Mesh-warped slices: the slice is drawn as a finely tessellated triangle list. Each vertex carries
// its screen position and its point (s, t) of the upright output rect; everything else (source
// mapping, flips, rotation, colour, blending) is as in quad.vert / quad.frag.

layout(location = 0) in vec4 vertexIn; // xy target pixels, zw output-rect point (s, t)

layout(location = 0) out vec3 vUV;
layout(location = 1) out vec3 vLocal;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 dst;
    vec4 src;
    vec4 color;
    vec4 params;
    ivec4 flags;
    ivec4 flags2;
    vec4 gain;
    vec4 grade;
    vec4 axes;
    vec4 pivot;
    vec4 quadTop;
    vec4 quadBottom;
    vec4 quadQ;
    vec4 blend;
    vec4 blendParams;
} ubuf;

void main()
{
    vec2 p = vertexIn.xy;
    vec2 ndc = vec2(p.x / ubuf.params.x * 2.0 - 1.0, 1.0 - p.y / ubuf.params.y * 2.0);
    gl_Position = ubuf.clipCorr * vec4(ndc, 0.0, 1.0);

    vec2 c = vertexIn.zw;
    vec2 s = c;
    if (ubuf.flags.y == 1) s.x = 1.0 - s.x;
    if (ubuf.flags.z == 1) s.y = 1.0 - s.y;
    int rot = ubuf.flags.x;
    if (rot == 1) s = vec2(s.y, 1.0 - s.x);
    else if (rot == 2) s = vec2(1.0 - s.x, 1.0 - s.y);
    else if (rot == 3) s = vec2(1.0 - s.y, s.x);
    vec2 uv = ubuf.flags2.z == 1 ? ubuf.src.xy + s.x * ubuf.axes.xy + s.y * ubuf.axes.zw
                                 : ubuf.src.xy + s * ubuf.src.zw;
    if (ubuf.flags.w == 1) uv.y = 1.0 - uv.y;
    vUV = vec3(uv, 1.0);
    vLocal = vec3(c, 1.0);
}
