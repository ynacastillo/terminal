/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- conimeinfo.h

Abstract:
- This module contains the structures for the console IME entrypoints
  for overall control

Author:
- Michael Niksa (MiNiksa) 10-May-2018

Revision History:
- From pieces of convarea.cpp originally authored by KazuM
--*/

#pragma once

#include "../buffer/out/TextAttribute.hpp"
#include "conareainfo.h"

class ConsoleImeInfo final
{
public:
    // IME composition string information
    // There is one "composition string" per line that must be rendered on the screen
    std::vector<ConversionAreaInfo> ConvAreaCompStr;

    void RefreshAreaAttributes();
    void ClearAllAreas();
    [[nodiscard]] HRESULT ResizeAllAreas(til::size newSize);
    void WriteCompMessage(std::wstring_view text, std::span<const BYTE> attributes);
    void WriteResultMessage(std::wstring_view text);
    void RedrawCompMessage();
    void SaveCursorVisibility();
    void RestoreCursorVisibility();

private:
    void _AddConversionArea();
    void _ClearComposition();
    void _WriteUndeterminedChars(std::wstring_view text, std::span<const BYTE> attributes);
    void _InsertConvertedString(std::wstring_view text);
    static TextAttribute s_RetrieveAttributeAt(size_t pos, std::span<const BYTE> attributes);

    bool _isSavedCursorVisible = false;

    std::wstring _text;
    std::vector<BYTE> _attributes;
};
