// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// clang-format off
#define SHADING_TYPE_TEXT_BACKGROUND    0
#define SHADING_TYPE_TEXT_GRAYSCALE     1
#define SHADING_TYPE_TEXT_CLEARTYPE     2
#define SHADING_TYPE_PASSTHROUGH        3
#define SHADING_TYPE_PASSTHROUGH_INVERT 4
#define SHADING_TYPE_DASHED_LINE        5
#define SHADING_TYPE_SOLID_FILL         6
// clang-format on

cbuffer ConstBuffer : register(b0)
{
    float2 positionScale;
    float grayscaleEnhancedContrast;
    float cleartypeEnhancedContrast;
    float4 gammaRatios;
    float dashedLineLength;
}

struct VSData
{
    float4 position : POSITION;
    float4 texcoord : TEXCOORD;
    uint color : COLOR;
    uint shadingType : ShadingType;
    // Structured Buffers are tightly packed. Nvidia recommends padding them to avoid crossing 128-bit
    // cache lines: https://developer.nvidia.com/content/understanding-structured-buffer-performance
    uint2 padding;
};

struct PSData
{
    nointerpolation uint shadingType : ShadingType;
    nointerpolation float4 color : Color;
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
};

float4 premultiplyColor(float4 color)
{
    color.rgb *= color.a;
    return color;
}

float4 alphaBlendPremultiplied(float4 bottom, float4 top)
{
    bottom *= 1 - top.a;
    return bottom + top;
}

float4 decodeRGBA(uint i)
{
    return (i >> uint4(0, 8, 16, 24) & 0xff) / 255.0f;
}
