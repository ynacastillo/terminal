/*++

Copyright (c) Microsoft Corporation.
Licensed under the MIT license.

Module Name:

    TfContext.h

Abstract:

    This file defines the CConsoleTSF Interface Class.

Author:

Revision History:

Notes:

--*/

#pragma once

class CConversionArea;

class CConsoleTSF :
    public ITfContextOwner,
    public ITfContextOwnerCompositionSink,
    public ITfInputProcessorProfileActivationSink,
    public ITfUIElementSink,
    public ITfCleanupContextSink,
    public ITfTextEditSink
{
public:
    CConsoleTSF(HWND hwndConsole, GetSuggestionWindowPos pfnPosition, GetTextBoxAreaPos pfnTextArea);
    virtual ~CConsoleTSF();

public:
    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ITfContextOwner
    STDMETHODIMP GetACPFromPoint(const POINT*, DWORD, LONG* pCP) override
    {
        if (pCP)
        {
            *pCP = 0;
        }

        return S_OK;
    }

    // This returns Rectangle of the text box of whole console.
    // When a user taps inside the rectangle while hardware keyboard is not available,
    // touch keyboard is invoked.
    STDMETHODIMP GetScreenExt(RECT* pRect) override
    {
        if (pRect)
        {
            *pRect = _pfnTextArea();
        }

        return S_OK;
    }

    // This returns rectangle of current command line edit area.
    // When a user types in East Asian language, candidate window is shown at this position.
    // Emoji and more panel (Win+.) is shown at the position, too.
    STDMETHODIMP GetTextExt(LONG, LONG, RECT* pRect, BOOL* pbClipped) override
    {
        if (pRect)
        {
            *pRect = _pfnPosition();
        }

        if (pbClipped)
        {
            *pbClipped = FALSE;
        }

        return S_OK;
    }

    STDMETHODIMP GetStatus(TF_STATUS* pTfStatus) override
    {
        if (pTfStatus)
        {
            pTfStatus->dwDynamicFlags = 0;
            pTfStatus->dwStaticFlags = TF_SS_TRANSITORY;
        }
        return pTfStatus ? S_OK : E_INVALIDARG;
    }
    STDMETHODIMP GetWnd(HWND* phwnd) override
    {
        *phwnd = _hwndConsole;
        return S_OK;
    }
    STDMETHODIMP GetAttribute(REFGUID, VARIANT*) override
    {
        return E_NOTIMPL;
    }

    // ITfContextOwnerCompositionSink methods
    STDMETHODIMP OnStartComposition(ITfCompositionView* pComposition, BOOL* pfOk) override;
    STDMETHODIMP OnUpdateComposition(ITfCompositionView* pComposition, ITfRange* pRangeNew) override;
    STDMETHODIMP OnEndComposition(ITfCompositionView* pComposition) override;

    // ITfInputProcessorProfileActivationSink
    STDMETHODIMP OnActivated(DWORD dwProfileType, LANGID langid, REFCLSID clsid, REFGUID catid, REFGUID guidProfile, HKL hkl, DWORD dwFlags) override;

    // ITfUIElementSink methods
    STDMETHODIMP BeginUIElement(DWORD dwUIElementId, BOOL* pbShow) override;
    STDMETHODIMP UpdateUIElement(DWORD dwUIElementId) override;
    STDMETHODIMP EndUIElement(DWORD dwUIElementId) override;

    // ITfCleanupContextSink methods
    STDMETHODIMP OnCleanupContext(TfEditCookie ecWrite, ITfContext* pic) override;

    // ITfTextEditSink methods
    STDMETHODIMP OnEndEdit(ITfContext* pInputContext, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord) override;

public:
    CConversionArea* CreateConversionArea();
    CConversionArea* GetConversionArea() const { return _pConversionArea.get(); }
    ITfContext* GetInputContext() const { return _context.get(); }
    HWND GetConsoleHwnd() const { return _hwndConsole; }
    TfClientId GetTfClientId() const { return _tid; }
    bool IsInComposition() const { return (_cCompositions > 0); }
    void OnEditSession() { _fEditSessionRequested = FALSE; }
    bool IsPendingCompositionCleanup() const { return _fCleanupSessionRequested || _fCompositionCleanupSkipped; }
    void OnCompositionCleanup(bool bSucceeded)
    {
        _fCleanupSessionRequested = FALSE;
        _fCompositionCleanupSkipped = !bSucceeded;
    }
    void SetModifyingDocFlag(bool fSet) { _fModifyingDoc = fSet; }
    void SetFocus(bool fSet) const
    {
        if (!fSet && _cCompositions)
        {
            // Close (terminate) any open compositions when losing the input focus.
            if (_context)
            {
                if (const auto spCompositionServices = _context.try_query<ITfContextOwnerCompositionServices>())
                {
                    spCompositionServices->TerminateComposition(nullptr);
                }
            }
        }
    }

    // A workaround for a MS Korean IME scenario where the IME appends a whitespace
    // composition programmatically right after completing a keyboard input composition.
    // Since post-composition clean-up is an async operation, the programmatic whitespace
    // composition gets completed before the previous composition cleanup happened,
    // and this results in a double insertion of the first composition. To avoid that, we'll
    // store the length of the last completed composition here until it's cleaned up.
    // (for simplicity, this patch doesn't provide a generic solution for all possible
    // scenarios with subsequent synchronous compositions, only for the known 'append').
    long GetCompletedRangeLength() const { return _cchCompleted; }
    void SetCompletedRangeLength(long cch) { _cchCompleted = cch; }

private:
    void _cleanup() const noexcept;
    [[nodiscard]] HRESULT _OnUpdateComposition();
    [[nodiscard]] HRESULT _OnCompleteComposition();
    static bool _HasCompositionChanged(ITfContext* context, TfEditCookie ec, ITfEditRecord* editRecord);

    ULONG _referenceCount = 1;

    // Cicero stuff.
    TfClientId _tid = 0;
    wil::com_ptr<ITfThreadMgrEx> _threadMgrEx;
    wil::com_ptr<ITfDocumentMgr> _documentMgr;
    wil::com_ptr<ITfContext> _context;
    wil::com_ptr<ITfSource> _threadMgrExSource;
    wil::com_ptr<ITfSource> _contextSource;
    wil::com_ptr<ITfSourceSingle> _contextSourceSingle;

    // Event sink cookies.
    DWORD _dwContextOwnerCookie = 0;
    DWORD _dwUIElementSinkCookie = 0;
    DWORD _dwTextEditSinkCookie = 0;
    DWORD _dwActivationSinkCookie = 0;

    // Conversion area object for the languages.
    std::unique_ptr<CConversionArea> _pConversionArea;

    // Console info.
    HWND _hwndConsole = nullptr;
    GetSuggestionWindowPos _pfnPosition = nullptr;
    GetTextBoxAreaPos _pfnTextArea = nullptr;

    // Miscellaneous flags
    bool _fModifyingDoc = false; // Set true, when calls ITfRange::SetText
    bool _fEditSessionRequested = false;
    bool _fCleanupSessionRequested = false;
    bool _fCompositionCleanupSkipped = false;

    int _cCompositions = 0;
    long _cchCompleted = 0; // length of completed composition waiting for cleanup
};
