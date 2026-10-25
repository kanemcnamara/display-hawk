/*{
    "DESCRIPTION": "Turning rays in two colours (Display Hawk example)",
    "CREDIT": "Display Hawk",
    "ISFVSN": "2",
    "CATEGORIES": ["Generator"],
    "INPUTS": [
        { "NAME": "rays", "LABEL": "Rays", "TYPE": "long", "VALUES": [3, 4, 6, 8, 12, 16], "LABELS": ["3", "4", "6", "8", "12", "16"], "DEFAULT": 8 },
        { "NAME": "speed", "LABEL": "Speed", "TYPE": "float", "DEFAULT": 0.2, "MIN": -2.0, "MAX": 2.0 },
        { "NAME": "twist", "LABEL": "Twist", "TYPE": "float", "DEFAULT": 3.0, "MIN": -10.0, "MAX": 10.0 },
        { "NAME": "colorA", "LABEL": "Colour A", "TYPE": "color", "DEFAULT": [1.0, 0.55, 0.0, 1.0] },
        { "NAME": "colorB", "LABEL": "Colour B", "TYPE": "color", "DEFAULT": [0.05, 0.0, 0.2, 1.0] }
    ]
}*/

void main()
{
    vec2 p = (isf_FragNormCoord - 0.5) * vec2(RENDERSIZE.x / RENDERSIZE.y, 1.0);
    float a = atan(p.y, p.x) / 6.2831853 + length(p) * twist * 0.1 + TIME * speed;
    float v = step(0.5, fract(a * float(rays)));
    gl_FragColor = mix(colorB, colorA, v);
}
