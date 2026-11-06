// ComputeCommon.glsl -- what a compute shader (.csh, GDIDispatchCompute)
// includes instead of Common.glsl, whose blocks claim bindings 0 and 1.
//
// A dispatch's bindings (GDIDefinitions.h): its parameters as a std140
// uniform block at binding 0, then its storage buffers, std430, at bindings
// 1, 2, ... in the order the dispatch passes them, and its one texture, if it
// passes one, at binding 8.
//
//   COMPUTE_PARAMS_BEGIN(MyParams)
//       vec4 Something;
//   COMPUTE_PARAMS_END(MyParams)
//   STORAGE_BUFFER(1, MyData) { vec4 Items[]; };
//   COMPUTE_TEXTURE(MyTexture);

#if WITH_VULKAN
#define COMPUTE_PARAMS_BEGIN(BlockName) layout(std140, set=0, binding=0) uniform BlockName {
#define STORAGE_BUFFER(slot, BlockName) layout(std430, set=0, binding=slot) buffer BlockName
#define COMPUTE_TEXTURE(Name) layout(set=0, binding=8) uniform sampler2D Name
#else
#define COMPUTE_PARAMS_BEGIN(BlockName) layout(std140, binding=0) uniform BlockName {
#define STORAGE_BUFFER(slot, BlockName) layout(std430, binding=slot) buffer BlockName
#define COMPUTE_TEXTURE(Name) layout(binding=8) uniform sampler2D Name
#endif
#define COMPUTE_PARAMS_END(BlockName) };

// A well-mixed 32-bit hash (PCG), and a float in [0, 1) from it.
uint HashUint(uint InValue)
{
    uint State = InValue * 747796405u + 2891336453u;
    uint Word  = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
    return (Word >> 22u) ^ Word;
}
float HashFloat(inout uint InOutSeed)
{
    InOutSeed = HashUint(InOutSeed);
    return float(InOutSeed >> 8u) * (1.0 / 16777216.0);
}
