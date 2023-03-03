#include "pch.h"
#include "BackendD3D11.h"

#include <til/hash.h>

#include <custom_shader_ps.h>
#include <custom_shader_vs.h>
#include <shader_ps.h>
#include <shader_vs.h>

#include "dwrite.h"

TIL_FAST_MATH_BEGIN

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4127)
#pragma warning(disable : 4189)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

BackendD3D11::GlyphCacheMap::~GlyphCacheMap()
{
    Clear();
}

BackendD3D11::GlyphCacheMap& BackendD3D11::GlyphCacheMap::operator=(GlyphCacheMap&& other) noexcept
{
    _map = std::exchange(other._map, {});
    _mapMask = std::exchange(other._mapMask, 0);
    _capacity = std::exchange(other._capacity, 0);
    _size = std::exchange(other._size, 0);
    return *this;
}

void BackendD3D11::GlyphCacheMap::Clear() noexcept
{
    if (_size)
    {
        for (auto& entry : _map)
        {
            if (entry.fontFace)
            {
                entry.fontFace->Release();
                entry.fontFace = nullptr;
            }
        }
    }
}

BackendD3D11::GlyphCacheEntry& BackendD3D11::GlyphCacheMap::FindOrInsert(IDWriteFontFace* fontFace, u16 glyphIndex, bool& inserted)
{
    const auto hash = _hash(fontFace, glyphIndex);

    for (auto i = hash;; ++i)
    {
        auto& entry = _map[i & _mapMask];
        if (entry.fontFace == fontFace && entry.glyphIndex == glyphIndex)
        {
            inserted = false;
            return entry;
        }
        if (!entry.fontFace)
        {
            inserted = true;
            return _insert(fontFace, glyphIndex, hash);
        }
    }
}

size_t BackendD3D11::GlyphCacheMap::_hash(IDWriteFontFace* fontFace, u16 glyphIndex) noexcept
{
    // MSVC 19.33 produces surprisingly good assembly for this without stack allocation.
    const uintptr_t data[2]{ std::bit_cast<uintptr_t>(fontFace), glyphIndex };
    return til::hash(&data[0], sizeof(data));
}

BackendD3D11::GlyphCacheEntry& BackendD3D11::GlyphCacheMap::_insert(IDWriteFontFace* fontFace, u16 glyphIndex, size_t hash)
{
    if (_size >= _capacity)
    {
        _bumpSize();
    }

    ++_size;

    for (auto i = hash;; ++i)
    {
        auto& entry = _map[i & _mapMask];
        if (!entry.fontFace)
        {
            entry.fontFace = fontFace;
            entry.glyphIndex = glyphIndex;
            entry.fontFace->AddRef();
            return entry;
        }
    }
}

void BackendD3D11::GlyphCacheMap::_bumpSize()
{
    const auto newMapSize = _map.size() * 2;
    const auto newMapMask = newMapSize - 1;
    FAIL_FAST_IF(newMapSize >= INT32_MAX); // overflow/truncation protection

    auto newMap = Buffer<GlyphCacheEntry>(newMapSize);

    for (const auto& entry : _map)
    {
        const auto newHash = _hash(entry.fontFace, entry.glyphIndex);
        newMap[newHash & newMapMask] = entry;
    }

    _map = std::move(newMap);
    _mapMask = newMapMask;
    _capacity = newMapSize / 2;
}

BackendD3D11::BackendD3D11(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
    THROW_IF_FAILED(_device->CreateVertexShader(&shader_vs[0], sizeof(shader_vs), nullptr, _vertexShader.addressof()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_ps[0], sizeof(shader_ps), nullptr, _pixelShader.addressof()));

    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(VSConstBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _vsConstantBuffer.addressof()));
    }

    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(PSConstBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _psConstantBuffer.addressof()));
    }

    {
        // The final step of the ClearType blending algorithm is a lerp() between the premultiplied alpha
        // background color and straight alpha foreground color given the 3 RGB weights in alphaCorrected:
        //   lerp(background, foreground, weights)
        // Which is equivalent to:
        //   background * (1 - weights) + foreground * weights
        //
        // This COULD be implemented using dual source color blending like so:
        //   .SrcBlend = D3D11_BLEND_SRC1_COLOR
        //   .DestBlend = D3D11_BLEND_INV_SRC1_COLOR
        //   .BlendOp = D3D11_BLEND_OP_ADD
        // Because:
        //   background * (1 - weights) + foreground * weights
        //       ^             ^        ^     ^           ^
        //      Dest     INV_SRC1_COLOR |    Src      SRC1_COLOR
        //                            OP_ADD
        //
        // BUT we need simultaneous support for regular "source over" alpha blending
        // (SHADING_TYPE_PASSTHROUGH)  like this:
        //   background * (1 - alpha) + foreground
        //
        // This is why we set:
        //   .SrcBlend = D3D11_BLEND_ONE
        //
        // --> We need to multiply the foreground with the weights ourselves.
        static constexpr D3D11_BLEND_DESC desc{
            .RenderTarget = { {
                .BlendEnable = TRUE,
                .SrcBlend = D3D11_BLEND_ONE,
                .DestBlend = D3D11_BLEND_INV_SRC1_COLOR,
                .BlendOp = D3D11_BLEND_OP_ADD,
                .SrcBlendAlpha = D3D11_BLEND_ONE,
                .DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA,
                .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
            } },
        };
        THROW_IF_FAILED(_device->CreateBlendState(&desc, _blendState.addressof()));
    }

    {
        static constexpr D3D11_BLEND_DESC desc{
            .RenderTarget = { {
                .BlendEnable = TRUE,
                .SrcBlend = D3D11_BLEND_ONE,
                .DestBlend = D3D11_BLEND_ONE,
                .BlendOp = D3D11_BLEND_OP_SUBTRACT,
                // In order for D3D to be okay with us using dual source blending in the shader, we need to use dual
                // source blending in the blend state. Alternatively we could write an extra shader for these cursors.
                .SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA,
                .DestBlendAlpha = D3D11_BLEND_ZERO,
                .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
            } },
        };
        THROW_IF_FAILED(_device->CreateBlendState(&desc, _blendStateInvert.addressof()));
    }

