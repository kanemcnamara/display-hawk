/*{
    "DESCRIPTION": "Rings of ripples spreading from a point (Display Hawk example)",
    "CREDIT": "Display Hawk",
    "ISFVSN": "2",
    "CATEGORIES": ["Distortion"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "centerX", "LABEL": "Centre X", "TYPE": "float", "DEFAULT": 0.5, "MIN": 0.0, "MAX": 1.0 },
        { "NAME": "centerY", "LABEL": "Centre Y", "TYPE": "float", "DEFAULT": 0.5, "MIN": 0.0, "MAX": 1.0 },
        { "NAME": "amplitude", "LABEL": "Amplitude", "TYPE": "float", "DEFAULT": 0.015, "MIN": 0.0, "MAX": 0.1 },
        { "NAME": "frequency", "LABEL": "Rings", "TYPE": "float", "DEFAULT": 20.0, "MIN": 1.0, "MAX": 80.0 },
        { "NAME": "speed", "LABEL": "Speed", "TYPE": "float", "DEFAULT": 2.0, "MIN": -10.0, "MAX": 10.0 }
    ]
}*/

void main()
{
    vec2 uv = isf_FragNormCoord;
    vec2 aspect = vec2(RENDERSIZE.x / RENDERSIZE.y, 1.0);
    vec2 d = (uv - vec2(centerX, centerY)) * aspect;
    float r = length(d);
    float wave = sin(r * frequency * 6.2831853 - TIME * speed * 6.2831853);
    vec2 offset = r > 0.0001 ? d / r * wave * amplitude / aspect : vec2(0.0);
    gl_FragColor = IMG_NORM_PIXEL(inputImage, uv + offset);
}
