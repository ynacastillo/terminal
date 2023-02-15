// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// clang-format off
#define SHADING_TYPE_TEXT_GRAYSCALE     0
#define SHADING_TYPE_TEXT_CLEARTYPE     1
#define SHADING_TYPE_PASSTHROUGH        2
#define SHADING_TYPE_PASSTHROUGH_INVERT 3
#define SHADING_TYPE_DASHED_LINE        4
#define SHADING_TYPE_SOLID_FILL         5
// clang-format on

struct VSData
{
    float2 position : SV_Position;
    float4 rect : Rect;
    float4 tex : Tex;
    float4 color : Color;
    uint shadingType : ShadingType;
};

struct PSData
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    nointerpolation float4 color : Color;
    nointerpolation uint shadingType : ShadingType;
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
