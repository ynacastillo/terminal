#pragma once

#include <d3d11_2.h>

#include <stb_rect_pack.h>
#include <til/hash.h>

#include "Backend.h"

namespace Microsoft::Console::Render::Atlas
{
    struct BackendD3D11 : IBackend
    {
        BackendD3D11(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext);

        void Render(const RenderingPayload& payload) override;
        bool RequiresContinuousRedraw() noexcept override;
        void WaitUntilCanRender() noexcept override;

    private:
        // NOTE: D3D constant buffers sizes must be a multiple of 16 bytes.
        struct alignas(16) ConstBuffer
        {
            // WARNING: Modify this carefully after understanding how HLSL struct packing works.
            // The gist is:
            // * Minimum alignment is 4 bytes (like `#pragma pack 4`)
            // * Members cannot straddle 16 byte boundaries
            //   This means a structure like {u32; u32; u32; u32x2} would require
            //   padding so that it is {u32; u32; u32; <4 byte padding>; u32x2}.
            // * bool will probably not work the way you want it to,
            //   because HLSL uses 32-bit bools and C++ doesn't.
            alignas(sizeof(f32x2)) f32x2 positionScale;
            alignas(sizeof(f32)) f32 grayscaleEnhancedContrast = 0;
            alignas(sizeof(f32)) f32 cleartypeEnhancedContrast = 0;
            alignas(sizeof(f32x4)) f32 gammaRatios[4]{};
            alignas(sizeof(f32)) f32 dashedLineLength = 0;
#pragma warning(suppress : 4324) // 'ConstBuffer': structure was padded due to alignment specifier
        };

        struct alignas(16) CustomConstBuffer
        {
            // WARNING: Same rules as for ConstBuffer above apply.
            alignas(sizeof(f32)) f32 time = 0;
            alignas(sizeof(f32)) f32 scale = 0;
            alignas(sizeof(f32x2)) f32x2 resolution;
            alignas(sizeof(f32x4)) f32x4 background;
#pragma warning(suppress : 4324) // 'CustomConstBuffer': structure was padded due to alignment specifier
        };

        enum class ShadingType
        {
            Background = 0,
            TextGrayscale,
            TextClearType,
            Passthrough,
            PassthroughInvert,
            DashedLine,
            SolidFill,
        };

        struct alignas(16) QuadInstance
        {
            alignas(sizeof(f32x4)) f32x4 position;
            alignas(sizeof(f32x4)) f32x4 texcoord;
            alignas(sizeof(u32)) u32 color = 0;
            alignas(sizeof(u32)) u32 shadingType = 0;
            alignas(sizeof(u32x2)) u32x2 padding;
        };
        static_assert(sizeof(QuadInstance) == 48);

        struct GlyphCacheEntry
        {
            // BODGY: The IDWriteFontFace results from us calling IDWriteFontFallback::MapCharacters
            // which at the time of writing returns the same IDWriteFontFace as long as someone is
            // holding a reference / the reference count doesn't drop to 0 (see ActiveFaceCache).
            IDWriteFontFace* fontFace = nullptr;
            u16 glyphIndex = 0;
            u16 shadingType = 0;
            i16x2 offset;
            f32x4 texcoord;
        };
        static_assert(sizeof(GlyphCacheEntry) == 32);

        struct GlyphCacheMap
        {
            GlyphCacheMap() = default;

            GlyphCacheMap& operator=(GlyphCacheMap&& other) noexcept
            {
                _map = std::exchange(other._map, {});
                _mapMask = std::exchange(other._mapMask, 0);
                _capacity = std::exchange(other._capacity, 0);
                _size = std::exchange(other._size, 0);
                return *this;
            }

            ~GlyphCacheMap()
            {
                Clear();
            }

            void Clear() noexcept
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

            GlyphCacheEntry& FindOrInsert(IDWriteFontFace* fontFace, u16 glyphIndex, bool& inserted)
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

        private:
            static size_t _hash(IDWriteFontFace* fontFace, u16 glyphIndex) noexcept
            {
                // MSVC 19.33 produces surprisingly good assembly for this without stack allocation.
                const uintptr_t data[2]{ std::bit_cast<uintptr_t>(fontFace), glyphIndex };
                return til::hash(&data[0], sizeof(data));
            }

            GlyphCacheEntry& _insert(IDWriteFontFace* fontFace, u16 glyphIndex, size_t hash)
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

            void _bumpSize()
            {
                const auto newMapSize = _map.size() << 1;
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
                _capacity = newMapMask / 2;
            }

            static constexpr u32 initialSize = 256;

