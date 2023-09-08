// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

unique_CConsoleTSF CreateTextServices(HWND hwndConsole, GetSuggestionWindowPos pfnPosition, GetTextBoxAreaPos pfnTextArea)
{
    unique_CConsoleTSF tsf{ new CConsoleTSF(hwndConsole, pfnPosition, pfnTextArea) };
    if (SUCCEEDED(tsf->Initialize()))
    {
        // Conhost calls this function only when the console window has focus.
        tsf->SetFocus(TRUE);
    }
    return tsf;
}

void DeleteTextServices(CConsoleTSF* tsf)
{
    tsf->Uninitialize();
    delete tsf;
}
