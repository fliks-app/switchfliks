#version 460

// One shader pair drives the whole UI: every primitive is a quad carrying its
// own rounded-rect description, so solid fills, focus rings, artwork and
// glyphs all batch together and never need a pipeline switch.

layout (location = 0) in vec2  inPos;    // pixel space, origin top-left
layout (location = 1) in vec2  inUv;
layout (location = 2) in vec4  inColor;
layout (location = 3) in vec2  inLocal;  // offset from the rect's centre, px
layout (location = 4) in vec4  inShape;  // halfW, halfH, radius, strokeWidth
layout (location = 5) in float inMode;   // 0 solid, 1 rgba texture, 2 alpha mask

layout (std140, binding = 0) uniform Viewport
{
    vec2 invSize; // 2/width, -2/height
} u;

layout (location = 0) out vec2       vUv;
layout (location = 1) out vec4       vColor;
layout (location = 2) out vec2       vLocal;
layout (location = 3) out flat vec4  vShape;
layout (location = 4) out flat float vMode;

void main()
{
    gl_Position = vec4(inPos.x * u.invSize.x - 1.0,
                       inPos.y * u.invSize.y + 1.0,
                       0.0, 1.0);
    vUv    = inUv;
    vColor = inColor;
    vLocal = inLocal;
    vShape = inShape;
    vMode  = inMode;
}
