#version 440

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;  // QRhi clip-space correction for the active backend
    vec4 dst;       // destination rect in target pixels, top-left origin
    vec4 src;       // normalized source rect, top-left origin
    vec4 color;     // solid colour when useTex == 0
    vec4 params;    // target width, target height, opacity, brightness
    ivec4 flags;    // rotation (quarter turns CW), flipH, flipV, texFlipY
    ivec4 flags2;   // useTex
} ubuf;

void main()
{
    // Corner of the destination quad, (0,0) = top-left; drawn as a 4-vertex triangle strip.
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 p = ubuf.dst.xy + c * ubuf.dst.zw;
    vec2 ndc = vec2(p.x / ubuf.params.x * 2.0 - 1.0, 1.0 - p.y / ubuf.params.y * 2.0);
    gl_Position = ubuf.clipCorr * vec4(ndc, 0.0, 1.0);

    vec2 s = c;
    if (ubuf.flags.y == 1) s.x = 1.0 - s.x;
    if (ubuf.flags.z == 1) s.y = 1.0 - s.y;
    // Content rotated clockwise: map the destination point back to the source point.
    int rot = ubuf.flags.x;
    if (rot == 1) s = vec2(s.y, 1.0 - s.x);
    else if (rot == 2) s = vec2(1.0 - s.x, 1.0 - s.y);
    else if (rot == 3) s = vec2(1.0 - s.y, s.x);
    vec2 uv = ubuf.src.xy + s * ubuf.src.zw;
    if (ubuf.flags.w == 1) uv.y = 1.0 - uv.y;
    vUV = uv;
}
