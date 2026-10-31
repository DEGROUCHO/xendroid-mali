// Frame generation shared declarations.
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D xe_fg_tex_a;
layout(set = 0, binding = 1) uniform texture2D xe_fg_tex_b;
layout(set = 0, binding = 2, rgb10_a2) uniform writeonly image2D xe_fg_out;
layout(set = 0, binding = 3, std430) buffer XeFgData { float xe_fg_data[]; };
layout(set = 0, binding = 4) uniform sampler xe_fg_sampler;

float XeFgLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
