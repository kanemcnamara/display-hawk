#version 440

layout(location = 0) in vec2 vUV;
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
} ubuf;

layout(binding = 1) uniform sampler2D tex;

void main()
{
    vec4 c = ubuf.flags2.x == 1 ? texture(tex, vUV) : ubuf.color;
    // A rotated source area may reach past the composition: nothing is there.
    if (ubuf.flags2.z == 1 && (vUV.x < 0.0 || vUV.y < 0.0 || vUV.x > 1.0 || vUV.y > 1.0))
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
    fragColor = c;
}
