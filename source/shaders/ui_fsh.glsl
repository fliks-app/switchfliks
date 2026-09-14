#version 460

layout (location = 0) in vec2       vUv;
layout (location = 1) in vec4       vColor;
layout (location = 2) in vec2       vLocal;
layout (location = 3) in flat vec4  vShape;
layout (location = 4) in flat float vMode;

layout (binding = 0) uniform sampler2D uTex;

layout (location = 0) out vec4 outColor;

float roundedBox(vec2 p, vec2 half_, float r)
{
    vec2 q = abs(p) - half_ + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main()
{
    vec4 c = vColor;
    if (vMode > 1.5)      c.a *= texture(uTex, vUv).r;
    else if (vMode > 0.5) c   *= texture(uTex, vUv);

    // A zero radius with no stroke is the common case (plain quad) and the SDF
    // would still cost a length(); skip it.
    if (vShape.z > 0.0 || vShape.w > 0.0) {
        float d = roundedBox(vLocal, vShape.xy, vShape.z);
        if (vShape.w > 0.0) d = abs(d + vShape.w * 0.5) - vShape.w * 0.5;
        c.a *= 1.0 - smoothstep(-0.5, 0.5, d);
    }

    if (c.a <= 0.0) discard;
    outColor = c;
}
