/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- FontInfo.hpp

Abstract:
- This serves as the structure defining font information.  There are three
  relevant classes defined.

- FontInfo - derived from FontInfoBase.  It also has font size
  information - both the width and height of the requested font, as
  well as the measured height and width of L'0' from GDI.  All
  coordinates { X, Y } pair are non zero and always set to some
  reasonable value, even when GDI APIs fail.  This helps avoid
  divide by zero issues while performing various sizing
  calculations.

Author(s):
- Michael Niksa (MiNiksa) 17-Nov-2015
--*/

#pragma once

#include "FontInfoBase.hpp"

struct FontInfo : FontInfoBase
{
    void SetFromEngine(
        std::wstring faceName,
        const unsigned char family,
        const unsigned int weight,
        const unsigned int codePage,
        CellSizeInDIP cellSizeInDIP,
        float fontSizeInPt,
        til::size cellSizeInPx);

    const CellSizeInDIP& GetUnscaledSize() const noexcept;
    float GetFontSize() const noexcept;

    const til::size& GetSize() const noexcept;

    bool IsTrueTypeFont() const noexcept;
    void FillLegacyNameBuffer(wchar_t (&buffer)[LF_FACESIZE]) const noexcept;

private:
    CellSizeInDIP _cellSizeInDIP;
    float _fontSizeInPt = 0;

    til::size _cellSizeInPx;
};
