#pragma once

#include "common.h"

namespace Microsoft::Console::Render::Atlas
{
    inline constexpr bool debugContinuousRedraw = false;
    inline constexpr bool debugDisableFrameLatencyWaitableObject = false;
    inline constexpr bool debugDisablePartialInvalidation = true;
    inline constexpr bool debugForceD2DMode = true;

    struct SwapChainManager
    {
        void UpdateSwapChainSettings(const RenderingPayload& p, IUnknown* device, auto&& prepareRecreate, auto&& prepareResize)
        {
            if (_targetGeneration != p.s->target.generation())
            {
                if (_swapChain)
                {
                    prepareRecreate();
                }
                _createSwapChain(p, device);
            }
            else if (_targetSize != p.s->targetSize)
            {
                prepareResize();
                THROW_IF_FAILED(_swapChain->ResizeBuffers(0, _targetSize.x, _targetSize.y, DXGI_FORMAT_UNKNOWN, flags));
                _targetSize = p.s->targetSize;
            }

            _updateMatrixTransform(p);
        }

        wil::com_ptr<ID3D11Texture2D> GetBuffer() const;
        void Present(const RenderingPayload& p);
        void WaitUntilCanRender() noexcept;

    private:
        void _createSwapChain(const RenderingPayload& p, IUnknown* device);
        void _updateMatrixTransform(const RenderingPayload& p) const;

        static constexpr DXGI_SWAP_CHAIN_FLAG flags = debugDisableFrameLatencyWaitableObject ? DXGI_SWAP_CHAIN_FLAG{} : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        wil::com_ptr<IDXGISwapChain2> _swapChain;
        wil::unique_handle _swapChainHandle;
        wil::unique_handle _frameLatencyWaitableObject;
        til::generation_t _targetGeneration;
        til::generation_t _fontGeneration;
        u16x2 _targetSize;
        bool _waitForPresentation = false;
    };

    template<typename T = D2D1_COLOR_F>
    constexpr T colorFromU32(u32 rgba)
    {
        const auto r = static_cast<float>((rgba >> 0) & 0xff) / 255.0f;
        const auto g = static_cast<float>((rgba >> 8) & 0xff) / 255.0f;
        const auto b = static_cast<float>((rgba >> 16) & 0xff) / 255.0f;
        const auto a = static_cast<float>((rgba >> 24) & 0xff) / 255.0f;
        return { r, g, b, a };
    }

    template<typename T = D2D1_COLOR_F>
    constexpr T colorFromU32Premultiply(u32 rgba)
    {
        const auto r = static_cast<float>((rgba >> 0) & 0xff) / 255.0f;
        const auto g = static_cast<float>((rgba >> 8) & 0xff) / 255.0f;
        const auto b = static_cast<float>((rgba >> 16) & 0xff) / 255.0f;
        const auto a = static_cast<float>((rgba >> 24) & 0xff) / 255.0f;
        return { r * a, g * a, b * a, a };
    }

    inline f32r getGlyphRunBlackBox(const DWRITE_GLYPH_RUN& glyphRun, float baselineX, float baselineY)
    {
        DWRITE_FONT_METRICS fontMetrics;
        glyphRun.fontFace->GetMetrics(&fontMetrics);

        std::unique_ptr<DWRITE_GLYPH_METRICS[]> glyphRunMetricsHeap;
        std::array<DWRITE_GLYPH_METRICS, 8> glyphRunMetricsStack;
        DWRITE_GLYPH_METRICS* glyphRunMetrics = glyphRunMetricsStack.data();

        if (glyphRun.glyphCount > glyphRunMetricsStack.size())
        {
            glyphRunMetricsHeap = std::make_unique_for_overwrite<DWRITE_GLYPH_METRICS[]>(glyphRun.glyphCount);
            glyphRunMetrics = glyphRunMetricsHeap.get();
        }

        glyphRun.fontFace->GetDesignGlyphMetrics(glyphRun.glyphIndices, glyphRun.glyphCount, glyphRunMetrics, false);

        float const fontScale = glyphRun.fontEmSize / fontMetrics.designUnitsPerEm;
        f32r accumulatedBounds{
            FLT_MAX,
            FLT_MAX,
            FLT_MIN,
            FLT_MIN,
        };

        for (uint32_t i = 0; i < glyphRun.glyphCount; ++i)
        {
            const auto& glyphMetrics = glyphRunMetrics[i];
            const auto glyphAdvance = glyphRun.glyphAdvances ? glyphRun.glyphAdvances[i] : glyphMetrics.advanceWidth * fontScale;

            const auto left = static_cast<float>(glyphMetrics.leftSideBearing) * fontScale;
            const auto top = static_cast<float>(glyphMetrics.topSideBearing - glyphMetrics.verticalOriginY) * fontScale;
            const auto right = static_cast<float>(gsl::narrow_cast<INT32>(glyphMetrics.advanceWidth) - glyphMetrics.rightSideBearing) * fontScale;
            const auto bottom = static_cast<float>(gsl::narrow_cast<INT32>(glyphMetrics.advanceHeight) - glyphMetrics.bottomSideBearing - glyphMetrics.verticalOriginY) * fontScale;

            if (left < right && top < bottom)
            {
                auto glyphX = baselineX;
                auto glyphY = baselineY;
                if (glyphRun.glyphOffsets)
                {
                    glyphX += glyphRun.glyphOffsets[i].advanceOffset;
                    glyphY -= glyphRun.glyphOffsets[i].ascenderOffset;
                }

                accumulatedBounds.left = std::min(accumulatedBounds.left, left + glyphX);
                accumulatedBounds.top = std::min(accumulatedBounds.top, top + glyphY);
                accumulatedBounds.right = std::max(accumulatedBounds.right, right + glyphX);
                accumulatedBounds.bottom = std::max(accumulatedBounds.bottom, bottom + glyphY);
            }

            baselineX += glyphAdvance;
        }

        return accumulatedBounds;
    }

