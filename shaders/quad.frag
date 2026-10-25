#version 440

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 dst;
    vec4 src;
    vec4 color;
    vec4 params;
    ivec4 flags;
    ivec4 flags2;
} ubuf;

layout(binding = 1) uniform sampler2D tex;

void main()
{
    vec4 c = ubuf.flags2.x == 1 ? texture(tex, vUV) : ubuf.color;
    c.rgb *= ubuf.params.w;
    c.a *= ubuf.params.z;
    fragColor = c;
}
