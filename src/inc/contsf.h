/*++

Copyright (c) Microsoft Corporation.
Licensed under the MIT license.

Module Name:

    contsf.h

Abstract:

    This module contains the internal structures and definitions used
    by the console IME.

Author:

    v-HirShi Jul.4.1995

Revision History:

--*/

#pragma once
#include "conime.h"

class CConsoleTSF;

void DeleteTextServices(CConsoleTSF* tsf);

using unique_CConsoleTSF = wistd::unique_ptr<CConsoleTSF, wil::function_deleter<decltype(&DeleteTextServices), DeleteTextServices>>;
typedef RECT (*GetSuggestionWindowPos)();
typedef RECT (*GetTextBoxAreaPos)();
unique_CConsoleTSF CreateTextServices(HWND hwndConsole, GetSuggestionWindowPos pfnPosition, GetTextBoxAreaPos pfnTextArea);