#ifndef NDEBUG
    _sourceDirectory = std::filesystem::path{ __FILE__ }.parent_path();
    _sourceCodeWatcher = wil::make_folder_change_reader_nothrow(_sourceDirectory.c_str(), false, wil::FolderChangeEvents::FileName | wil::FolderChangeEvents::LastWriteTime, [this](wil::FolderChangeEvent, PCWSTR path) {
        if (til::ends_with(path, L".hlsl"))
        {
            auto expected = INT64_MAX;
            const auto invalidationTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            _sourceCodeInvalidationTime.compare_exchange_strong(expected, invalidationTime.time_since_epoch().count(), std::memory_order_relaxed);
        }
    });
#endif
}

void BackendD3D11::_recreateBackgroundBitmapSamplerState(const RenderingPayload& p)
{
    const auto color = colorFromU32Premultiply<DXGI_RGBA>(p.s->misc->backgroundColor);
    const D3D11_SAMPLER_DESC desc{
        .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
        .AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
        .AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
        .MipLODBias = 0.0f,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .BorderColor = { color.r, color.g, color.b, color.a },
        .MinLOD = -FLT_MAX,
        .MaxLOD = FLT_MAX,
    };
    THROW_IF_FAILED(_device->CreateSamplerState(&desc, _backgroundBitmapSamplerState.put()));
}

