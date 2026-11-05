// Shared by the Vulkan accelerator's compute shaders (spec 18.5). Buffers
// are reached by device address (Vulkan 1.2), passed in push constants as
// uvec2, so no shader needs 64-bit integers. A sort key is a uvec2 (low
// word, high word): the bytes of a little-endian uint64.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(local_size_x = 256) in;

layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32 { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32 { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U64 { uvec2 v[]; };

// Measures, as CDB_VEC_* in cdb_vec.h; element types as cdb_vector_elem.
const uint COSINE = 1u, DOT = 2u, EUCLIDEAN = 3u;
const uint F32_ELEM = 0u, F16_ELEM = 1u, BF16_ELEM = 2u, I8_ELEM = 3u;

// The selection's state per query: 8 words.
const uint S_PREFIX = 0u, S_MASK = 2u, S_NEED = 4u, S_DONE = 5u, S_COUNT = 6u;

// The invocations of a grid-stride loop over x.
uint stride_x()
{
    return gl_NumWorkGroups.x * gl_WorkGroupSize.x;
}

// Keys compare high word first.
bool key_less(uvec2 a, uvec2 b)
{
    return a.y < b.y || (a.y == b.y && a.x < b.x);
}
