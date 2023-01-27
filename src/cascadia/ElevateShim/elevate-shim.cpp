// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <string>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>
#include <gsl/gsl_util>
#include <gsl/pointers>
#include <shellapi.h>
#include <appmodel.h>

// BODGY
//
// If we try to do this in the Terminal itself, then there's a bunch of weird
// things that can go wrong and prevent the elevated Terminal window from
// getting created. Specifically, if the origin Terminal exits right away after
// spawning the elevated WT, then ShellExecute might not successfully complete
// the elevation. What's even more, the Terminal will mysteriously crash
// somewhere in XAML land.
//
// To mitigate this, the Terminal will call into us with the commandline it
// wants elevated. We'll hang around until ShellExecute is finished, so that the
// process can successfully elevate.

#pragma warning(suppress : 26461) // we can't change the signature of wWinMain
int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // This will invoke an elevated terminal in two possible ways. See GH#14501
    // In both scenarios, it passes the entire cmdline as-is to the new process.

    auto cmd = GetIdealWtShellExecuteTarget();
    // Go!

    // The cmdline argument passed to WinMain is stripping the first argument.
    // Using GetCommandLine() instead for lParameters

    // disable warnings from SHELLEXECUTEINFOW struct. We can't fix that.
#pragma warning(push)
#pragma warning(disable : 26476) // Macro uses naked union over variant.
    SHELLEXECUTEINFOW seInfo{ 0 };
#pragma warning(pop)

    seInfo.cbSize = sizeof(seInfo);
    seInfo.fMask = SEE_MASK_DEFAULT;
    seInfo.lpVerb = L"runas"; // This asks the shell to elevate the process
    seInfo.lpFile = cmd.c_str(); // This is `shell:AppsFolder\...` or `...\WindowsTerminal.exe`
    seInfo.lpParameters = GetCommandLine(); // This is `new-tab -p {guid}`
    seInfo.nShow = SW_SHOWNORMAL;
    LOG_IF_WIN32_BOOL_FALSE(ShellExecuteExW(&seInfo));

    return 0;
}