void BackendD3D11::Render(const RenderingPayload& p)
{
    _debugUpdateShaders();

    if (_generation != p.s.generation())
    {
        _swapChainManager.UpdateSwapChainSettings(
            p,
            _device.get(),
            [this]() {
                _renderTargetView.reset();
                _deviceContext->ClearState();
            },
            [this]() {
                _renderTargetView.reset();
                _deviceContext->ClearState();
                _deviceContext->Flush();
            });

        if (!_renderTargetView)
        {
            const auto buffer = _swapChainManager.GetBuffer();
            THROW_IF_FAILED(_device->CreateRenderTargetView(buffer.get(), nullptr, _renderTargetView.put()));
        }

        const auto fontChanged = _fontGeneration != p.s->font.generation();
        const auto miscChanged = _miscGeneration != p.s->misc.generation();
        const auto targetSizeChanged = _targetSize != p.s->targetSize;
        const auto cellCountChanged = _cellCount != p.s->cellCount;

        if (fontChanged)
        {
            DWrite_GetRenderParams(p.dwriteFactory.get(), &_gamma, &_cleartypeEnhancedContrast, &_grayscaleEnhancedContrast, _textRenderingParams.put());
            _resetGlyphAtlas = true;

            if (_d2dRenderTarget)
            {
                _d2dRenderTargetUpdateFontSettings(p);
            }
        }

        if (miscChanged)
        {
            _recreateBackgroundBitmapSamplerState(p);
            _recreateCustomShader(p);
        }

        if (cellCountChanged)
        {
            _recreateBackgroundColorBitmap(p);
        }

        if (targetSizeChanged || miscChanged)
        {
            _recreateCustomOffscreenTexture(p);
        }

        if (targetSizeChanged || fontChanged)
        {
            _recreateConstBuffer(p);
        }

        _generation = p.s.generation();
        _fontGeneration = p.s->font.generation();
        _miscGeneration = p.s->misc.generation();
        _cellCount = p.s->cellCount;
    }

    _instancesSize = 0;

    {
        // IA: Input Assembler
        _deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _deviceContext->IASetIndexBuffer(_indexBuffer.get(), _indicesFormat, 0);

        // VS: Vertex Shader
        _deviceContext->VSSetShader(_vertexShader.get(), nullptr, 0);
        _deviceContext->VSSetConstantBuffers(0, 1, _vsConstantBuffer.addressof());
        _deviceContext->VSSetShaderResources(0, 1, _instanceBufferView.addressof());

        // RS: Rasterizer Stage
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<f32>(p.s->targetSize.x);
        viewport.Height = static_cast<f32>(p.s->targetSize.y);
        _deviceContext->RSSetViewports(1, &viewport);

        // PS: Pixel Shader
        ID3D11ShaderResourceView* const resources[]{ _backgroundBitmapView.get(), _glyphAtlasView.get() };
        _deviceContext->PSSetShader(_pixelShader.get(), nullptr, 0);
        _deviceContext->PSSetConstantBuffers(0, 1, _psConstantBuffer.addressof());
        _deviceContext->PSSetSamplers(0, 1, _backgroundBitmapSamplerState.addressof());
        _deviceContext->PSSetShaderResources(0, 2, &resources[0]);

        // OM: Output Merger
        _deviceContext->OMSetBlendState(_blendState.get(), nullptr, 0xffffffff);
        _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);
    }

    // Background
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_backgroundBitmap.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        auto data = static_cast<char*>(mapped.pData);
        for (size_t i = 0; i < p.s->cellCount.y; ++i)
        {
            memcpy(data, p.backgroundBitmap.data() + i * p.s->cellCount.x, p.s->cellCount.x * sizeof(u32));
            data += mapped.RowPitch;
        }
        _deviceContext->Unmap(_backgroundBitmap.get(), 0);
    }
    {
        const auto targetWidth = static_cast<f32>(p.s->targetSize.x);
        const auto targetHeight = static_cast<f32>(p.s->targetSize.y);
        const auto contentWidth = static_cast<f32>(p.s->cellCount.x * p.s->font->cellSize.x);
        const auto contentHeight = static_cast<f32>(p.s->cellCount.y * p.s->font->cellSize.y);
        _appendQuad(
            { 0.0f, 0.0f, targetWidth, targetHeight },
            { 0.0f, 0.0f, targetWidth / contentWidth, targetHeight / contentHeight },
            0,
            ShadingType::Background);
    }

    // Text
    {
        if (_resetGlyphAtlas)
        {
            _resetAtlasAndBeginDraw(p);
            _resetGlyphAtlas = false;
        }

        auto baselineY = p.s->font->baselineInDIP;
        for (const auto& row : p.rows)
        {
            f32 cumulativeAdvance = 0;

            for (const auto& m : row.mappings)
            {
                for (auto i = m.glyphsFrom; i < m.glyphsTo; ++i)
                {
                    bool inserted = false;
                    auto& entry = _glyphCache.FindOrInsert(m.fontFace.get(), row.glyphIndices[i], inserted);
                    if (inserted)
                    {
                        _beginDrawing();

                        if (!_drawGlyph(p, entry, m.fontEmSize))
                        {
                            _endDrawing();
                            _flushRects(p);
                            _resetAtlasAndBeginDraw(p);
                            --i;
                            continue;
                        }
                    }

                    if (entry.shadingType)
                    {
                        const auto x = (cumulativeAdvance + row.glyphOffsets[i].advanceOffset) * p.d.font.pixelPerDIP + entry.offset.x;
                        const auto y = (baselineY - row.glyphOffsets[i].ascenderOffset) * p.d.font.pixelPerDIP + entry.offset.y;
                        const auto w = entry.texcoord.z - entry.texcoord.x;
                        const auto h = entry.texcoord.w - entry.texcoord.y;
                        _appendQuad({ x, y, x + w, y + h }, entry.texcoord, row.colors[i], static_cast<ShadingType>(entry.shadingType));
                    }

                    cumulativeAdvance += row.glyphAdvances[i];
                }
            }

            baselineY += p.d.font.cellSizeDIP.y;
        }

        _endDrawing();
    }

    // Gridlines
    {
        size_t y = 0;
        for (const auto& row : p.rows)
        {
            if (!row.gridLineRanges.empty())
            {
                _drawGridlines(p, row, y);
            }
            y++;
        }
    }

    // Cursor
    if (p.cursorRect.non_empty())
    {
        const auto color = p.s->cursor->cursorColor;

        // Cursors that are 0xffffffff invert the color they're on. The problem is that the inversion
        // of a pure gray background color (0x7f) is also gray and so the cursor would appear invisible.
        //
        // An imperfect but simple solution is to instead XOR the color with 0xc0, flipping the top two bits.
        // This preserves the lower 6 bits and so gray (0x7f) gets inverted to light gray (0xbf) instead.
        // Normally this would be super trivial to do using D3D11_LOGIC_OP_XOR, but this would break
        // the lightness adjustment that the ClearType/Grayscale AA algorithms use. Additionally,
        // in case of ClearType specifically, this would break the red/blue shift on the edges.
        //
        // The alternative approach chosen here does a regular linear inversion (1 - RGB), but checks the
        // background color of all cells the cursor is on and darkens it if any of them could be considered "gray".
        if (color == 0xffffffff)
        {
            _flushRects(p);
            _deviceContext->OMSetBlendState(_blendStateInvert.get(), nullptr, 0xffffffff);

            const auto y = p.cursorRect.top * p.s->cellCount.x;
            const auto left = p.cursorRect.left;
            const auto right = p.cursorRect.right;
            u32 lastColor = 0;

            for (auto x = left; x < right; ++x)
            {
                const auto bgReg = p.backgroundBitmap[y + x] | 0xff000000;

                // If the current background color matches the previous one, we can just extend the previous quad to the right.
                if (bgReg == lastColor)
                {
                    _getLastQuad().position.z = static_cast<f32>(p.s->font->cellSize.x * (x + 1));
                }
                else
                {
                    const auto bgInv = ~bgReg;
                    // gte = greater than or equal, lte = lower than or equal
                    const auto gte70 = ((bgReg & 0x7f7f7f) + 0x101010 | bgReg) & 0x808080;
                    const auto lte8f = ((bgInv & 0x7f7f7f) + 0x101010 | bgInv) & 0x808080;
                    // isGray will now be true if all 3 channels of the color are in the range [0x70,0x8f].
                    const auto isGray = (gte70 & lte8f) == 0x808080;
                    // The shader will invert the color by calculating `cursorColor - rendertTargetColor`, where
                    // `rendertTargetColor` is the color already in the render target view (= all the text, etc.).
                    // To avoid the issue mentioned above, we want to darken the color by -32 in each channel.
                    // To do so we can just pass it a corresponding `cursorColor` in the range [0xc0,0xff].
                    // Since the [0xc0,0xff] range is twice as large as [0x70,0x8f], we multiply by 2.
                    const auto cursorColor = isGray ? 0xffc0c0c0 + 2 * (bgReg - 0xff707070) : 0xffffffff;

                    f32x4s rect{
                        static_cast<f32>(p.s->font->cellSize.x * x),
                        static_cast<f32>(p.s->font->cellSize.y * p.cursorRect.top),
                        static_cast<f32>(p.s->font->cellSize.x * (x + 1)),
                        static_cast<f32>(p.s->font->cellSize.y * p.cursorRect.bottom),
                    };

                    switch (static_cast<CursorType>(p.s->cursor->cursorType))
                    {
                    case CursorType::Legacy:
                        rect.y = rect.w - (rect.w - rect.y) * static_cast<float>(p.s->cursor->heightPercentage) / 100.0f;
                        _appendQuad(rect, cursorColor, ShadingType::SolidFill);
                        break;
                    case CursorType::VerticalBar:
                        rect.z = rect.x + p.s->font->thinLineWidth;
                        _appendQuad(rect, cursorColor, ShadingType::SolidFill);
                        break;
                    case CursorType::Underscore:
                        rect.y += p.s->font->underlinePos;
                        rect.w = rect.y + p.s->font->underlineWidth;
                        _appendQuad(rect, cursorColor, ShadingType::SolidFill);
                        break;
                    case CursorType::EmptyBox:
                        break;
                    case CursorType::FullBox:
                        _appendQuad(rect, cursorColor, ShadingType::SolidFill);
                        break;
                    case CursorType::DoubleUnderscore:
                    {
                        auto rect2 = rect;
                        rect.y += p.s->font->doubleUnderlinePos.x;
                        rect.w = rect.y + p.s->font->thinLineWidth;
                        _appendQuad(rect, cursorColor, ShadingType::SolidFill);
                        rect2.y += p.s->font->doubleUnderlinePos.y;
                        rect2.w = rect.y + p.s->font->thinLineWidth;
                        _appendQuad(rect2, cursorColor, ShadingType::SolidFill);
                        break;
                    }
                    default:
                        break;
                    }

                    lastColor = bgReg;
                }
            }

            _flushRects(p);
            _deviceContext->OMSetBlendState(_blendState.get(), nullptr, 0xffffffff);
        }
        else
        {
            const f32x4s rect{
                static_cast<f32>(p.s->font->cellSize.x * p.cursorRect.left),
                static_cast<f32>(p.s->font->cellSize.y * p.cursorRect.top),
                static_cast<f32>(p.s->font->cellSize.x * p.cursorRect.right),
                static_cast<f32>(p.s->font->cellSize.y * p.cursorRect.bottom),
            };
            _appendQuad(rect, color, ShadingType::SolidFill);
        }
    }

    // Selection
    {
        size_t y = 0;
        u16 lastFrom = 0;
        u16 lastTo = 0;

        for (const auto& row : p.rows)
        {
            if (row.selectionTo > row.selectionFrom)
            {
                // If the current selection line matches the previous one, we can just extend the previous quad downwards.
                // The way this is implemented isn't very smart, but we also don't have very many rows to iterate through.
                if (row.selectionFrom == lastFrom && row.selectionTo == lastTo)
                {
                    _getLastQuad().position.w = static_cast<f32>(p.s->font->cellSize.y * (y + 1));
                }
                else
                {
                    _appendQuad(
                        {
                            static_cast<f32>(p.s->font->cellSize.x * row.selectionFrom),
                            static_cast<f32>(p.s->font->cellSize.y * y),
                            static_cast<f32>(p.s->font->cellSize.x * row.selectionTo),
                            static_cast<f32>(p.s->font->cellSize.y * (y + 1)),
                        },
                        p.s->misc->selectionColor,
                        ShadingType::SolidFill);
                    lastFrom = row.selectionFrom;
                    lastTo = row.selectionTo;
                }
            }

            y++;
        }
    }

    _flushRects(p);
    _swapChainManager.Present(p);
}

