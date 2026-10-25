#version 440

// Procedural generators (render/Generators.cpp). Each mode computes a coverage f in 0..1 and the
// output is the back colour mixed towards the front colour by f (straight alpha).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 clipCorr;
    vec4 size;
    ivec4 mode;
    vec4 p0;
    vec4 p1;
    vec4 color1;
    vec4 color2;
} ubuf;

const float PI = 3.14159265;

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec2 hash22(vec2 p)
{
    float n = hash21(p);
    return vec2(n, hash21(p + n + 17.0));
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), u.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}

vec2 rotate(vec2 v, float degrees)
{
    float a = radians(degrees);
    return vec2(cos(a) * v.x - sin(a) * v.y, sin(a) * v.x + cos(a) * v.y);
}

// Signed distance-ish shape value: < 0 inside. q is centred, radius r.
float shapeDist(int shape, vec2 q, float r)
{
    if (shape == 0)
        return length(q) - r;                                     // circle
    if (shape == 1)
        return max(abs(q.x), abs(q.y)) - r;                       // square
    if (shape == 2) {                                             // triangle
        q.y += r * 0.25;
        return max(abs(q.x) * 0.866 + q.y * 0.5, -q.y) - r * 0.5;
    }
    if (shape == 3) {                                             // star (5 points)
        float a = atan(q.x, q.y) + PI;
        float seg = 2.0 * PI / 5.0;
        float k = abs(mod(a, seg) - seg * 0.5) / (seg * 0.5);
        return length(q) - r * mix(0.45, 1.0, k * k);
    }
    if (shape == 4)
        return abs(length(q) - r * 0.8) - r * 0.2;                // ring
    vec2 h = abs(q);                                              // hexagon
    return max(h.x * 0.866 + h.y * 0.5, h.y) - r;
}

