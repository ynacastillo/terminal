// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "ConsoleTSF.h"

#include "TfConvArea.h"
#include "TfEditSession.h"

/* 626761ad-78d2-44d2-be8b-752cf122acec */
static constexpr GUID GUID_APPLICATION = { 0x626761ad, 0x78d2, 0x44d2, { 0xbe, 0x8b, 0x75, 0x2c, 0xf1, 0x22, 0xac, 0xec } };

CConsoleTSF::CConsoleTSF(HWND hwndConsole, GetSuggestionWindowPos pfnPosition, GetTextBoxAreaPos pfnTextArea) :
    _hwndConsole(hwndConsole),
    _pfnPosition(pfnPosition),
    _pfnTextArea(pfnTextArea)
{
    try
    {
        // There's no point in calling TF_GetThreadMgr. ITfThreadMgr is a per-thread singleton.
        _threadMgrEx = wil::CoCreateInstance<ITfThreadMgrEx>(CLSID_TF_ThreadMgr, CLSCTX_INPROC_SERVER);

        THROW_IF_FAILED(_threadMgrEx->ActivateEx(&_tid, TF_TMAE_CONSOLE));
        THROW_IF_FAILED(_threadMgrEx->CreateDocumentMgr(_documentMgr.addressof()));

        TfEditCookie ecTmp;
        THROW_IF_FAILED(_documentMgr->CreateContext(_tid, 0, static_cast<ITfContextOwnerCompositionSink*>(this), _context.addressof(), &ecTmp));

        _threadMgrExSource = _threadMgrEx.query<ITfSource>();
        THROW_IF_FAILED(_threadMgrExSource->AdviseSink(IID_ITfInputProcessorProfileActivationSink, static_cast<ITfInputProcessorProfileActivationSink*>(this), &_dwActivationSinkCookie));
        THROW_IF_FAILED(_threadMgrExSource->AdviseSink(IID_ITfUIElementSink, static_cast<ITfUIElementSink*>(this), &_dwUIElementSinkCookie));

        _contextSource = _context.query<ITfSource>();
        THROW_IF_FAILED(_contextSource->AdviseSink(IID_ITfContextOwner, static_cast<ITfContextOwner*>(this), &_dwContextOwnerCookie));
        THROW_IF_FAILED(_contextSource->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &_dwTextEditSinkCookie));

        _contextSourceSingle = _context.query<ITfSourceSingle>();
        THROW_IF_FAILED(_contextSourceSingle->AdviseSingleSink(_tid, IID_ITfCleanupContextSink, static_cast<ITfCleanupContextSink*>(this)));

        THROW_IF_FAILED(_documentMgr->Push(_context.get()));

        // Collect the active keyboard layout info.
        if (const auto spITfProfilesMgr = wil::CoCreateInstanceNoThrow<ITfInputProcessorProfileMgr>(CLSID_TF_InputProcessorProfiles, CLSCTX_INPROC_SERVER))
        {
            TF_INPUTPROCESSORPROFILE ipp;
            if (SUCCEEDED(spITfProfilesMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &ipp)))
            {
                std::ignore = CConsoleTSF::OnActivated(ipp.dwProfileType, ipp.langid, ipp.clsid, ipp.catid, ipp.guidProfile, ipp.hkl, ipp.dwFlags);
            }
        }
    }
    catch (...)
    {
        _cleanup();
        throw;
    }
}

CConsoleTSF::~CConsoleTSF()
{
    _cleanup();
}

void CConsoleTSF::_cleanup() const noexcept
{
    if (_contextSourceSingle)
    {
        _contextSourceSingle->UnadviseSingleSink(_tid, IID_ITfCleanupContextSink);
    }
    if (_contextSource)
    {
        _contextSource->UnadviseSink(_dwTextEditSinkCookie);
        _contextSource->UnadviseSink(_dwContextOwnerCookie);
    }
    if (_threadMgrExSource)
    {
        _threadMgrExSource->UnadviseSink(_dwUIElementSinkCookie);
        _threadMgrExSource->UnadviseSink(_dwActivationSinkCookie);
    }

    // Clear the Cicero reference to our document manager.
    if (_threadMgrEx && _documentMgr)
    {
        wil::com_ptr<ITfDocumentMgr> spPrevDocMgr;
        _threadMgrEx->AssociateFocus(_hwndConsole, nullptr, spPrevDocMgr.addressof());
    }

    // Dismiss the input context and document manager.
    if (_documentMgr)
    {
        _documentMgr->Pop(TF_POPF_ALL);
    }

    // Deactivate per-thread Cicero.
    if (_threadMgrEx)
    {
        _threadMgrEx->Deactivate();
    }
}