bool BackendD3D11::RequiresContinuousRedraw() noexcept
{
    return _requiresContinuousRedraw;
}

void BackendD3D11::WaitUntilCanRender() noexcept
{
    _swapChainManager.WaitUntilCanRender();
}

void BackendD3D11::_debugUpdateShaders()
try
{
#ifndef NDEBUG
    const auto invalidationTime = _sourceCodeInvalidationTime.load(std::memory_order_relaxed);

    if (invalidationTime == INT64_MAX || invalidationTime > std::chrono::steady_clock::now().time_since_epoch().count())
    {
        return;
    }

    _sourceCodeInvalidationTime.store(INT64_MAX, std::memory_order_relaxed);

    static const auto compile = [](const std::filesystem::path& path, const char* target) {
        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ path.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        if (error)
        {
            std::thread t{ [error = std::move(error)]() noexcept {
                MessageBoxA(nullptr, static_cast<const char*>(error->GetBufferPointer()), "Compilation error", MB_ICONERROR | MB_OK);
            } };
            t.detach();
        }

        THROW_IF_FAILED(hr);
        return blob;
    };

    struct FileVS
    {
        std::wstring_view filename;
        wil::com_ptr<ID3D11VertexShader> BackendD3D11::*target;
    };
    struct FilePS
    {
        std::wstring_view filename;
        wil::com_ptr<ID3D11PixelShader> BackendD3D11::*target;
    };

    static std::array filesVS{
        FileVS{ L"shader_vs.hlsl", &BackendD3D11::_vertexShader },
    };
    static std::array filesPS{
        FilePS{ L"shader_text_cleartype_ps.hlsl", &BackendD3D11::_pixelShader },
        FilePS{ L"shader_text_grayscale_ps.hlsl", &BackendD3D11::_pixelShader },
    };

    std::array<wil::com_ptr<ID3D11VertexShader>, filesVS.size()> compiledVS;
    std::array<wil::com_ptr<ID3D11PixelShader>, filesPS.size()> compiledPS;

    // Compile our files before moving them into `this` below to ensure we're
    // always in a consistent state where all shaders are seemingly valid.
    for (size_t i = 0; i < filesVS.size(); ++i)
    {
        const auto blob = compile(_sourceDirectory / filesVS[i].filename, "vs_4_0");
        THROW_IF_FAILED(_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledVS[i].addressof()));
    }
    for (size_t i = 0; i < filesPS.size(); ++i)
    {
        const auto blob = compile(_sourceDirectory / filesPS[i].filename, "ps_4_0");
        THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledPS[i].addressof()));
    }

    for (size_t i = 0; i < filesVS.size(); ++i)
    {
        this->*filesVS[i].target = std::move(compiledVS[i]);
    }
    for (size_t i = 0; i < filesPS.size(); ++i)
    {
        this->*filesPS[i].target = std::move(compiledPS[i]);
    }
