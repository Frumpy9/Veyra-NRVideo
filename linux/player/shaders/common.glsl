layout(push_constant) uniform Parameters {
    uint width;
    uint height;
    uint tilesX;
    uint tilesY;
    uint radius;
    float phase;
    float cutThreshold;
    uint reserved;
} p;
layout(std430, binding = 0) readonly buffer FrameA { uint a[]; };
layout(std430, binding = 1) readonly buffer FrameB { uint b[]; };
layout(std430, binding = 2) buffer Motion { vec4 flow[]; };
layout(std430, binding = 3) writeonly buffer OutputFrame { uint outputPixels[]; };
layout(std430, binding = 4) buffer Statistics { uint errorSum; uint tileCount; } stats;

vec4 pixel(bool second, ivec2 q) {
    q = clamp(q, ivec2(0), ivec2(p.width, p.height) - 1);
    uint index = uint(q.y) * p.width + uint(q.x);
    return unpackUnorm4x8(second ? b[index] : a[index]);
}
float luma(vec3 rgb) { return dot(rgb, vec3(0.2126, 0.7152, 0.0722)); }