STDMETHODIMP CConsoleTSF::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
    {
        return E_POINTER;
    }

    if (IsEqualGUID(riid, IID_ITfCleanupContextSink))
    {
        *ppvObj = static_cast<ITfCleanupContextSink*>(this);
    }
    else if (IsEqualGUID(riid, IID_ITfContextOwnerCompositionSink))
    {
        *ppvObj = static_cast<ITfContextOwnerCompositionSink*>(this);
    }
    else if (IsEqualGUID(riid, IID_ITfUIElementSink))
    {
        *ppvObj = static_cast<ITfUIElementSink*>(this);
    }
    else if (IsEqualGUID(riid, IID_ITfContextOwner))
    {
        *ppvObj = static_cast<ITfContextOwner*>(this);
    }
    else if (IsEqualGUID(riid, IID_ITfInputProcessorProfileActivationSink))
    {
        *ppvObj = static_cast<ITfInputProcessorProfileActivationSink*>(this);
    }
    else if (IsEqualGUID(riid, IID_ITfTextEditSink))
    {
        *ppvObj = static_cast<ITfTextEditSink*>(this);
    }
    else if (IsEqualGUID(riid, IID_IUnknown))
    {
        *ppvObj = static_cast<IUnknown*>(static_cast<ITfContextOwner*>(this));
    }
    else
    {
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE CConsoleTSF::AddRef()
{
    return InterlockedIncrement(&_referenceCount);
}

ULONG STDMETHODCALLTYPE CConsoleTSF::Release()
{
    const auto cr = InterlockedDecrement(&_referenceCount);
    if (cr == 0)
    {
        delete this;
    }
    return cr;
}

STDMETHODIMP CConsoleTSF::OnCleanupContext(TfEditCookie ecWrite, ITfContext* pic)
{
    //
    // Remove GUID_PROP_COMPOSING
    //
    wil::com_ptr<ITfProperty> prop;
    if (SUCCEEDED(pic->GetProperty(GUID_PROP_COMPOSING, prop.addressof())))
    {
        wil::com_ptr<IEnumTfRanges> enumranges;
        if (SUCCEEDED(prop->EnumRanges(ecWrite, enumranges.addressof(), nullptr)))
        {
            wil::com_ptr<ITfRange> rangeTmp;
            while (enumranges->Next(1, rangeTmp.addressof(), nullptr) == S_OK)
            {
                VARIANT var;
                VariantInit(&var);
                prop->GetValue(ecWrite, rangeTmp.get(), &var);
                if ((var.vt == VT_I4) && (var.lVal != 0))
                {
                    prop->Clear(ecWrite, rangeTmp.get());
                }
            }
        }
    }
    return S_OK;
}

STDMETHODIMP CConsoleTSF::OnStartComposition(ITfCompositionView* pCompView, BOOL* pfOk)
{
    if (!_pConversionArea || (_cCompositions > 0 && (!_fModifyingDoc)))
    {
        *pfOk = FALSE;
    }
    else
    {
        *pfOk = TRUE;
        // Ignore compositions triggered by our own edit sessions
        // (i.e. when the application is the composition owner)
        auto clsidCompositionOwner = GUID_APPLICATION;
        pCompView->GetOwnerClsid(&clsidCompositionOwner);
        if (!IsEqualGUID(clsidCompositionOwner, GUID_APPLICATION))
        {
            _cCompositions++;
            if (_cCompositions == 1)
            {
                LOG_IF_FAILED(ImeStartComposition());
            }
        }
    }
    return S_OK;
}

STDMETHODIMP CConsoleTSF::OnUpdateComposition(ITfCompositionView* /*pComp*/, ITfRange*)
{
    return S_OK;
}

STDMETHODIMP CConsoleTSF::OnEndComposition(ITfCompositionView* pCompView)
{
    if (!_cCompositions || !_pConversionArea)
    {
        return E_FAIL;
    }
    // Ignore compositions triggered by our own edit sessions
    // (i.e. when the application is the composition owner)
    auto clsidCompositionOwner = GUID_APPLICATION;
    pCompView->GetOwnerClsid(&clsidCompositionOwner);
    if (!IsEqualGUID(clsidCompositionOwner, GUID_APPLICATION))
    {
        _cCompositions--;
        if (!_cCompositions)
        {
            LOG_IF_FAILED(_OnCompleteComposition());
            LOG_IF_FAILED(ImeEndComposition());
        }
    }
    return S_OK;
}

STDMETHODIMP CConsoleTSF::OnEndEdit(ITfContext* pInputContext, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord)
{
    if (_cCompositions && _pConversionArea && _HasCompositionChanged(pInputContext, ecReadOnly, pEditRecord))
    {
        LOG_IF_FAILED(_OnUpdateComposition());
    }
    return S_OK;
}

STDMETHODIMP CConsoleTSF::OnActivated(DWORD /*dwProfileType*/, LANGID /*langid*/, REFCLSID /*clsid*/, REFGUID catid, REFGUID /*guidProfile*/, HKL /*hkl*/, DWORD dwFlags)
{
    if (!(dwFlags & TF_IPSINK_FLAG_ACTIVE))
    {
        return S_OK;
    }
    if (!IsEqualGUID(catid, GUID_TFCAT_TIP_KEYBOARD))
    {
        // Don't care for non-keyboard profiles.
        return S_OK;
    }

    try
    {
        CreateConversionArea();
    }
    CATCH_RETURN();

    return S_OK;
}

STDMETHODIMP CConsoleTSF::BeginUIElement(DWORD /*dwUIElementId*/, BOOL* pbShow)
{
    *pbShow = TRUE;
    return S_OK;
}

STDMETHODIMP CConsoleTSF::UpdateUIElement(DWORD /*dwUIElementId*/)
{
    return S_OK;
}

STDMETHODIMP CConsoleTSF::EndUIElement(DWORD /*dwUIElementId*/)
{
    return S_OK;
}

CConversionArea* CConsoleTSF::CreateConversionArea()
{
    const bool fHadConvArea = (_pConversionArea != nullptr);

    if (!_pConversionArea)
    {
        _pConversionArea = std::make_unique<CConversionArea>();
    }

    // Associate the document\context with the console window.
    if (!fHadConvArea)
    {
        wil::com_ptr<ITfDocumentMgr> spPrevDocMgr;
        _threadMgrEx->AssociateFocus(_hwndConsole, _pConversionArea ? _documentMgr.get() : nullptr, spPrevDocMgr.addressof());
    }

    return _pConversionArea.get();
}

[[nodiscard]] HRESULT CConsoleTSF::_OnUpdateComposition()
{
    if (_fEditSessionRequested)
    {
        return S_FALSE;
    }

    auto hr = E_OUTOFMEMORY;
    if (const auto pEditSession = new (std::nothrow) CEditSessionUpdateCompositionString(this))
    {
        // Can't use TF_ES_SYNC because called from OnEndEdit.
        _fEditSessionRequested = TRUE;
        _context->RequestEditSession(_tid, pEditSession, TF_ES_READWRITE, &hr);
        if (FAILED(hr))
        {
            pEditSession->Release();
            _fEditSessionRequested = FALSE;
        }
    }
    return hr;
}

[[nodiscard]] HRESULT CConsoleTSF::_OnCompleteComposition()
{
    // Update the composition area.

    auto hr = E_OUTOFMEMORY;
    if (const auto pEditSession = new (std::nothrow) CEditSessionCompositionComplete(this))
    {
        // The composition could have been finalized because of a caret move, therefore it must be
        // inserted synchronously while at the original caret position.(TF_ES_SYNC is ok for a nested RO session).
        _context->RequestEditSession(_tid, pEditSession, TF_ES_READ | TF_ES_SYNC, &hr);
        if (FAILED(hr))
        {
            pEditSession->Release();
        }
    }

    // Cleanup (empty the context range) after the last composition, unless a new one has started.
    if (!_fCleanupSessionRequested)
    {
        _fCleanupSessionRequested = TRUE;
        if (const auto pEditSessionCleanup = new (std::nothrow) CEditSessionCompositionCleanup(this))
        {
            // Can't use TF_ES_SYNC because requesting RW while called within another session.
            // For the same reason, must use explicit TF_ES_ASYNC, or the request will be rejected otherwise.
            _context->RequestEditSession(_tid, pEditSessionCleanup, TF_ES_READWRITE | TF_ES_ASYNC, &hr);
            if (FAILED(hr))
            {
                pEditSessionCleanup->Release();
                _fCleanupSessionRequested = FALSE;
            }
        }
    }
    return hr;
}

static wil::com_ptr<ITfRange> getTrackCompositionProperty(ITfContext* context, TfEditCookie ec)
{
    wil::com_ptr<ITfProperty> Property;
    if (FAILED(context->GetProperty(GUID_PROP_CONIME_TRACKCOMPOSITION, &Property)))
    {
        return {};
    }

    wil::com_ptr<IEnumTfRanges> ranges;
    if (FAILED(Property->EnumRanges(ec, ranges.addressof(), NULL)))
    {
        return {};
    }

    VARIANT var{ .vt = VT_EMPTY };
    wil::com_ptr<ITfRange> range;
    while (ranges->Next(1, range.put(), nullptr) == S_OK)
    {
        if (SUCCEEDED(Property->GetValue(ec, range.get(), &var)) && V_VT(&var) == VT_I4 && V_I4(&var) != 0)
        {
            return range;
        }
        VariantClear(&var);
    }

    return {};
}

bool CConsoleTSF::_HasCompositionChanged(ITfContext* context, TfEditCookie ec, ITfEditRecord* editRecord)
{
    BOOL changed;
    if (SUCCEEDED(editRecord->GetSelectionStatus(&changed)) && changed)
    {
        return TRUE;
    }

    const auto rangeTrackComposition = getTrackCompositionProperty(context, ec);
    // If there is no track composition property,
    // the composition has been changed since we put it.
    if (!rangeTrackComposition)
    {
        return TRUE;
    }

    // Get the text range that does not include read only area for reconversion.
    wil::com_ptr<ITfRange> rangeAllText;
    LONG cch;
    if (FAILED(CEditSessionObject::GetAllTextRange(ec, context, rangeAllText.addressof(), &cch)))
    {
        return FALSE;
    }

    // If the start position of the track composition range is not the beginning of IC,
    // the composition has been changed since we put it.
    LONG lResult;
    if (FAILED(rangeTrackComposition->CompareStart(ec, rangeAllText.get(), TF_ANCHOR_START, &lResult)))
    {
        return FALSE;
    }
    if (lResult != 0)
    {
        return TRUE;
    }

    if (FAILED(rangeTrackComposition->CompareEnd(ec, rangeAllText.get(), TF_ANCHOR_END, &lResult)))
    {
        return FALSE;
    }
    if (lResult != 0)
    {
        return TRUE;
    }

    // If the start position of the track composition range is not the beginning of IC,
    // the composition has been changed since we put it.
    //
    // If we find the changes in these property, we need to update hIMC.
    const GUID* guids[] = { &GUID_PROP_COMPOSING, &GUID_PROP_ATTRIBUTE };
    wil::com_ptr<IEnumTfRanges> EnumPropertyChanged;
    if (FAILED(editRecord->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, guids, ARRAYSIZE(guids), EnumPropertyChanged.addressof())))
    {
        return FALSE;
    }

    wil::com_ptr<ITfRange> range;
    while (EnumPropertyChanged->Next(1, range.put(), nullptr) == S_OK)
    {
        BOOL empty;
        if (range->IsEmpty(ec, &empty) != S_OK || !empty)
        {
            return TRUE;
        }
    }
    return FALSE;
}