void main()
{
    vec2 sz = ubuf.size.xy;
    float t = ubuf.size.z;
    float aspect = sz.x / max(sz.y, 1.0);
    vec2 uv = vUV;
    vec2 c = (uv - 0.5) * vec2(aspect, 1.0); // centred, square units (height = 1)
    vec4 p0 = ubuf.p0, p1 = ubuf.p1;
    int mode = ubuf.mode.x;
    float f = 0.0;

    if (mode == 0) { // shapes, optionally repeated in a grid
        // One shape centred, or a grid of n rows of square cells.
        float n = max(1.0, floor(p0.w + 0.5));
        vec2 q = c * n;
        if (n > 1.0)
            q = fract(q + 0.5) - 0.5;
        q = rotate(q, p0.z + t * p1.x * 360.0);
        float pulse = 1.0 + p1.z * sin(t * p1.w * 2.0 * PI);
        float r = 0.5 * p0.y * pulse;
        float d = shapeDist(int(p0.x + 0.5), q, r);
        float soft = max(p1.y * 0.5, 0.001);
        f = 1.0 - smoothstep(-soft * 0.5, soft * 0.5, d);
    } else if (mode == 1) { // stripes
        vec2 q = rotate(c, -p0.x);
        float v = fract(q.x * p0.y / aspect + t * p0.w);
        float w = p0.z, soft = max(p1.x * 0.5, 0.001);
        f = smoothstep(0.0, soft, v) * (1.0 - smoothstep(w, w + soft, v));
    } else if (mode == 2) { // gradients
        int type = int(p0.x + 0.5);
        vec2 q = rotate(c, -p0.y);
        float v = type == 0 ? q.x / aspect + 0.5
                : type == 1 ? length(c) * 1.4
                : type == 2 ? atan(q.y, q.x) / (2.0 * PI) + 0.5
                            : (abs(q.x) + abs(q.y)) * 1.2;
        v = v * p0.w + t * p0.z;
        f = p1.x > 0.5 ? 1.0 - abs(fract(v) * 2.0 - 1.0) : fract(v);
    } else if (mode == 3) { // plasma
        vec2 q = c * p0.x;
        float tt = t * p0.y;
        float v = sin(q.x + tt) + sin(q.y * 1.3 - tt * 1.1) + sin((q.x + q.y) * 0.7 + tt * 0.6);
        if (p0.z > 1.5) v += sin(length(q + vec2(sin(tt * 0.3), cos(tt * 0.4)) * 2.0) * 1.5);
        if (p0.z > 2.5) v += sin(q.x * 2.1 - q.y * 1.7 + tt * 1.7) * 0.5;
        if (p0.z > 3.5) v += sin(length(q) * 3.0 - tt * 2.0) * 0.5;
        f = 0.5 + 0.5 * sin(v * PI * 0.5);
    } else if (mode == 4) { // fractal value noise
        vec2 q = c * p0.x + vec2(t * p0.y, t * p0.y * 0.7);
        float sum = 0.0, amp = 0.5, norm = 0.0;
        int octaves = int(p0.z + 0.5);
        for (int i = 0; i < 6; ++i) {
            if (i < octaves) {
                sum += valueNoise(q) * amp;
                norm += amp;
                q = q * 2.03 + vec2(13.7, 7.1);
                amp *= 0.5;
            }
        }
        f = clamp((sum / norm - 0.5) * p0.w + 0.5, 0.0, 1.0);
    } else if (mode == 5) { // particles: one per grid cell, drifting, wrapping
        float n = p0.x;
        int dir = int(p0.w + 0.5);
        vec2 drift = dir == 0 ? vec2(0, -1) : dir == 1 ? vec2(0, 1) : dir == 2 ? vec2(-1, 0) : vec2(1, 0);
        vec2 grid = uv * vec2(n * aspect, n);
        if (dir == 4) { // out from the centre: polar grid
            float a = atan(c.y, c.x) / (2.0 * PI) + 0.5;
            grid = vec2(a * n * 2.0, length(c) * n - t * p0.z * n * 0.25);
        } else {
            grid += drift * t * p0.z * n * 0.25;
        }
        vec2 id = floor(grid);
        float best = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec2 cid = id + vec2(x, y);
                vec2 pos = cid + 0.2 + 0.6 * hash22(cid);
                float d = length(grid - pos);
                float size = p0.y * (0.5 + hash21(cid + 3.1));
                float glow = 1.0 - smoothstep(size * (1.0 - p1.y), size, d);
                float tw = 1.0 - p1.x * (0.5 + 0.5 * sin(t * (2.0 + 4.0 * hash21(cid + 9.7)) + hash21(cid) * 6.28));
                best = max(best, glow * tw);
            }
        }
        f = best;
    } else if (mode == 6) { // starfield: layers of stars rushing towards the viewer
        float layers = floor(p0.y + 0.5);
        for (int i = 0; i < 8; ++i) {
            if (float(i) < layers) {
                float depth = fract(float(i) / layers + t * p0.x * 0.1);
                float scale = mix(20.0, 0.5, depth);
                vec2 q = c * scale + vec2(float(i) * 17.3, float(i) * 9.1);
                vec2 id = floor(q);
                vec2 pos = id + hash22(id);
                float d = length(q - pos);
                float star = (1.0 - smoothstep(0.0, 0.05 * p0.z, d)) * smoothstep(0.0, 0.3, depth) * (1.0 - smoothstep(0.85, 1.0, depth));
                f = max(f, star * step(0.7, hash21(id + float(i))));
            }
        }
    } else if (mode == 7) { // tunnel
        float r = p1.x > 0.5 ? max(abs(c.x), abs(c.y)) : length(c);
        float a = atan(c.y, c.x) / (2.0 * PI);
        float z = 0.25 / max(r, 0.001);
        float v = z * p0.y * 0.25 + t * p0.x + a * floor(p0.z + 0.5); // whole turns: no seam
        float soft = max(p1.y * 0.5, 0.001);
        f = smoothstep(0.5 - soft, 0.5, fract(v)) * (1.0 - smoothstep(1.0 - soft, 1.0, fract(v)));
        f = mix(f, 1.0 - f, step(0.5, fract(a * 8.0 + z * 0.1))) * smoothstep(0.0, 0.08, r);
    } else if (mode == 8) { // radar sweep with rings
        float a = fract(atan(c.y, c.x) / (2.0 * PI) - t * p0.x);
        float trail = clamp(1.0 - a / max(p0.y, 0.001), 0.0, 1.0);
        float rings = p0.z > 0.5 ? smoothstep(0.46, 0.49, abs(fract(length(c) * p0.z * 2.0) - 0.5)) : 0.0;
        f = max(trail * trail, rings * 0.6) * (1.0 - smoothstep(0.48, 0.5, length(c)));
    } else if (mode == 9) { // moving checker
        vec2 q = rotate(c, -p0.z) * p0.x + vec2(t * p0.y, 0.0);
        f = mod(floor(q.x) + floor(q.y), 2.0);
    }
    fragColor = mix(ubuf.color2, ubuf.color1, clamp(f, 0.0, 1.0));
}
