#version 440

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    mat4 colorMatrix;
    ivec4 mode; // x: 0 NV12-style UV, 1 planar U/V, 2 NV21-style VU, 3 HAP Q YCoCg, 4 HAP Q + alpha
} ubuf;

layout(binding = 1) uniform sampler2D texY;
layout(binding = 2) uniform sampler2D texU; // UV (semi-planar), U (planar) or BC4 alpha (HAP Q Alpha)
layout(binding = 3) uniform sampler2D texV; // V (planar only)

void main()
{
    int mode = ubuf.mode.x;
    if (mode >= 3) {
        // Scaled YCoCg in DXT5 (HAP Q): Co and Cg in red/green around 0.5, the scale in blue,
        // luma in alpha. Same maths as the reference HAP shader.
        vec4 c = texture(texY, vUV);
        c.xy -= 0.50196078431373;
        float scale = c.z * (255.0 / 8.0) + 1.0;
        float co = c.x / scale;
        float cg = c.y / scale;
        float y = c.w;
        vec3 rgb = vec3(y + co - cg, y + cg, y - co - cg);
        float a = mode == 4 ? texture(texU, vUV).r : 1.0;
        fragColor = vec4(clamp(rgb, 0.0, 1.0), a);
        return;
    }

    vec3 yuv;
    yuv.x = texture(texY, vUV).r;
    if (mode == 1) {
        yuv.y = texture(texU, vUV).r;
        yuv.z = texture(texV, vUV).r;
    } else if (mode == 2) {
        yuv.zy = texture(texU, vUV).rg;
    } else {
        yuv.yz = texture(texU, vUV).rg;
    }
    fragColor = vec4(clamp((ubuf.colorMatrix * vec4(yuv, 1.0)).rgb, 0.0, 1.0), 1.0);
}
