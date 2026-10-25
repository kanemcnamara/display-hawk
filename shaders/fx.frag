#version 440

// Every effect is one mode of this shader. Input and output are premultiplied RGBA of the same
// size; colour effects work on unpremultiplied colour. Mode numbers match FxMode in Effects.h.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 size;   // target width, height, time (seconds), 1 = input texture is Y-up
    ivec4 mode;  // effect mode, pass, panel count, panel textures bound
    vec4 p0;
    vec4 p1;
} ubuf;

layout(binding = 1) uniform sampler2D tex;
// LED panels: panelId holds (panel index + 1) in RGB at every composition pixel (0 = no panel);
// panelTable holds two texels per panel: (order left-right, order top-bottom, order by number,
// random) and the panel rectangle (x, y, w, h) in composition pixels.
layout(binding = 2) uniform sampler2D panelId;
layout(binding = 3) uniform sampler2D panelTable;

const float PI = 3.14159265;

vec4 src(vec2 uv)
{
    if (ubuf.size.w > 0.5)
        uv.y = 1.0 - uv.y;
    return texture(tex, uv);
}

vec3 unpremul(vec4 c) { return c.a > 0.0001 ? c.rgb / c.a : vec3(0.0); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 hueRotate(vec3 c, float degrees)
{
    // Rotation about the grey axis.
    float a = radians(degrees);
    float cs = cos(a), sn = sin(a);
    vec3 k = vec3(0.57735);
    return c * cs + cross(k, c) * sn + k * dot(k, c) * (1.0 - cs);
}

vec3 hueColour(float h)
{
    return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

float hash(float a, float b)
{
    return fract(sin(a * 12.9898 + b * 78.233) * 43758.5453);
}

int panelAt(vec2 px)
{
    if (ubuf.mode.w == 0)
        return -1;
    ivec2 ip = clamp(ivec2(px), ivec2(0), ivec2(ubuf.size.xy) - 1);
    vec4 v = texelFetch(panelId, ip, 0);
    int id = int(v.r * 255.0 + 0.5) + int(v.g * 255.0 + 0.5) * 256 + int(v.b * 255.0 + 0.5) * 65536;
    return id - 1;
}

vec4 panelInfo(int i, int row)
{
    int w = textureSize(panelTable, 0).x;
    return texelFetch(panelTable, ivec2(i - (i / w) * w, (i / w) * 2 + row), 0);
}

float panelOrder(int i, float which)
{
    vec4 o = panelInfo(i, 0);
    int k = int(which + 0.5);
    return k == 0 ? o.x : (k == 1 ? o.y : (k == 2 ? o.z : o.w));
}

void main()
{
    int mode = ubuf.mode.x;
    vec2 sz = ubuf.size.xy;
    vec2 uv = vUV;
    vec2 px = uv * sz; // pixel centre, top-left origin
    float time = ubuf.size.z;
    vec4 p0 = ubuf.p0;
    vec4 p1 = ubuf.p1;
    vec4 c = src(uv);
    vec4 o = c;

    if (mode == 0) { // Colour: brightness, contrast, saturation, hue
        vec3 rgb = unpremul(c);
        rgb += p0.x;
        rgb = (rgb - 0.5) * p0.y + 0.5;
        rgb = mix(vec3(luma(rgb)), rgb, p0.z);
        rgb = hueRotate(rgb, p0.w);
        o = vec4(clamp(rgb, 0.0, 1.0) * c.a, c.a);
    } else if (mode == 1) { // Tint
        vec3 rgb = unpremul(c);
        vec3 t = luma(rgb) * p0.xyz;
        o = vec4(mix(rgb, t, p0.w) * c.a, c.a);
    } else if (mode == 2) { // Invert
        vec3 rgb = unpremul(c);
        o = vec4(mix(rgb, 1.0 - rgb, p0.x) * c.a, c.a);
    } else if (mode == 3) { // Posterize
        float levels = max(2.0, floor(p0.x + 0.5)) - 1.0;
        vec3 rgb = floor(unpremul(c) * levels + 0.5) / levels;
        o = vec4(rgb * c.a, c.a);
    } else if (mode == 4) { // Transform: position (fractions of the size, +y up), scale, rotation, tile
        vec2 q = px - sz * 0.5 - vec2(p0.x, -p0.y) * sz;
        float a = radians(p0.w);
        q = vec2(cos(a) * q.x + sin(a) * q.y, -sin(a) * q.x + cos(a) * q.y);
        q = q / max(p0.z, 0.001) + sz * 0.5;
        vec2 s = q / sz;
        if (p1.x > 0.5)
            o = src(fract(s));
        else if (s.x < 0.0 || s.y < 0.0 || s.x > 1.0 || s.y > 1.0)
            o = vec4(0.0);
        else
            o = src(s);
    } else if (mode == 5) { // Mirror
        int m = int(p0.x + 0.5);
        vec2 s = uv;
        if (m == 0 || m == 4)
            s.x = s.x > 0.5 ? 1.0 - s.x : s.x;
        if (m == 1)
            s.x = s.x < 0.5 ? 1.0 - s.x : s.x;
        if (m == 2 || m == 4)
            s.y = s.y > 0.5 ? 1.0 - s.y : s.y;
        if (m == 3)
            s.y = s.y < 0.5 ? 1.0 - s.y : s.y;
        o = src(s);
    } else if (mode == 6) { // Kaleidoscope
        vec2 centre = p0.zw * sz;
        vec2 d = px - centre;
        float r = length(d);
        float seg = 2.0 * PI / max(2.0, floor(p0.x + 0.5));
        float rot = radians(p0.y);
        float a = atan(d.y, d.x) - rot;
        a = a - seg * floor(a / seg);
        if (a > seg * 0.5)
            a = seg - a;
        a += rot;
        vec2 s = (centre + r * vec2(cos(a), sin(a))) / sz;
        o = src(clamp(s, vec2(0.0), vec2(1.0)));
    } else if (mode == 7) { // Pixelate
        float b = max(1.0, p0.x);
        o = src((floor(px / b) + 0.5) * b / sz);
    } else if (mode == 8) { // Blur: separable Gaussian, pass 0 horizontal, pass 1 vertical
        float radius = p0.x;
        if (radius >= 0.5) {
            vec2 dir = ubuf.mode.y == 0 ? vec2(1.0 / sz.x, 0.0) : vec2(0.0, 1.0 / sz.y);
            float stepPx = max(1.0, radius / 16.0);
            float sigma = max(radius * 0.5, 0.5);
            vec4 sum = vec4(0.0);
            float wsum = 0.0;
            for (int i = -16; i <= 16; ++i) {
                float x = float(i) * stepPx;
                if (abs(x) <= radius) {
                    float w = exp(-x * x / (2.0 * sigma * sigma));
                    sum += src(uv + dir * x) * w;
                    wsum += w;
                }
            }
            o = sum / wsum;
        }
    } else if (mode == 9) { // RGB split
        float a = radians(p0.y);
        vec2 d = vec2(cos(a), -sin(a)) * p0.x / sz;
        vec4 r = src(uv + d);
        vec4 b = src(uv - d);
        o = vec4(r.r, c.g, b.b, max(c.a, max(r.a, b.a)));
    } else if (mode == 10) { // Edges (Sobel on luma)
        vec2 t = 1.0 / sz;
        float tl = luma(src(uv + t * vec2(-1.0, -1.0)).rgb);
        float tc = luma(src(uv + t * vec2(0.0, -1.0)).rgb);
        float tr = luma(src(uv + t * vec2(1.0, -1.0)).rgb);
        float ml = luma(src(uv + t * vec2(-1.0, 0.0)).rgb);
        float mr = luma(src(uv + t * vec2(1.0, 0.0)).rgb);
        float bl = luma(src(uv + t * vec2(-1.0, 1.0)).rgb);
        float bc = luma(src(uv + t * vec2(0.0, 1.0)).rgb);
        float br = luma(src(uv + t * vec2(1.0, 1.0)).rgb);
        float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
        float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
        float e = clamp(length(vec2(gx, gy)) * p0.x, 0.0, 1.0);
        o = vec4(mix(c.rgb, vec3(e), p0.y), mix(c.a, max(c.a, e), p0.y));
    } else if (mode == 11) { // Strobe
        bool on = p0.x <= 0.0 || fract(time * p0.x) < p0.y;
        if (!on)
            o = p0.z > 0.5 ? vec4(1.0) : vec4(0.0);
    } else if (mode == 12) { // Panel outline
        int id = panelAt(px);
        if (id >= 0) {
            vec4 r = panelInfo(id, 1);
            float d = min(min(px.x - r.x, r.x + r.z - px.x), min(px.y - r.y, r.y + r.w - px.y));
            float line = clamp(p0.x - d + 0.5, 0.0, 1.0) * p1.x;
            o = mix(c, vec4(p0.yzw, 1.0), line);
        }
    } else if (mode == 13) { // Panel chase: a band of lit panels travels through them in order
        int id = panelAt(px);
        float lit = 0.0;
        if (id >= 0) {
            float d = fract(time * p0.y - panelOrder(id, p0.x)); // how far behind the head of the band
            float width = max(p0.z, 0.001);
            lit = 1.0 - smoothstep(min(width * (1.0 - p0.w), width - 0.0001), width, d);
        }
        o = p1.x > 0.5 ? vec4(min(c.rgb + lit, 1.0), max(c.a, lit)) : c * lit;
    } else if (mode == 14) { // Panel twinkle: random panels flash and fade
        int id = panelAt(px);
        float lit = 0.0;
        if (id >= 0) {
            float t = time * p0.x + panelInfo(id, 0).w * 17.0;
            float k = mod(floor(t), 4096.0);
            if (hash(float(id - (id / 4096) * 4096), k) < p0.y)
                lit = max(0.0, 1.0 - fract(t) / max(p0.z, 0.001));
        }
        o = p0.w > 0.5 ? vec4(min(c.rgb + lit, 1.0), max(c.a, lit)) : c * lit;
    } else if (mode == 15) { // Panel colours: a hue per panel along an order
        int id = panelAt(px);
        if (id >= 0) {
            vec3 h = hueColour(fract(panelOrder(id, p0.x) * p0.y + time * p0.z));
            o = mix(c, vec4(h, 1.0), p0.w);
        }
    } else if (mode == 16) { // Colour key: pixels near the key colour become transparent
        vec3 rgb = unpremul(c);
        float dist = distance(rgb, p0.xyz) / 1.7320508;
        float a = smoothstep(p0.w, p0.w + max(p1.x, 0.0001), dist);
        // Spill removal: treat edge pixels as a mix of the foreground and the key colour.
        vec3 fg = clamp((rgb - (1.0 - a) * p0.xyz) / max(a, 0.05), 0.0, 1.0);
        rgb = mix(rgb, fg, p1.y);
        float alpha = c.a * a;
        o = vec4(rgb * alpha, alpha);
    } else if (mode == 17) { // Luma key: Dark removes below the threshold, Bright above it
        float l = luma(unpremul(c));
        float soft = max(p0.y, 0.0001);
        float a = p0.z > 0.5 ? 1.0 - smoothstep(p0.x - soft, p0.x, l) : smoothstep(p0.x, p0.x + soft, l);
        o = c * a;
    }
    fragColor = o;
}
