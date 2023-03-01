// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "shader_common.hlsl"

StructuredBuffer<VSData> instances : register(t0);

// clang-format off
PSData main(uint id: SV_VertexID)
// clang-format on
{
    VSData data = instances[id / 4];

    float2 position;
    position.x = (id & 1) ? 1 : 0;
    position.y = (id & 2) ? 1 : 0;
    
    PSData output;
    output.shadingType = data.shadingType;
    output.color = decodeRGBA(data.color);
    // positionScale is expected to be float2(2.0f / sizeInPixel.x, -2.0f / sizeInPixel.y). Together with the
    // addition below this will transform our "position" from pixel into normalized device coordinate (NDC) space.
    output.position = float4((position * data.position.zw + data.position.xy) * positionScale + float2(-1.0f, 1.0f), 0, 1);
    output.texcoord = position * data.texcoord.zw + data.texcoord.xy;
    return output;
}
