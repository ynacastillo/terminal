// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../inc/FontInfo.hpp"

void FontInfo::SetFromEngine(std::wstring faceName, const unsigned char family, const unsigned weight, const unsigned codePage, CellSizeInDIP cellSizeInDIP, float fontSizeInPt, til::size cellSizeInPx)
{
    _faceName = std::move(faceName);
    _family = family;
    _weight = weight;
    _codePage = codePage;
    _cellSizeInDIP = cellSizeInDIP;
    _fontSizeInPt = fontSizeInPt;
    _cellSizeInPx = cellSizeInPx;
}

const CellSizeInDIP& FontInfo::GetUnscaledSize() const noexcept
{
    return _cellSizeInDIP;
}

float FontInfo::GetFontSize() const noexcept
{
    return _fontSizeInPt;
}

const til::size& FontInfo::GetSize() const noexcept
{
    return _cellSizeInPx;
}

bool FontInfo::IsTrueTypeFont() const noexcept
{
    return WI_IsFlagSet(_family, TMPF_TRUETYPE);
}

void FontInfo::FillLegacyNameBuffer(wchar_t (&buffer)[LF_FACESIZE]) const noexcept
{
    const auto toCopy = std::min(std::size(buffer) - 1, _faceName.size());
    const auto last = std::copy_n(_faceName.data(), toCopy, &buffer[0]);
    *last = L'\0';
}