#endif
}
CATCH_LOG()

void BackendD3D11::_recreateCustomShader(const RenderingPayload& p)
{
    _customOffscreenTexture.reset();
    _customOffscreenTextureView.reset();
    _customOffscreenTextureTargetView.reset();
    _customVertexShader.reset();
    _customPixelShader.reset();
    _customShaderConstantBuffer.reset();
    _customShaderSamplerState.reset();
    _requiresContinuousRedraw = false;

    if (!p.s->misc->customPixelShaderPath.empty())
    {
        const char* target = nullptr;
        switch (_device->GetFeatureLevel())
        {
        case D3D_FEATURE_LEVEL_10_0:
            target = "ps_4_0";
            break;
        case D3D_FEATURE_LEVEL_10_1:
            target = "ps_4_1";
            break;
        default:
            target = "ps_5_0";
            break;
        }

        static constexpr auto flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR
#ifdef NDEBUG
            | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
            // Only enable strictness and warnings in DEBUG mode
            //  as these settings makes it very difficult to develop
            //  shaders as windows terminal is not telling the user
            //  what's wrong, windows terminal just fails.
            //  Keep it in DEBUG mode to catch errors in shaders
            //  shipped with windows terminal
            | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ p.s->misc->customPixelShaderPath.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ flags,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        // Unless we can determine otherwise, assume this shader requires evaluation every frame
        _requiresContinuousRedraw = true;

        if (SUCCEEDED(hr))
        {
            THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _customPixelShader.put()));

            // Try to determine whether the shader uses the Time variable
            wil::com_ptr<ID3D11ShaderReflection> reflector;
            if (SUCCEEDED_LOG(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(reflector.put()))))
            {
                if (ID3D11ShaderReflectionConstantBuffer* constantBufferReflector = reflector->GetConstantBufferByIndex(0)) // shader buffer
                {
                    if (ID3D11ShaderReflectionVariable* variableReflector = constantBufferReflector->GetVariableByIndex(0)) // time
                    {
                        D3D11_SHADER_VARIABLE_DESC variableDescriptor;
                        if (SUCCEEDED_LOG(variableReflector->GetDesc(&variableDescriptor)))
                        {
                            // only if time is used
                            _requiresContinuousRedraw = WI_IsFlagSet(variableDescriptor.uFlags, D3D_SVF_USED);
                        }
                    }
                }
            }
        }
        else
        {
            if (error)
            {
                LOG_HR_MSG(hr, "%*hs", error->GetBufferSize(), error->GetBufferPointer());
            }
            else
            {
                LOG_HR(hr);
            }
            if (p.warningCallback)
            {
                p.warningCallback(D2DERR_SHADER_COMPILE_FAILED);
            }
        }
    }
    else if (p.s->misc->useRetroTerminalEffect)
    {
        THROW_IF_FAILED(_device->CreatePixelShader(&custom_shader_ps[0], sizeof(custom_shader_ps), nullptr, _customPixelShader.put()));
        // We know the built-in retro shader doesn't require continuous redraw.
        _requiresContinuousRedraw = false;
    }

    if (_customPixelShader)
    {
        THROW_IF_FAILED(_device->CreateVertexShader(&custom_shader_vs[0], sizeof(custom_shader_vs), nullptr, _customVertexShader.put()));

        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(CustomConstBuffer);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _customShaderConstantBuffer.put()));
        }

        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.MaxAnisotropy = 1;
            desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            THROW_IF_FAILED(_device->CreateSamplerState(&desc, _customShaderSamplerState.put()));
        }

        _customShaderStartTime = std::chrono::steady_clock::now();
    }
}

void BackendD3D11::_recreateCustomOffscreenTexture(const RenderingPayload& p)
{
    if (!p.s->misc->customPixelShaderPath.empty())
    {
        // Avoid memory usage spikes by releasing memory first.
        _customOffscreenTexture.reset();
        _customOffscreenTextureView.reset();
        _customOffscreenTextureTargetView.reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = p.s->targetSize.x;
        desc.Height = p.s->targetSize.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _customOffscreenTexture.addressof()));
        THROW_IF_FAILED(_device->CreateShaderResourceView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureView.addressof()));
        THROW_IF_FAILED(_device->CreateRenderTargetView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureTargetView.addressof()));
    }
}