            Buffer<GlyphCacheEntry> _map{ initialSize };
            size_t _mapMask = initialSize - 1;
            size_t _capacity = _mapMask / 2;
            size_t _size = 0;
        };

        void _debugUpdateShaders();
        void _refreshCustomShader(const RenderingPayload& p);
        void _refreshCustomOffscreenTexture(const RenderingPayload& p);
        void _refreshBackgroundColorBitmap(const RenderingPayload& p);
        void _refreshConstBuffer(const RenderingPayload& p);
        void _d2dRenderTargetUpdateFontSettings(const RenderingPayload& p) const;
        void _resetAtlasAndBeginDraw(const RenderingPayload& p);
        void _appendRect(f32x4 position, u32 color, ShadingType shadingType);
        void _appendRect(f32x4 position, f32x4 texcoord, u32 color, ShadingType shadingType);
        __declspec(noinline) void _bumpInstancesSize();
        void _flushRects(const RenderingPayload& p);
        bool _drawGlyph(const RenderingPayload& p, GlyphCacheEntry& entry, f32 fontEmSize);

        SwapChainManager _swapChainManager;

        wil::com_ptr<ID3D11Device1> _device;
        wil::com_ptr<ID3D11DeviceContext1> _deviceContext;
        wil::com_ptr<ID3D11RenderTargetView> _renderTargetView;
        wil::com_ptr<ID3D11RenderTargetView> _renderTargetViewUInt;

        wil::com_ptr<ID3D11RasterizerState> _rasterizerState;
        wil::com_ptr<ID3D11VertexShader> _vertexShader;
        wil::com_ptr<ID3D11PixelShader> _textPixelShader;
        wil::com_ptr<ID3D11BlendState1> _textBlendState;

        wil::com_ptr<ID3D11Buffer> _constantBuffer;
        wil::com_ptr<ID3D11InputLayout> _textInputLayout;

        wil::com_ptr<ID3D11Buffer> _instanceBuffer;
        wil::com_ptr<ID3D11ShaderResourceView> _instanceBufferView;
        size_t _instanceBufferSize = 0;
        Buffer<QuadInstance> _instances;
        size_t _instancesSize;

        wil::com_ptr<ID3D11Buffer> _indexBuffer;
        size_t _indexBufferSize = 0;
        Buffer<u32> _indices;
        size_t _indicesSize;

        wil::com_ptr<ID3D11Texture2D> _backgroundColorBitmap;
        wil::com_ptr<ID3D11ShaderResourceView> _backgroundColorBitmapView;

        wil::com_ptr<ID3D11Texture2D> _glyphAtlas;
        wil::com_ptr<ID3D11ShaderResourceView> _glyphAtlasView;

        wil::com_ptr<ID3D11Texture2D> _customOffscreenTexture;
        wil::com_ptr<ID3D11ShaderResourceView> _customOffscreenTextureView;
        wil::com_ptr<ID3D11RenderTargetView> _customOffscreenTextureTargetView;
        wil::com_ptr<ID3D11VertexShader> _customVertexShader;
        wil::com_ptr<ID3D11PixelShader> _customPixelShader;
        wil::com_ptr<ID3D11Buffer> _customShaderConstantBuffer;
        wil::com_ptr<ID3D11SamplerState> _customShaderSamplerState;
        std::chrono::steady_clock::time_point _customShaderStartTime;

        // D2D resources
        wil::com_ptr<ID2D1DeviceContext> _d2dRenderTarget;
        wil::com_ptr<ID2D1DeviceContext4> _d2dRenderTarget4; // Optional. Supported since Windows 10 14393.
        wil::com_ptr<ID2D1SolidColorBrush> _brush;
        Buffer<DWRITE_FONT_AXIS_VALUE> _textFormatAxes[2][2];
        wil::com_ptr<ID2D1StrokeStyle> _dottedStrokeStyle;

        // D3D resources
        GlyphCacheMap _glyphCache;
        Buffer<stbrp_node> _rectPackerData;
        stbrp_context _rectPacker{};

        bool _requiresContinuousRedraw = false;

        float _gamma = 0;
        float _cleartypeEnhancedContrast = 0;
        float _grayscaleEnhancedContrast = 0;
        wil::com_ptr<IDWriteRenderingParams1> _textRenderingParams;

        u32 _brushColor = 0;
        u16x2 _targetSize;
        u16x2 _cellCount;

        til::generation_t _generation;
        til::generation_t _fontGeneration;
        til::generation_t _miscGeneration;

#ifndef NDEBUG
        std::filesystem::path _sourceDirectory;
        wil::unique_folder_change_reader_nothrow _sourceCodeWatcher;
        std::atomic<int64_t> _sourceCodeInvalidationTime{ INT64_MAX };
#endif
    };
}
