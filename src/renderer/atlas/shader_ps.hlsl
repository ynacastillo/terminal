// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "dwrite.hlsl"
#include "shader_common.hlsl"

cbuffer ConstBuffer : register(b0)
{
    float4 invertCursorRect;
    float4 gammaRatios;
    float enhancedContrast;
    float dashedLineLength;
    float2 backgroundBitmapSize;
    float4 backgroundColor;
}

SamplerState backgroundSampler : register(s0);
Texture2D<float4> background : register(t0);
Texture2D<float4> glyphAtlas : register(t1);

struct Output
{
    float4 color;
    float4 weights;
};

// clang-format off
Output main(PSData data) : SV_Target
// clang-format on
{
    {
        float4 rect = (data.shadingType & SHADING_TYPE_FLAG_INVERT_CURSOR) ? invertCursorRect : float4(0, 0, 0, 0);
        if (all(data.position.xy >= rect.xy && data.position.xy < rect.zw))
        {
            float4 ip; // integral part
            float4 frac = modf(data.color * (255.0f / 64.0f), ip);
            data.color = (3.0f - ip + frac) * (64.0f / 255.0f);
            data.color = float4(data.color.rgb, 1);
        }
    }

    switch (data.shadingType)
    {
    case SHADING_TYPE_TEXT_BACKGROUND:
    {
        Output output;
        output.color = all(data.texcoord < backgroundBitmapSize) ? background[data.texcoord] : backgroundColor;
        output.weights = float4(1, 1, 1, 1);
        return output;
    }
    case SHADING_TYPE_TEXT_GRAYSCALE:
    {
        // These are independent of the glyph texture and could be moved to the vertex shader or CPU side of things.
        float4 foreground = premultiplyColor(data.color);
        float blendEnhancedContrast = DWrite_ApplyLightOnDarkContrastAdjustment(enhancedContrast, data.color.rgb);
        float intensity = DWrite_CalcColorIntensity(data.color.rgb);
        // These aren't.
        float4 glyph = glyphAtlas[data.texcoord];
        float contrasted = DWrite_EnhanceContrast(glyph.a, blendEnhancedContrast);
        float alphaCorrected = DWrite_ApplyAlphaCorrection(contrasted, intensity, gammaRatios);

        Output output;
        output.color = alphaCorrected * foreground;
        output.weights = output.color.aaaa;
        return output;
    }
    case SHADING_TYPE_TEXT_CLEARTYPE:
    {
        // These are independent of the glyph texture and could be moved to the vertex shader or CPU side of things.
        float blendEnhancedContrast = DWrite_ApplyLightOnDarkContrastAdjustment(enhancedContrast, data.color.rgb);
        // These aren't.
        float4 glyph = glyphAtlas[data.texcoord];
        float3 contrasted = DWrite_EnhanceContrast3(glyph.rgb, blendEnhancedContrast);
        float3 alphaCorrected = DWrite_ApplyAlphaCorrection3(contrasted, data.color.rgb, gammaRatios);

        Output output;
        output.weights = float4(alphaCorrected * data.color.a, 1);
        output.color = output.weights * data.color;
        return output;
    }
    case SHADING_TYPE_TEXT_COLOR:
    {
        Output output;
        output.color = glyphAtlas[data.texcoord];
        output.weights = output.color.aaaa;
        return output;
    }
    case SHADING_TYPE_DASHED_LINE:
    {
        bool on = frac(data.position.x / dashedLineLength) < 0.333333333f;

        Output output;
        output.color = on * premultiplyColor(data.color);
        output.weights = output.color.aaaa;
        return output;
    }
    default:
    {
        Output output;
        output.color = premultiplyColor(data.color);
        output.weights = output.color.aaaa;
        return output;
    }
    }
}
