// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

struct CSSLengthPercentage
{
    enum class ReferenceFrame : uint8_t
    {
        // This indicates the object is empty/unset. _value will be 0.
        None,
        // Inputs with a "pt" or "px" suffix are considered "absolute".
        // _value contains an absolute size in CSS inches. In other words,
        // an input of "96px" or "72pt" results in a _value of 1.
        Absolute,
        // Inputs with no suffix or "%" are considered font-size dependent.
        // _value should be multiplied by the current font-size to get the new font-size.
        FontSize,
        // Inputs with a "ch" suffix are considered advance-width dependent.
        // _value should be multiplied by the current advance-width to get the new font-size.
        AdvanceWidth,
    };

    static CSSLengthPercentage FromString(const wchar_t* str);

    float Resolve(float fallback, float dpi, float fontSize, float advanceWidth) const noexcept;

private:
    float _value = 0;
    ReferenceFrame _referenceFrame = ReferenceFrame::None;
};
