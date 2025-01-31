#version 450 core

#extension GL_EXT_spirv_intrinsics: enable
#extension GL_ARB_gpu_shader_int64: enable

spirv_instruction (extensions = ["SPV_KHR_shader_clock"], capabilities = [5055], id = 5056)
uvec2 clockRealtime2x32EXT(void);

spirv_instruction (extensions = ["SPV_KHR_shader_clock"], capabilities = [5055], id = 5056)
int64_t clockRealtimeEXT(void);

spirv_instruction (extensions = ["SPV_AMD_shader_trinary_minmax"], set = "SPV_AMD_shader_trinary_minmax", id = 1)
vec2 min3(vec2 x, vec2 y, vec2 z);

layout(location = 0) in vec3 vec3In;

layout(location = 0) out uvec2 uvec2Out;
layout(location = 1) out int64_t i64Out;
layout(location = 2) out vec2 vec2Out;

void main()
{
    uvec2Out = clockRealtime2x32EXT();
    i64Out = clockRealtimeEXT();
    vec2Out = min3(vec3In.xy, vec3In.yz, vec3In.zx); 
}