void BackendD3D11::_recreateBackgroundColorBitmap(const RenderingPayload& p)
{
    // Avoid memory usage spikes by releasing memory first.
    _backgroundBitmap.reset();
    _backgroundBitmapView.reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = p.s->cellCount.x;
    desc.Height = p.s->cellCount.y;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _backgroundBitmap.addressof()));
    THROW_IF_FAILED(_device->CreateShaderResourceView(_backgroundBitmap.get(), nullptr, _backgroundBitmapView.addressof()));
}

void BackendD3D11::_recreateConstBuffer(const RenderingPayload& p)
{
    {
        VSConstBuffer data;
        data.positionScale = { 2.0f / p.s->targetSize.x, -2.0f / p.s->targetSize.y };
        _deviceContext->UpdateSubresource(_vsConstantBuffer.get(), 0, nullptr, &data, 0, 0);
    }
    {
        PSConstBuffer data;
        DWrite_GetGammaRatios(_gamma, data.gammaRatios);
        data.enhancedContrast = p.s->font->antialiasingMode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE ? _cleartypeEnhancedContrast : _grayscaleEnhancedContrast;
        data.dashedLineLength = p.s->font->underlineWidth * 3.0f;
        _deviceContext->UpdateSubresource(_psConstantBuffer.get(), 0, nullptr, &data, 0, 0);
    }
}

void BackendD3D11::_d2dRenderTargetUpdateFontSettings(const RenderingPayload& p) const
{
    _d2dRenderTarget->SetDpi(p.s->font->dpi, p.s->font->dpi);
    _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->font->antialiasingMode));
}

void BackendD3D11::_beginDrawing()
{
    if (!_d2dBeganDrawing)
    {
        _d2dRenderTarget->BeginDraw();
        _d2dBeganDrawing = true;
    }
}

void BackendD3D11::_endDrawing()
{
    if (_d2dBeganDrawing)
    {
        THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
        _d2dBeganDrawing = false;
    }
}

void BackendD3D11::_resetAtlasAndBeginDraw(const RenderingPayload& p)
{
    // This block of code calculates the size of a power-of-2 texture that has an area larger than the targetSize
    // of the swap chain. In other words for a 985x1946 pixel swap chain (area = 1916810) it would result in a u/v
    // of 2048x1024 (area = 2097152). This has 2 benefits: GPUs like power-of-2 textures and it ensures that we don't
    // resize the texture every time you resize the window by a pixel. Instead it only grows/shrinks by a factor of 2.
    auto area = static_cast<u32>(p.s->targetSize.x) * static_cast<u32>(p.s->targetSize.y);
    // The index returned by _BitScanReverse is undefined when the input is 0. We can simultaneously
    // guard against this and avoid unreasonably small textures, by clamping the min. texture size.
    area = std::max(uint32_t{ 256 * 256 }, area);
    unsigned long index;
    _BitScanReverse(&index, area - 1);
    const auto u = ::base::saturated_cast<u16>(1u << ((index + 2) / 2));
    const auto v = ::base::saturated_cast<u16>(1u << ((index + 1) / 2));

    if (u != _rectPacker.width || v != _rectPacker.height)
    {
        _d2dRenderTarget.reset();
        _d2dRenderTarget4.reset();
        _glyphAtlas.reset();
        _glyphAtlasView.reset();

        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = u;
            desc.Height = v;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = { 1, 0 };
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _glyphAtlas.addressof()));
            THROW_IF_FAILED(_device->CreateShaderResourceView(_glyphAtlas.get(), nullptr, _glyphAtlasView.addressof()));
        }

        {
            const auto surface = _glyphAtlas.query<IDXGISurface>();

            D2D1_RENDER_TARGET_PROPERTIES props{};
            props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            wil::com_ptr<ID2D1RenderTarget> renderTarget;
            THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
            _d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
            _d2dRenderTarget4 = renderTarget.query<ID2D1DeviceContext4>();

            // We don't really use D2D for anything except DWrite, but it
            // can't hurt to ensure that everything it does is pixel aligned.
            _d2dRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            // Ensure that D2D uses the exact same gamma as our shader uses.
            _d2dRenderTarget->SetTextRenderingParams(_textRenderingParams.get());

            _d2dRenderTargetUpdateFontSettings(p);
        }

        {
            static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
            THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
            _brushColor = 0xffffffff;
        }

        ID3D11ShaderResourceView* const resources[]{ _backgroundBitmapView.get(), _glyphAtlasView.get() };
        _deviceContext->PSSetShaderResources(0, 2, &resources[0]);
    }

    _glyphCache.Clear();
    _rectPackerData = Buffer<stbrp_node>{ u };
    stbrp_init_target(&_rectPacker, u, v, _rectPackerData.data(), gsl::narrow_cast<int>(_rectPackerData.size()));

    _beginDrawing();
    _d2dRenderTarget->Clear();
}

void BackendD3D11::_appendQuad(f32x4s position, u32 color, ShadingType shadingType)
{
    _appendQuad(position, {}, color, shadingType);
}

void BackendD3D11::_appendQuad(f32x4s position, f32x4s texcoord, u32 color, ShadingType shadingType)
{
    if (_instancesSize >= _instances.size())
    {
        _bumpInstancesSize();
    }

    _instances[_instancesSize++] = QuadInstance{ position, texcoord, color, static_cast<u32>(shadingType) };
}

