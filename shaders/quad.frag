#version 440

layout(location = 0) in vec3 vUV;
layout(location = 1) in vec3 vLocal;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 dst;
    vec4 src;
    vec4 color;
    vec4 params;  // target w, target h, opacity, brightness
    ivec4 flags;
    ivec4 flags2; // useTex, input mode (see below)
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

layout(binding = 1) uniform sampler2D tex;

// Soft-edge ramp for overlapping projectors: an S-curve of steepness `curve` (1 = linear) from the
// edge inwards, then gamma-corrected so two overlapping projectors add up to even light.
float edgeRamp(float distance, float width)
{
    if (width <= 0.0)
        return 1.0;
    float x = clamp(distance / width, 0.0, 1.0);
    float p = ubuf.blendParams.x;
    float g = x < 0.5 ? 0.5 * pow(2.0 * x, p) : 1.0 - 0.5 * pow(2.0 * (1.0 - x), p);
    return pow(g, ubuf.blendParams.y);
}

void main()
{
    vec2 uv = vUV.xy / vUV.z;
    vec4 c = ubuf.flags2.x == 1 ? texture(tex, uv) : ubuf.color;
    // A rotated source area may reach past the composition: nothing is there.
    if (ubuf.flags2.z == 1 && (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0))
        c = vec4(0.0, 0.0, 0.0, 1.0);
    int mode = ubuf.flags2.y;
    if (mode == 0) {
        // Straight alpha (images, patterns, solids): opacity scales alpha only.
        c.rgb *= ubuf.params.w;
        c.a *= ubuf.params.z;
    } else {
        // Premultiplied input (rendered layers): opacity scales everything.
        c *= ubuf.params.z;
        c.rgb *= ubuf.params.w;
        // For a MIN (darken) blend, transparent areas must be white so they leave the destination alone.
        if (mode == 2)
            c.rgb += 1.0 - c.a;
    }
    if (ubuf.grade.z > 0.5) {
        // LED colour correction: channel gains, then a gamma exponent, then lifted black level.
        vec3 rgb = clamp(c.rgb * ubuf.gain.rgb, 0.0, 1.0);
        rgb = pow(rgb, vec3(ubuf.grade.x));
        c.rgb = ubuf.grade.y + (1.0 - ubuf.grade.y) * rgb;
    }
    if (ubuf.blendParams.z > 0.5) {
        vec2 l = vLocal.xy / vLocal.z;
        float f = edgeRamp(l.x, ubuf.blend.x) * edgeRamp(1.0 - l.x, ubuf.blend.y)
                * edgeRamp(l.y, ubuf.blend.z) * edgeRamp(1.0 - l.y, ubuf.blend.w);
        c.rgb *= f;
    }
    fragColor = c;
}
