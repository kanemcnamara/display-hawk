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
} ubuf;

layout(binding = 1) uniform sampler2D tex;

void main()
{
    vec4 c = ubuf.flags2.x == 1 ? texture(tex, vUV) : ubuf.color;
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
    fragColor = c;
}
