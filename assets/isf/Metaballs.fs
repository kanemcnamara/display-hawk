/*{
    "DESCRIPTION": "Soft blobs that drift and merge (Display Hawk example)",
    "CREDIT": "Display Hawk",
    "ISFVSN": "2",
    "CATEGORIES": ["Generator"],
    "INPUTS": [
        { "NAME": "count", "LABEL": "Blobs", "TYPE": "float", "DEFAULT": 6.0, "MIN": 1.0, "MAX": 12.0 },
        { "NAME": "size", "LABEL": "Size", "TYPE": "float", "DEFAULT": 0.12, "MIN": 0.02, "MAX": 0.5 },
        { "NAME": "speed", "LABEL": "Speed", "TYPE": "float", "DEFAULT": 0.4, "MIN": 0.0, "MAX": 3.0 },
        { "NAME": "blobColor", "LABEL": "Colour", "TYPE": "color", "DEFAULT": [0.2, 0.9, 1.0, 1.0] },
        { "NAME": "background", "LABEL": "Background", "TYPE": "color", "DEFAULT": [0.0, 0.0, 0.0, 1.0] }
    ]
}*/

void main()
{
    vec2 aspect = vec2(RENDERSIZE.x / RENDERSIZE.y, 1.0);
    vec2 p = isf_FragNormCoord * aspect;
    float field = 0.0;
    for (int i = 0; i < 12; ++i) {
        if (float(i) >= count)
            break;
        float fi = float(i);
        vec2 c = vec2(0.5 + 0.4 * sin(TIME * speed * (0.7 + fi * 0.13) + fi * 1.7),
                      0.5 + 0.4 * cos(TIME * speed * (0.5 + fi * 0.11) + fi * 2.3)) * aspect;
        float d = length(p - c);
        field += size * size / max(d * d, 0.0001);
    }
    float v = smoothstep(0.9, 1.1, field);
    gl_FragColor = mix(background, blobColor, v);
}
