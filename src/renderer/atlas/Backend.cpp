#include "pch.h"
#include "Backend.h"

using namespace Microsoft::Console::Render::Atlas;

wil::com_ptr<ID3D11Texture2D> SwapChainManager::GetBuffer() const
{
    wil::com_ptr<ID3D11Texture2D> buffer;
    THROW_IF_FAILED(_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
    return buffer;
}

void SwapChainManager::Present(const RenderingPayload& p)
{
    const til::rect fullRect{ 0, 0, p.s->cellCount.x, p.s->cellCount.y };

    if (!p.dirtyRect)
    {
        return;
    }

    if (p.dirtyRect != fullRect)
    {
        auto dirtyRectInPx = p.dirtyRect;
        dirtyRectInPx.left *= p.s->font->cellSize.x;
        dirtyRectInPx.top *= p.s->font->cellSize.y;
        dirtyRectInPx.right *= p.s->font->cellSize.x;
        dirtyRectInPx.bottom *= p.s->font->cellSize.y;

        RECT scrollRect{};
        POINT scrollOffset{};
        DXGI_PRESENT_PARAMETERS params{
            .DirtyRectsCount = 1,
            .pDirtyRects = dirtyRectInPx.as_win32_rect(),
        };

        if (p.scrollOffset)
        {
            scrollRect = {
                0,
                std::max<til::CoordType>(0, p.scrollOffset),
                p.s->cellCount.x,
                p.s->cellCount.y + std::min<til::CoordType>(0, p.scrollOffset),
            };
            scrollOffset = {
                0,
                p.scrollOffset,
            };

            scrollRect.top *= p.s->font->cellSize.y;
            scrollRect.right *= p.s->font->cellSize.x;
            scrollRect.bottom *= p.s->font->cellSize.y;

            scrollOffset.y *= p.s->font->cellSize.y;

            params.pScrollRect = &scrollRect;
            params.pScrollOffset = &scrollOffset;
        }

        THROW_IF_FAILED(_swapChain->Present1(1, 0, &params));
    }
    else
    {
        THROW_IF_FAILED(_swapChain->Present(1, 0));
    }

    _waitForPresentation = true;
}

void SwapChainManager::WaitUntilCanRender() noexcept
{
    // IDXGISwapChain2::GetFrameLatencyWaitableObject returns an auto-reset event.
    // Once we've waited on the event, waiting on it again will block until the timeout elapses.
    // _waitForPresentation guards against this.
    if constexpr (!debugDisableFrameLatencyWaitableObject)
    {
        if (_waitForPresentation)
        {
            WaitForSingleObjectEx(_frameLatencyWaitableObject.get(), 100, true);
            _waitForPresentation = false;
        }
    }
}

void SwapChainManager::_createSwapChain(const RenderingPayload& p, IUnknown* device)
{
    _swapChain.reset();
    _frameLatencyWaitableObject.reset();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = p.s->targetSize.x;
    desc.Height = p.s->targetSize.y;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Sometimes up to 2 buffers are locked, for instance during screen capture or when moving the window.
    // 3 buffers seems to guarantee a stable framerate at display frequency at all times.
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_NONE;
    // DXGI_SWAP_EFFECT_FLIP_DISCARD is a mode that was created at a time were display drivers
    // lacked support for Multiplane Overlays (MPO) and were copying buffers was expensive.
    // This allowed DWM to quickly draw overlays (like gamebars) on top of rendered content.
    // With faster GPU memory in general and with support for MPO in particular this isn't
    // really an advantage anymore. Instead DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL allows for a
    // more "intelligent" composition and display updates to occur like Panel Self Refresh
    // (PSR) which requires dirty rectangles (Present1 API) to work correctly.
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // If our background is opaque we can enable "independent" flips by setting DXGI_ALPHA_MODE_IGNORE.
    // As our swap chain won't have to compose with DWM anymore it reduces the display latency dramatically.
    desc.AlphaMode = p.s->target->enableTransparentBackground ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = flags;

    wil::com_ptr<IDXGISwapChain1> swapChain0;

    if (p.s->target->hwnd)
    {
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        THROW_IF_FAILED(p.dxgiFactory->CreateSwapChainForHwnd(device, p.s->target->hwnd, &desc, nullptr, nullptr, swapChain0.addressof()));
    }
    else
    {
        const auto module = GetModuleHandleW(L"dcomp.dll");
        const auto DCompositionCreateSurfaceHandle = GetProcAddressByFunctionDeclaration(module, DCompositionCreateSurfaceHandle);
        THROW_LAST_ERROR_IF(!DCompositionCreateSurfaceHandle);

        // As per: https://docs.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-dcompositioncreatesurfacehandle
        static constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;
        THROW_IF_FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, _swapChainHandle.addressof()));
        THROW_IF_FAILED(p.dxgiFactory.query<IDXGIFactoryMedia>()->CreateSwapChainForCompositionSurfaceHandle(device, _swapChainHandle.get(), &desc, nullptr, swapChain0.addressof()));
    }

    _swapChain = swapChain0.query<IDXGISwapChain2>();
    _frameLatencyWaitableObject.reset(_swapChain->GetFrameLatencyWaitableObject());
    _targetGeneration = p.s->target.generation();
    _targetSize = p.s->targetSize;
    _waitForPresentation = true;

    WaitUntilCanRender();

    if (p.swapChainChangedCallback)
    {
        try
        {
            p.swapChainChangedCallback(_swapChainHandle.get());
        }
        CATCH_LOG()
    }
}

void SwapChainManager::_updateMatrixTransform(const RenderingPayload& p) const
{
    // XAML's SwapChainPanel combines the worst of both worlds and applies a transform to the
    // swap chain to match the display scale and not just if it got a perspective transform, etc.
    // This if condition undoes the damage no one asked for. (Seriously though: Why?)
    if (_fontGeneration != p.s->font.generation() && !p.s->target->hwnd)
    {
        const DXGI_MATRIX_3X2_F matrix{
            ._11 = p.d.font.dipPerPixel,
            ._22 = p.d.font.dipPerPixel,
        };
        THROW_IF_FAILED(_swapChain->SetMatrixTransform(&matrix));
    }
}
