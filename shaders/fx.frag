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
// panelTable holds three texels per panel, stacked: (order left-right, order top-bottom, order by
// number, random), (centre x, centre y, width, height) in composition pixels, and (cos, sin) of the
// wall's clockwise rotation.
layout(binding = 2) uniform sampler2D panelId;
layout(binding = 3) uniform sampler2D panelTable;
// Trails: this effect's own output from the previous frame (same size, rendered).
layout(binding = 4) uniform sampler2D history;

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
    return texelFetch(panelTable, ivec2(i - (i / w) * w, (i / w) * 3 + row), 0);
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
            // Distance to the nearest edge, in the panel's own (unrotated) frame.
            vec4 box = panelInfo(id, 1);
            vec2 cs = panelInfo(id, 2).xy;
            vec2 q = px - box.xy;
            vec2 local = vec2(cs.x * q.x + cs.y * q.y, -cs.y * q.x + cs.x * q.y);
            vec2 edge = box.zw * 0.5 - abs(local);
            float d = min(edge.x, edge.y);
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
    } else if (mode == 18) { // Wave: sideways (or up-down) sine displacement, moving
        bool vertical = p0.w > 0.5;
        float along = vertical ? px.x : px.y;
        float shift = p0.x * sin(2.0 * PI * (along / max(p0.y, 1.0) - time * p0.z));
        o = src(uv + (vertical ? vec2(0.0, shift / sz.y) : vec2(shift / sz.x, 0.0)));
    } else if (mode == 19) { // Twirl: rotation that fades out towards the radius
        vec2 centre = p0.zw * sz;
        vec2 d = px - centre;
        float radius = max(p0.y, 0.001) * min(sz.x, sz.y);
        float k = clamp(1.0 - length(d) / radius, 0.0, 1.0);
        float a = radians(p0.x) * k * k;
        d = vec2(cos(a) * d.x - sin(a) * d.y, sin(a) * d.x + cos(a) * d.y);
        o = src((centre + d) / sz);
    } else if (mode == 20) { // Vignette
        vec2 q = (uv - 0.5) * vec2(sz.x / sz.y, 1.0);
        float v = smoothstep(p0.y, p0.y - max(p0.z, 0.001), length(q));
        o = vec4(c.rgb * mix(1.0, v, p0.x), c.a);
    } else if (mode == 21) { // Glow: bright parts spread out and added back
        vec3 glow = vec3(0.0);
        float radius = p0.y;
        for (int i = 0; i < 24; ++i) {
            float r = sqrt((float(i) + 0.5) / 24.0) * radius;
            float a = float(i) * 2.39996;
            vec4 s = src(uv + vec2(cos(a), sin(a)) * r / sz);
            vec3 rgb = unpremul(s);
            glow += rgb * s.a * max(luma(rgb) - p0.x, 0.0) / max(1.0 - p0.x, 0.001);
        }
        glow /= 24.0;
        o = vec4(min(c.rgb + glow * p0.z, 1.0), max(c.a, min(1.0, luma(glow) * p0.z)));
    } else if (mode == 22) { // Threshold: black and white
        float l = luma(unpremul(c));
        float soft = max(p0.y, 0.0001);
        float v = smoothstep(p0.x - soft, p0.x + soft, l);
        o = vec4(vec3(v) * c.a, c.a);
    } else if (mode == 23) { // Duotone: shadows to one colour, highlights to another
        vec3 rgb = unpremul(c);
        vec3 tone = mix(p0.xyz, vec3(p0.w, p1.x, p1.y), luma(rgb));
        o = vec4(mix(rgb, tone, p1.z) * c.a, c.a);
    } else if (mode == 24) { // Grain
        vec2 cell = floor(px / max(p0.y, 1.0));
        float seed = p0.z > 0.5 ? floor(time * 30.0) : 0.0;
        float n = fract(sin(dot(cell + seed * 1.37, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
        o = vec4(clamp(unpremul(c) + n * p0.x, 0.0, 1.0) * c.a, c.a);
    } else if (mode == 25) { // Scanlines
        float lines = max(p0.x, 1.0);
        float v = fract(uv.y * lines + time * p0.w);
        float dark = step(1.0 - p0.y, v) * p0.z;
        o = vec4(c.rgb * (1.0 - dark), c.a);
    } else if (mode == 26) { // Sharpen (unsharp mask)
        vec2 t = 1.0 / sz;
        vec4 blur = (src(uv + vec2(t.x, 0.0)) + src(uv - vec2(t.x, 0.0)) + src(uv + vec2(0.0, t.y)) + src(uv - vec2(0.0, t.y))) * 0.25;
        o = vec4(clamp(c.rgb + (c.rgb - blur.rgb) * p0.x, 0.0, c.a), c.a);
    } else if (mode == 27) { // Zoom blur towards a centre
        vec2 centre = p0.yz;
        vec4 sum = vec4(0.0);
        for (int i = 0; i < 16; ++i) {
            float k = 1.0 - p0.x * float(i) / 15.0 * 0.5;
            sum += src(centre + (uv - centre) * k);
        }
        o = sum / 16.0;
    } else if (mode == 28) { // Trails: the previous output fades behind the new picture
        vec2 h = uv;
        if (ubuf.size.w > 0.5)
            h.y = 1.0 - h.y;
        vec4 prev = texture(history, h) * p0.x;
        o = p0.y > 0.5 ? max(c, prev) : c + prev * (1.0 - c.a);
    } else if (mode == 29) { // Hue cycle
        vec3 rgb = hueRotate(unpremul(c), time * p0.x * 360.0);
        o = vec4(clamp(rgb, 0.0, 1.0) * c.a, c.a);
    } else if (mode == 30) { // Hexagon mosaic
        float s = max(p0.x, 2.0);
        vec2 r = vec2(1.0, 1.7320508);
        vec2 h = r * 0.5;
        vec2 a = mod(px / s, r) - h;
        vec2 b = mod(px / s - h, r) - h;
        vec2 g = dot(a, a) < dot(b, b) ? a : b;
        o = src((px - g * s) / sz);
    } else if (mode == 31) { // Bulge / pinch
        vec2 centre = p0.zw * sz;
        vec2 d = px - centre;
        float radius = max(p0.y, 0.001) * min(sz.x, sz.y);
        float r = length(d) / radius;
        if (r < 1.0 && r > 0.0001) // inside the radius, distances are remapped: > 0 magnifies, < 0 pinches
            d = d / r * pow(r, max(0.05, 1.0 + p0.x));
        o = src((centre + d) / sz);
    } else if (mode == 32) { // Scroll with wrap-around
        o = src(fract(uv - vec2(p0.x, -p0.y) * time));
    }
    fragColor = o;
}
