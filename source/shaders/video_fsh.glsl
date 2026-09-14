#version 460

// Colour conversion for decoded frames. Two layouts arrive here: YUV420p from
// the software decoder, and NV12 from the Tegra hardware decoder, whose
// chroma is a single interleaved plane. Both are handled by the same pass —
// only where U and V are read from differs.

layout (location = 0) in vec2 vUv;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D texY;
layout (binding = 1) uniform sampler2D texU;   // NV12: interleaved UV
layout (binding = 2) uniform sampler2D texV;   // NV12: unused, bound to texU

layout (std140, binding = 0) uniform VideoParams
{
    vec4 config;   // x: 1 when chroma is interleaved (NV12)
    vec4 matrixRow0;
    vec4 matrixRow1;
    vec4 matrixRow2;
} p;

void main()
{
    vec3 yuv;
    yuv.x = texture(texY, vUv).r;
    if (p.config.x > 0.5) {
        yuv.yz = texture(texU, vUv).rg;
    } else {
        yuv.y = texture(texU, vUv).r;
        yuv.z = texture(texV, vUv).r;
    }

    vec4 s = vec4(yuv, 1.0);
    outColor = vec4(dot(p.matrixRow0, s), dot(p.matrixRow1, s), dot(p.matrixRow2, s), 1.0);
}
