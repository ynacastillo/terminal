/*++

Copyright (c) Microsoft Corporation.
Licensed under the MIT license.

Module Name:

    TfConvArea.cpp

Abstract:

    This file implements the CConversionArea Class.

Author:

Revision History:

Notes:

--*/

#include "precomp.h"
#include "ConsoleTSF.h"
#include "TfCtxtComp.h"
#include "TfConvArea.h"

//+---------------------------------------------------------------------------
// CConversionArea
//----------------------------------------------------------------------------

[[nodiscard]] HRESULT CConversionArea::DrawComposition(const std::wstring_view CompStr,
                                                       const std::vector<TF_DISPLAYATTRIBUTE>& DisplayAttributes,
                                                       const DWORD CompCursorPos)
{
    const auto encodedAttributes = _DisplayAttributesToEncodedAttributes(DisplayAttributes, CompCursorPos);
    return ImeComposeData(CompStr, encodedAttributes);
}

[[nodiscard]] HRESULT CConversionArea::ClearComposition()
{
    return ImeClearComposeData();
}

[[nodiscard]] HRESULT CConversionArea::DrawResult(const std::wstring_view ResultStr)
{
    return ImeComposeResult(ResultStr);
}

[[nodiscard]] std::vector<BYTE> CConversionArea::_DisplayAttributesToEncodedAttributes(const std::vector<TF_DISPLAYATTRIBUTE>& DisplayAttributes,
                                                                                       const DWORD CompCursorPos)
{
    std::vector<BYTE> encodedAttrs;
    for (const auto& da : DisplayAttributes)
    {
        BYTE bAttr;

        if (da.bAttr == TF_ATTR_OTHER || da.bAttr > TF_ATTR_FIXEDCONVERTED)
        {
            bAttr = ATTR_TARGET_CONVERTED;
        }
        else
        {
            if (da.bAttr == TF_ATTR_INPUT_ERROR)
            {
                bAttr = ATTR_CONVERTED;
            }
            else
            {
                bAttr = (BYTE)da.bAttr;
            }
        }
        encodedAttrs.emplace_back(bAttr);
    }

    if (CompCursorPos != -1)
    {
        if (CompCursorPos == 0)
        {
            encodedAttrs[CompCursorPos] |= CONIME_CURSOR_LEFT; // special handling for ConSrv... 0x20 = COMMON_LVB_GRID_SINGLEFLAG + COMMON_LVB_GRID_LVERTICAL
        }
        else if (CompCursorPos - 1 < DisplayAttributes.size())
        {
            encodedAttrs[CompCursorPos - 1] |= CONIME_CURSOR_RIGHT; // special handling for ConSrv... 0x10 = COMMON_LVB_GRID_SINGLEFLAG + COMMON_LVB_GRID_RVERTICAL
        }
    }

    return encodedAttrs;
}
