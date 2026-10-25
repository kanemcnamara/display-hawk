#version 440

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    mat4 colorMatrix;
    ivec4 mode;
} ubuf;

layout(binding = 1) uniform sampler2D texY;
layout(binding = 2) uniform sampler2D texU; // UV (semi-planar) or U (planar)
layout(binding = 3) uniform sampler2D texV; // V (planar only)

void main()
{
    vec3 yuv;
    yuv.x = texture(texY, vUV).r;
    if (ubuf.mode.x == 1) {
        yuv.y = texture(texU, vUV).r;
        yuv.z = texture(texV, vUV).r;
    } else if (ubuf.mode.x == 2) {
        yuv.zy = texture(texU, vUV).rg;
    } else {
        yuv.yz = texture(texU, vUV).rg;
    }
    fragColor = vec4(clamp((ubuf.colorMatrix * vec4(yuv, 1.0)).rgb, 0.0, 1.0), 1.0);
}
