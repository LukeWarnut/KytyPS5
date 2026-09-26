#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout(binding = 0, set = 0) uniform texture2DMS depth;
layout(push_constant) uniform Push {
	int sample_id;
} push;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
	ivec2 coord = ivec2(gl_FragCoord.xy);
	color       = vec4(texelFetch(depth, coord, push.sample_id).r, 0.0, 0.0, 1.0);
}
