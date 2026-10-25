/*{
    "DESCRIPTION": "Tints the picture towards a colour in pulses (Display Hawk example)",
    "CREDIT": "Display Hawk",
    "ISFVSN": "2",
    "CATEGORIES": ["Color Effect"],
    "INPUTS": [
        { "NAME": "inputImage", "TYPE": "image" },
        { "NAME": "tint", "LABEL": "Colour", "TYPE": "color", "DEFAULT": [1.0, 0.2, 0.6, 1.0] },
        { "NAME": "rate", "LABEL": "Pulses per second", "TYPE": "float", "DEFAULT": 2.0, "MIN": 0.0, "MAX": 10.0 },
        { "NAME": "invert", "LABEL": "Invert", "TYPE": "bool", "DEFAULT": false }
    ]
}*/

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    float pulse = 0.5 + 0.5 * sin(TIME * rate * 6.2831853);
    vec3 rgb = invert ? 1.0 - c.rgb : c.rgb;
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    gl_FragColor = vec4(mix(rgb, tint.rgb * luma * 1.5, pulse * tint.a), c.a);
}