    inline bool _drawGlyphRun(IDWriteFactory4* dwriteFactory4, ID2D1DeviceContext* d2dRenderTarget, ID2D1DeviceContext4* d2dRenderTarget4, D2D_POINT_2F baselineOrigin, const DWRITE_GLYPH_RUN* glyphRun, ID2D1Brush* foregroundBrush) noexcept
    {
        static constexpr auto measuringMode = DWRITE_MEASURING_MODE_NATURAL;
        static constexpr auto formats =
            DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
            DWRITE_GLYPH_IMAGE_FORMATS_CFF |
            DWRITE_GLYPH_IMAGE_FORMATS_COLR |
            DWRITE_GLYPH_IMAGE_FORMATS_SVG |
            DWRITE_GLYPH_IMAGE_FORMATS_PNG |
            DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
            DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
            DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;

        wil::com_ptr<IDWriteColorGlyphRunEnumerator1> enumerator;

        // If ID2D1DeviceContext4 isn't supported, we'll exit early below.
        auto hr = DWRITE_E_NOCOLOR;

        if (d2dRenderTarget4)
        {
            D2D_MATRIX_3X2_F transform;
            d2dRenderTarget4->GetTransform(&transform);
            float dpiX, dpiY;
            d2dRenderTarget4->GetDpi(&dpiX, &dpiY);
            transform = transform * D2D1::Matrix3x2F::Scale(dpiX, dpiY);

            // Support for ID2D1DeviceContext4 implies support for IDWriteFactory4.
            // ID2D1DeviceContext4 is required for drawing below.
            hr = dwriteFactory4->TranslateColorGlyphRun(baselineOrigin, glyphRun, nullptr, formats, measuringMode, nullptr, 0, &enumerator);
        }

        if (hr == DWRITE_E_NOCOLOR)
        {
            d2dRenderTarget->DrawGlyphRun(baselineOrigin, glyphRun, foregroundBrush, measuringMode);
            return false;
        }

        THROW_IF_FAILED(hr);

        const auto previousAntialiasingMode = d2dRenderTarget4->GetTextAntialiasMode();
        d2dRenderTarget4->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto cleanup = wil::scope_exit([&]() {
            d2dRenderTarget4->SetTextAntialiasMode(previousAntialiasingMode);
        });

        wil::com_ptr<ID2D1SolidColorBrush> solidBrush;

        for (;;)
        {
            BOOL hasRun;
            THROW_IF_FAILED(enumerator->MoveNext(&hasRun));
            if (!hasRun)
            {
                break;
            }

            const DWRITE_COLOR_GLYPH_RUN1* colorGlyphRun;
            THROW_IF_FAILED(enumerator->GetCurrentRun(&colorGlyphRun));

            ID2D1Brush* runBrush;
            if (colorGlyphRun->paletteIndex == /*DWRITE_NO_PALETTE_INDEX*/ 0xffff)
            {
                runBrush = foregroundBrush;
            }
            else
            {
                if (!solidBrush)
                {
                    THROW_IF_FAILED(d2dRenderTarget4->CreateSolidColorBrush(colorGlyphRun->runColor, &solidBrush));
                }
                else
                {
                    solidBrush->SetColor(colorGlyphRun->runColor);
                }
                runBrush = solidBrush.get();
            }

            switch (colorGlyphRun->glyphImageFormat)
            {
            case DWRITE_GLYPH_IMAGE_FORMATS_NONE:
                break;
            case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
            case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
            case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
            case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
                d2dRenderTarget4->DrawColorBitmapGlyphRun(colorGlyphRun->glyphImageFormat, baselineOrigin, &colorGlyphRun->glyphRun, colorGlyphRun->measuringMode, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
                break;
            case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
                d2dRenderTarget4->DrawSvgGlyphRun(baselineOrigin, &colorGlyphRun->glyphRun, runBrush, nullptr, 0, colorGlyphRun->measuringMode);
                break;
            default:
                d2dRenderTarget4->DrawGlyphRun(baselineOrigin, &colorGlyphRun->glyphRun, colorGlyphRun->glyphRunDescription, runBrush, colorGlyphRun->measuringMode);
                break;
            }
        }

        return true;
    }

}
