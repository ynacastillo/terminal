// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../inc/FontInfoBase.hpp"

CellSizeInDIP CellSizeInDIP::FromInteger_DoNotUse(til::size size) noexcept
{
    return { static_cast<float>(size.width), static_cast<float>(size.height) };
}

til::size CellSizeInDIP::AsInteger_DoNotUse() const noexcept
{
    return { til::math::rounding, width, height };
}

FontInfoBase::FontInfoBase(std::wstring faceName, const unsigned char family, const unsigned weight, const unsigned uiCodePage) :
    _faceName{ std::move(faceName) },
    _family{ family },
    _weight{ weight },
    _codePage{ uiCodePage }
{
}

unsigned char FontInfoBase::GetFamily() const noexcept
{
    return _family;
}

unsigned int FontInfoBase::GetWeight() const noexcept
{
    return _weight;
}

const std::wstring& FontInfoBase::GetFaceName() const noexcept
{
    return _faceName;
}

unsigned int FontInfoBase::GetCodePage() const noexcept
{
    return _codePage;
}
