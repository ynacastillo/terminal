// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// This application allows you to monitor the text buffer contents of ConPTY.
// All you need to do is run this application in Windows Terminal and it will pop up a window.
// At the time of writing the implementation is rudimentary. It has no support for wide glyphs and is very slow.

#include "pch.h"

#include <wil/win32_helpers.h>

#pragma warning(disable : 4100) // '...': unreferenced formal parameter

// WS_OVERLAPPEDWINDOW without WS_THICKFRAME, which disables resize by the user.
static constexpr DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

static bool g_firstPaint = true;
static std::vector<CHAR_INFO> g_charInfoBuffer;

LRESULT WndProc(HWND hwnd, UINT message, size_t wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
    {
        const auto out = GetStdHandle(STD_OUTPUT_HANDLE);

        CONSOLE_SCREEN_BUFFER_INFOEX info{ .cbSize = sizeof(info) };
        if (!GetConsoleScreenBufferInfoEx(out, &info))
        {
            PostQuitMessage(0);
            return 0;
        }

        auto bufferSize = info.dwSize;
        bufferSize.X += 10;
        bufferSize.Y += 10;

        if (const auto s = static_cast<size_t>(bufferSize.X) * bufferSize.Y; g_charInfoBuffer.size() < s)
        {
            g_charInfoBuffer.resize(s);
        }

        SMALL_RECT readArea{ 0, 0, bufferSize.X, bufferSize.Y };
        if (!ReadConsoleOutputW(out, g_charInfoBuffer.data(), bufferSize, {}, &readArea))
        {
            PostQuitMessage(0);
            return 0;
        }

        COORD readSize;
        readSize.X = readArea.Right + 1;
        readSize.Y = readArea.Bottom + 1;

        const auto dc = wil::GetDC(hwnd);
        const auto dpi = GetDpiForWindow(hwnd);

        const LOGFONTW lf{
            .lfHeight = -MulDiv(10, dpi, 72),
            .lfWeight = FW_REGULAR,
            .lfCharSet = DEFAULT_CHARSET,
            .lfQuality = PROOF_QUALITY,
            .lfPitchAndFamily = FIXED_PITCH | FF_MODERN,
        };
        const wil::unique_hfont font{ CreateFontIndirectW(&lf) };
        const auto restoreFont = wil::SelectObject(dc.get(), font.get());

        SIZE cellSize;
        GetTextExtentPoint32W(dc.get(), L"0", 1, &cellSize);

        RECT windowRect{ 0, 0, cellSize.cx * readSize.X, cellSize.cy * readSize.Y };
        AdjustWindowRectExForDpi(&windowRect, windowStyle, FALSE, 0, dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);

        COLORREF lastFG = 0xffffffff;
        COLORREF lastBG = 0xffffffff;

        for (SHORT y = 0; y < readSize.Y; y++)
        {
            for (SHORT x = 0; x < readSize.X; x++)
            {
                const auto& ci = g_charInfoBuffer[y * bufferSize.X + x];
                const auto fg = info.ColorTable[(ci.Attributes >> 0) & 15];
                const auto bg = info.ColorTable[(ci.Attributes >> 4) & 15];

                if (lastFG != fg)
                {
                    SetTextColor(dc.get(), fg);
                    lastFG = fg;
                }
                if (lastBG != bg)
                {
                    SetBkColor(dc.get(), bg);
                    lastBG = bg;
                }

                RECT r;
                r.left = cellSize.cx * x;
                r.top = cellSize.cy * y;
                r.right = r.left + cellSize.cx;
                r.bottom = r.top + cellSize.cy;

                ExtTextOutW(dc.get(), r.left, r.top, ETO_CLIPPED, &r, &ci.Char.UnicodeChar, 1, nullptr);
            }
        }

        if (g_firstPaint)
        {
            g_firstPaint = false;
            ShowWindow(hwnd, SW_SHOWNORMAL);
        }

        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

static void winMainImpl(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        MessageBoxW(nullptr, L"This application needs to be spawned from within a console session.", L"Failure", MB_ICONWARNING | MB_OK);
        return;
    }

    const WNDCLASSEXW wc{
        .cbSize = sizeof(wc),
        .style = CS_OWNDC,
        .lpfnWndProc = WndProc,
        .hInstance = hInstance,
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .lpszClassName = L"ConsoleMonitor",
    };
    THROW_LAST_ERROR_IF(!RegisterClassExW(&wc));

    const wil::unique_hwnd hwnd{
        THROW_LAST_ERROR_IF_NULL(CreateWindowExW(
            0,
            wc.lpszClassName,
            L"ConsoleMonitor",
            windowStyle,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            0,
            0,
            nullptr,
            nullptr,
            wc.hInstance,
            nullptr))
    };

    SetTimer(hwnd.get(), 0, 100, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    wil::WaitForDebuggerPresent(false);

    try
    {
        winMainImpl(hInstance, hPrevInstance, lpCmdLine, nShowCmd);
    }
    catch (const wil::ResultException& e)
    {
        wchar_t message[2048];
        wil::GetFailureLogString(&message[0], 2048, e.GetFailureInfo());
        MessageBoxW(nullptr, &message[0], L"Exception", MB_ICONERROR | MB_OK);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }

    return 0;
}
