// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "dwrite.hlsl"
#include "shader_common.hlsl"

cbuffer ConstBuffer : register(b0)
{
    float4 positionScale;
    float4 gammaRatios;
    float enhancedContrast;
    float dashedLineLength;
}

Texture2D<float4> glyphAtlas : register(t0);

struct Output
{
    float4 color;
    float4 weights;
};

// clang-format off
Output main(PSData data) : SV_Target
// clang-format on
{
    switch (data.shadingType)
    {
    case SHADING_TYPE_TEXT_GRAYSCALE:
    {
        float4 foreground = premultiplyColor(data.color);
        float4 glyphColor = glyphAtlas[data.texcoord];
        float blendEnhancedContrast = DWrite_ApplyLightOnDarkContrastAdjustment(enhancedContrast, data.color.rgb);
        float intensity = DWrite_CalcColorIntensity(data.color.rgb);
        float contrasted = DWrite_EnhanceContrast(glyphColor.a, blendEnhancedContrast);
        float alphaCorrected = DWrite_ApplyAlphaCorrection(contrasted, intensity, gammaRatios);
        float4 color = alphaCorrected * foreground;

        Output output;
        output.color = color;
        output.weights = color.aaaa;
        return output;
    }
    case SHADING_TYPE_TEXT_CLEARTYPE:
    {
        float4 glyph = glyphAtlas[data.texcoord];
        float blendEnhancedContrast = DWrite_ApplyLightOnDarkContrastAdjustment(enhancedContrast, data.color.rgb);
        float3 contrasted = DWrite_EnhanceContrast3(glyph.rgb, blendEnhancedContrast);
        float3 alphaCorrected = DWrite_ApplyAlphaCorrection3(contrasted, data.color.rgb, gammaRatios);
        float4 weights = float4(alphaCorrected * data.color.a, 1);

        // The final step of the ClearType blending algorithm is a lerp() between the premultiplied alpha
        // background color and straight alpha foreground color given the 3 RGB weights in alphaCorrected:
        //   lerp(background, foreground, weights)
        // Which is equivalent to:
        //   background * (1 - weights) + foreground * weights
        //
        // This COULD be implemented using dual source color blending like so:
        //   .SrcBlend = D3D11_BLEND_SRC1_COLOR
        //   .DestBlend = D3D11_BLEND_INV_SRC1_COLOR
        //   .BlendOp = D3D11_BLEND_OP_ADD
        // Because:
        //   background * (1 - weights) + foreground * weights
        //       ^             ^        ^     ^           ^
        //      Dest     INV_SRC1_COLOR |    Src      SRC1_COLOR
        //                            OP_ADD
        //
        // BUT we need simultaneous support for regular "source over" alpha blending
        // (SHADING_TYPE_PASSTHROUGH)  like this:
        //   background * (1 - alpha) + foreground
        //
        // This is why we set:
        //   .SrcBlend = D3D11_BLEND_ONE
        //
        // --> We need to multiply the foreground with the weights ourselves.
        Output output;
        output.color = weights * data.color;
        output.weights = weights;
        return output;
    }
    case SHADING_TYPE_PASSTHROUGH:
    {
        float4 color = glyphAtlas[data.texcoord];

        Output output;
        output.color = color;
        output.weights = color.aaaa;
        return output;
    }
    case SHADING_TYPE_PASSTHROUGH_INVERT:
    {
        float4 color = glyphAtlas[data.texcoord];
        color.rgb = color.aaa - color.rgb;

        Output output;
        output.color = color;
        output.weights = color.aaaa;
        return output;
    }
    case SHADING_TYPE_DASHED_LINE:
    {
        bool on = frac(data.position.x / dashedLineLength) < 0.333333333f;
        float4 color = on * premultiplyColor(data.color);

        Output output;
        output.color = color;
        output.weights = color.aaaa;
        return output;
    }
    case SHADING_TYPE_SOLID_FILL:
    default:
    {
        float4 color = premultiplyColor(data.color);

        Output output;
        output.color = color;
        output.weights = color.aaaa;
        return output;
    }
    }
}
