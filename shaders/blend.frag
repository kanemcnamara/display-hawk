#version 440

// Mixes a layer onto everything below it with a blend mode the fixed-function blender cannot do.
// base: everything below (premultiplied; opaque in the composition, maybe not in a group);
// layer: the layer, premultiplied. Mode numbers follow
// BlendMode in model/Project.h.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 params;  // x opacity, y blend mode, z 1 = textures are Y-up, w 1 = the base is opaque (composition)
} ubuf;

layout(binding = 1) uniform sampler2D baseTex;
layout(binding = 2) uniform sampler2D layerTex;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 setLum(vec3 c, float l)
{
    c += l - luma(c);
    float lo = min(c.r, min(c.g, c.b)), hi = max(c.r, max(c.g, c.b)), L = luma(c);
    if (lo < 0.0)
        c = L + (c - L) * L / max(L - lo, 1e-5);
    if (hi > 1.0)
        c = L + (c - L) * (1.0 - L) / max(hi - L, 1e-5);
    return c;
}

float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }

vec3 setSat(vec3 c, float s)
{
    float lo = min(c.r, min(c.g, c.b)), hi = max(c.r, max(c.g, c.b));
    return hi > lo ? (c - lo) * s / (hi - lo) : vec3(0.0);
}

float softLight(float b, float s)
{
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return s <= 0.5 ? b - (1.0 - 2.0 * s) * b * (1.0 - b) : b + (2.0 * s - 1.0) * (d - b);
}

void main()
{
    vec2 uv = vUV;
    if (ubuf.params.z > 0.5)
        uv.y = 1.0 - uv.y;
    vec4 B = texture(baseTex, uv);
    vec3 b = B.a > 0.0001 ? B.rgb / B.a : vec3(0.0);
    vec4 L = texture(layerTex, uv);
    float a = L.a * ubuf.params.x;
    vec3 s = L.a > 0.0001 ? L.rgb / L.a : vec3(0.0);
    int mode = int(ubuf.params.y + 0.5);

    vec3 r = s;
    if (mode == 6) r = max(b - s, 0.0);                                   // Subtract
    else if (mode == 7) r = abs(b - s);                                   // Difference
    else if (mode == 8) r = b + s - 2.0 * b * s;                          // Exclusion
    else if (mode == 9)                                                   // Overlay
        r = vec3(b.r < 0.5 ? 2.0 * b.r * s.r : 1.0 - 2.0 * (1.0 - b.r) * (1.0 - s.r),
                 b.g < 0.5 ? 2.0 * b.g * s.g : 1.0 - 2.0 * (1.0 - b.g) * (1.0 - s.g),
                 b.b < 0.5 ? 2.0 * b.b * s.b : 1.0 - 2.0 * (1.0 - b.b) * (1.0 - s.b));
    else if (mode == 10) r = vec3(softLight(b.r, s.r), softLight(b.g, s.g), softLight(b.b, s.b)); // Soft light
    else if (mode == 11)                                                  // Hard light
        r = vec3(s.r < 0.5 ? 2.0 * b.r * s.r : 1.0 - 2.0 * (1.0 - b.r) * (1.0 - s.r),
                 s.g < 0.5 ? 2.0 * b.g * s.g : 1.0 - 2.0 * (1.0 - b.g) * (1.0 - s.g),
                 s.b < 0.5 ? 2.0 * b.b * s.b : 1.0 - 2.0 * (1.0 - b.b) * (1.0 - s.b));
    else if (mode == 12) r = min(b / max(1.0 - s, 1e-4), 1.0);           // Colour dodge
    else if (mode == 13) r = 1.0 - min((1.0 - b) / max(s, 1e-4), 1.0);   // Colour burn
    else if (mode == 14) r = setLum(setSat(s, sat(b)), luma(b));          // Hue
    else if (mode == 15) r = setLum(setSat(b, sat(s)), luma(b));          // Saturation
    else if (mode == 16) r = setLum(s, luma(b));                          // Colour
    else if (mode == 17) r = setLum(b, luma(s));                          // Luminosity

    if (mode >= 18) {
        // Masks: the layer decides how much of everything below shows (opacity fades the mask out).
        float m = mode == 18 ? L.a : (mode == 19 ? luma(s) * L.a : 1.0 - luma(s) * L.a);
        // In the composition a mask darkens (it stays opaque); in a group it cuts the alpha too.
        float f = mix(1.0, m, ubuf.params.x);
        fragColor = ubuf.params.w > 0.5 ? vec4(B.rgb * f, B.a) : B * f;
        return;
    }
    // Where there is nothing below the layer shows as it is; elsewhere the blend result does.
    vec3 mixed = (1.0 - B.a) * s + B.a * clamp(r, 0.0, 1.0);
    fragColor = vec4((1.0 - a) * B.rgb + a * mixed, B.a + a * (1.0 - B.a));
}