BackendD3D11::QuadInstance& BackendD3D11::_getLastQuad() noexcept
{
    assert(_instancesSize != 0);
    return _instances[_instancesSize - 1];
}

void BackendD3D11::_bumpInstancesSize()
{
    _instances = Buffer<QuadInstance>{ std::max<size_t>(1024, _instances.size() << 1) };
}

void BackendD3D11::_flushRects(const RenderingPayload& p)
{
    if (!_instancesSize)
    {
        return;
    }

    if (_instancesSize > _instanceBufferSize)
    {
        _recreateInstanceBuffers(p);
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, _instances.data(), _instancesSize * sizeof(QuadInstance));
        _deviceContext->Unmap(_instanceBuffer.get(), 0);
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_indexBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));

        if (_indicesFormat == DXGI_FORMAT_R16_UINT)
        {
            auto data = static_cast<u16*>(mapped.pData);
            for (u16 vertices = gsl::narrow_cast<u16>(4 * _instancesSize), off = 0; off < vertices; off += 4)
            {
                *data++ = off + 0;
                *data++ = off + 1;
                *data++ = off + 2;
                *data++ = off + 3;
                *data++ = off + 2;
                *data++ = off + 1;
            }
        }
        else
        {
            assert(_indicesFormat == DXGI_FORMAT_R32_UINT);
            auto data = static_cast<u32*>(mapped.pData);
            for (u32 vertices = gsl::narrow_cast<u32>(4 * _instancesSize), off = 0; off < vertices; off += 4)
            {
                *data++ = off + 0;
                *data++ = off + 1;
                *data++ = off + 2;
                *data++ = off + 3;
                *data++ = off + 2;
                *data++ = off + 1;
            }
        }

        _deviceContext->Unmap(_indexBuffer.get(), 0);
    }

    // I found 4 approaches to drawing lots of quads quickly.
    // They can often be found in discussions about "particle" or "point sprite" rendering in game development.
    // * Compute Shader: My understanding is that at the time of writing games are moving over to bucketing
    //   particles into "tiles" on the screen and drawing them with a compute shader. While this improves
    //   performance, it doesn't mix well with our goal of allowing arbitrary overlaps between glyphs.
    //   Additionally none of the next 3 approaches use any significant amount of GPU time in the first place.
    // * Geometry Shader: Geometry shaders can generate vertices on the fly, which would neatly replace
    //   our need for an index buffer. The reason this wasn't chosen is the same as for the next point.
    // * DrawInstanced: On my own hardware (Nvidia RTX 4090) this seems to perform ~50% better than the final point,
    //   but with no significant difference in power draw. However the popular "Vertex Shader Tricks" talk from
    //   Bill Bilodeau at GDC 2014 suggests that this at least doesn't apply to 2014ish hardware, which supposedly
    //   performs poorly with very small, instanced meshes. Furthermore, public feedback suggests that we still
    //   have a lot of users with older hardware, so I've chosen the following approach, suggested in the talk.
    // * DrawIndexed: This works about the same as DrawInstanced, but instead of using D3D11_INPUT_PER_INSTANCE_DATA,
    //   it uses a SRV (shader resource view) for instance data and maps each SV_VertexID to a SRV slot.
    _deviceContext->DrawIndexed(gsl::narrow_cast<UINT>(6 * _instancesSize), 0, 0);

    _instancesSize = 0;
}

void BackendD3D11::_recreateInstanceBuffers(const RenderingPayload& p)
{
    static constexpr size_t R16max = 1 << 16;
    // While the viewport size of the terminal is probably a good initial estimate for the amount of instances we'll see,
    // I feel like we should ensure that the estimate doesn't exceed the limit for a DXGI_FORMAT_R16_UINT index buffer.
    const auto estimatedInstances = std::min(R16max / 4, static_cast<size_t>(p.s->cellCount.x) * p.s->cellCount.y);
    const auto minSize = std::max(_instancesSize, estimatedInstances);
    // std::bit_ceil will result in a nice exponential growth curve. I don't know exactly how structured buffers are treated
    // by various drivers, but I'm assuming that they prefer buffer sizes that are close to power-of-2 sizes as well.
    const auto newInstancesSize = std::bit_ceil(minSize * sizeof(QuadInstance)) / sizeof(QuadInstance);
    const auto newIndicesSize = newInstancesSize * 6;
    const auto vertices = newInstancesSize * 4;
    const auto indicesFormat = vertices <= R16max ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    const auto indexSize = vertices <= R16max ? sizeof(u16) : sizeof(u32);

    _indexBuffer.reset();
    _instanceBuffer.reset();
    _instanceBufferView.reset();

    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = gsl::narrow<UINT>(newIndicesSize * indexSize);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _indexBuffer.addressof()));
    }

    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = gsl::narrow<UINT>(newInstancesSize * sizeof(QuadInstance));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(QuadInstance);
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _instanceBuffer.addressof()));
        THROW_IF_FAILED(_device->CreateShaderResourceView(_instanceBuffer.get(), nullptr, _instanceBufferView.addressof()));
    }

    _deviceContext->IASetIndexBuffer(_indexBuffer.get(), indicesFormat, 0);
    _deviceContext->VSSetShaderResources(0, 1, _instanceBufferView.addressof());

    _instanceBufferSize = newInstancesSize;
    _indicesFormat = indicesFormat;
}

