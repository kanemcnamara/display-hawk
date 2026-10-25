#version 440

layout(location = 0) out vec3 vUV; // (u, v) * q and q: perspective-correct for corner pins
layout(location = 1) out vec3 vLocal; // point of the upright output rect (s, t) * q and q (edge blending)

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;  // QRhi clip-space correction for the active backend
    vec4 dst;       // destination rect in target pixels, top-left origin
    vec4 src;       // normalized source rect, top-left origin
    vec4 color;     // solid colour when useTex == 0
    vec4 params;    // target width, target height, opacity, brightness
    ivec4 flags;    // rotation (quarter turns CW), flipH, flipV, texFlipY
    ivec4 flags2;   // useTex
    vec4 gain;      // per-channel gain (slice colour correction)
    vec4 grade;     // x gamma exponent, y black level, z 1 = correction enabled
    vec4 axes;      // flags2.z == 1: source edge vectors (u, v); src.xy is then the source origin
    vec4 pivot;     // destination turned by z radians (clockwise) about (x, y), in target pixels
    vec4 quadTop;   // flags2.w == 1: corner-pinned destination, top-left and top-right corners
    vec4 quadBottom;// bottom-left and bottom-right corners (target pixels)
    vec4 quadQ;     // projective weights for top-left, top-right, bottom-left, bottom-right
    vec4 blend;       // edge blend widths, fractions of the output: left, right, top, bottom
    vec4 blendParams; // x curve steepness, y 1 / projector gamma, z 1 = blending on
} ubuf;

void main()
{
    // Corner of the destination quad, (0,0) = top-left; drawn as a 4-vertex triangle strip.
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 p = ubuf.dst.xy + c * ubuf.dst.zw;
    float q = 1.0;
    if (ubuf.flags2.w == 1) { // corner pin: the four corners are given
        int i = gl_VertexIndex;
        p = i == 0 ? ubuf.quadTop.xy : (i == 1 ? ubuf.quadTop.zw : (i == 2 ? ubuf.quadBottom.xy : ubuf.quadBottom.zw));
        q = i == 0 ? ubuf.quadQ.x : (i == 1 ? ubuf.quadQ.y : (i == 2 ? ubuf.quadQ.z : ubuf.quadQ.w));
    }
    if (ubuf.pivot.z != 0.0) { // layers fitted into a rotated wall
        float cs = cos(ubuf.pivot.z), sn = sin(ubuf.pivot.z);
        vec2 q = p - ubuf.pivot.xy;
        p = ubuf.pivot.xy + vec2(cs * q.x - sn * q.y, sn * q.x + cs * q.y);
    }
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
    // A rotated source area (slices at an angle) is given as an origin and two edge vectors.
    vec2 uv = ubuf.flags2.z == 1 ? ubuf.src.xy + s.x * ubuf.axes.xy + s.y * ubuf.axes.zw
                                 : ubuf.src.xy + s * ubuf.src.zw;
    if (ubuf.flags.w == 1) uv.y = 1.0 - uv.y;
    vUV = vec3(uv * q, q);
    vLocal = vec3(c * q, q);
}