bool BackendD3D11::_drawGlyph(const RenderingPayload& p, GlyphCacheEntry& entry, f32 fontEmSize)
{
    DWRITE_GLYPH_RUN glyphRun{};
    glyphRun.fontFace = entry.fontFace;
    glyphRun.fontEmSize = fontEmSize;
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &entry.glyphIndex;

    auto box = getGlyphRunBlackBox(glyphRun, 0, 0);
    if (box.left >= box.right || box.top >= box.bottom)
    {
        entry = {};
        return true;
    }

    box.left = floorf(box.left * p.d.font.pixelPerDIP) - 1.0f;
    box.top = floorf(box.top * p.d.font.pixelPerDIP) - 1.0f;
    box.right = ceilf(box.right * p.d.font.pixelPerDIP) + 1.0f;
    box.bottom = ceilf(box.bottom * p.d.font.pixelPerDIP) + 1.0f;

    stbrp_rect rect{};
    rect.w = gsl::narrow_cast<int>(box.right - box.left);
    rect.h = gsl::narrow_cast<int>(box.bottom - box.top);
    if (!stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        return false;
    }

    const D2D1_POINT_2F baseline{
        (rect.x - box.left) * p.d.font.dipPerPixel,
        (rect.y - box.top) * p.d.font.dipPerPixel,
    };
    const auto colorGlyph = _drawGlyphRun(p.dwriteFactory4.get(), _d2dRenderTarget.get(), _d2dRenderTarget4.get(), baseline, &glyphRun, _brush.get());

    entry.shadingType = static_cast<u16>(colorGlyph ? ShadingType::Passthrough : (p.s->font->antialiasingMode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE ? ShadingType::TextClearType : ShadingType::TextGrayscale));
    entry.offset.x = box.left;
    entry.offset.y = box.top;
    entry.texcoord.x = static_cast<f32>(rect.x);
    entry.texcoord.y = static_cast<f32>(rect.y);
    entry.texcoord.z = static_cast<f32>(rect.x + rect.w);
    entry.texcoord.w = static_cast<f32>(rect.y + rect.h);
    return true;
}

void BackendD3D11::_drawGridlines(const RenderingPayload& p, const ShapedRow& row, size_t y)
{
    for (const auto& r : row.gridLineRanges)
    {
        // AtlasEngine.cpp shouldn't add any gridlines if they don't do anything.
        assert(r.lines.any());

        const auto top = static_cast<f32>(p.s->font->cellSize.y * y);
        const auto bottom = static_cast<f32>(p.s->font->cellSize.y * (y + 1));
        auto left = static_cast<f32>(p.s->font->cellSize.x * r.from);
        auto right = static_cast<f32>(p.s->font->cellSize.x * r.to);

        if (r.lines.test(GridLines::Left))
        {
            for (; left < right; left += p.s->font->cellSize.x)
            {
                _appendQuad(
                    {
                        left,
                        top,
                        left + p.s->font->thinLineWidth,
                        bottom,
                    },
                    r.color,
                    ShadingType::SolidFill);
            }
        }
        if (r.lines.test(GridLines::Top))
        {
            _appendQuad(
                {
                    left,
                    top,
                    right,
                    top + static_cast<f32>(p.s->font->thinLineWidth),
                },
                r.color,
                ShadingType::SolidFill);
        }
        if (r.lines.test(GridLines::Right))
        {
            for (; right > left; right -= p.s->font->cellSize.x)
            {
                _appendQuad(
                    {
                        right - p.s->font->thinLineWidth,
                        top,
                        right,
                        bottom,
                    },
                    r.color,
                    ShadingType::SolidFill);
            }
        }
        if (r.lines.test(GridLines::Bottom))
        {
            _appendQuad(
                {
                    left,
                    bottom - p.s->font->thinLineWidth,
                    right,
                    bottom,
                },
                r.color,
                ShadingType::SolidFill);
        }
        if (r.lines.test(GridLines::Underline))
        {
            _appendQuad(
                {
                    left,
                    top + p.s->font->underlinePos,
                    right,
                    top + p.s->font->underlinePos + p.s->font->underlineWidth,
                },
                r.color,
                ShadingType::SolidFill);
        }
        if (r.lines.test(GridLines::HyperlinkUnderline))
        {
            _appendQuad(
                {
                    left,
                    top + p.s->font->underlinePos,
                    right,
                    top + p.s->font->underlinePos + p.s->font->underlineWidth,
                },
                r.color,
                ShadingType::DashedLine);
        }
        if (r.lines.test(GridLines::DoubleUnderline))
        {
            _appendQuad(
                {
                    left,
                    top + p.s->font->doubleUnderlinePos.x,
                    right,
                    top + p.s->font->doubleUnderlinePos.x + p.s->font->thinLineWidth,
                },
                r.color,
                ShadingType::SolidFill);

            _appendQuad(
                {
                    left,
                    top + p.s->font->doubleUnderlinePos.y,
                    right,
                    top + p.s->font->doubleUnderlinePos.y + p.s->font->thinLineWidth,
                },
                r.color,
                ShadingType::SolidFill);
        }
        if (r.lines.test(GridLines::Strikethrough))
        {
            _appendQuad(
                {
                    left,
                    top + p.s->font->strikethroughPos,
                    right,
                    top + p.s->font->strikethroughPos + p.s->font->strikethroughWidth,
                },
                r.color,
                ShadingType::SolidFill);
        }
    }
}

TIL_FAST_MATH_END
