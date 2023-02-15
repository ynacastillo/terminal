#pragma once

#include <d3d11_2.h>

#include "Backend.h"

namespace Microsoft::Console::Render::Atlas
{
    struct BackendD2D : IBackend
    {
        BackendD2D(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext);

        void Render(const RenderingPayload& payload) override;
        bool RequiresContinuousRedraw() noexcept override;
        void WaitUntilCanRender() noexcept override;

    private:
        ID2D1Brush* _brushWithColor(u32 color);
        void _d2dDrawLine(const RenderingPayload& p, u16r rect, u16 pos, u16 width, u32 color, ID2D1StrokeStyle* strokeStyle);
        void _d2dFillRectangle(const RenderingPayload& p, u16r rect, u32 color);
        void _d2dCellFlagRendererCursor(const RenderingPayload& p, u16r rect, u32 color);
        void _d2dCellFlagRendererSelected(const RenderingPayload& p, u16r rect, u32 color);
        void _d2dCellFlagRendererUnderline(const RenderingPayload& p, u16r rect, u32 color);
        void _d2dCellFlagRendererUnderlineDotted(const RenderingPayload& p, u16r rect, u32 color);
        void _d2dCellFlagRendererUnderlineDouble(const RenderingPayload& p, u16r rect, u32 color);
        void _d2dCellFlagRendererStrikethrough(const RenderingPayload& p, u16r rect, u32 color);

        SwapChainManager _swapChainManager;

        wil::com_ptr<ID3D11Device1> _device;
        wil::com_ptr<ID3D11DeviceContext1> _deviceContext;
        wil::com_ptr<ID3D11RenderTargetView> _renderTargetView;
        wil::com_ptr<ID3D11RenderTargetView> _renderTargetViewUInt;

        wil::com_ptr<ID3D11VertexShader> _vertexShader;
        wil::com_ptr<ID3D11PixelShader> _cleartypePixelShader;
        wil::com_ptr<ID3D11PixelShader> _grayscalePixelShader;
        wil::com_ptr<ID3D11PixelShader> _invertCursorPixelShader;
        wil::com_ptr<ID3D11BlendState1> _cleartypeBlendState;
        wil::com_ptr<ID3D11BlendState1> _alphaBlendState;
        wil::com_ptr<ID3D11BlendState1> _invertCursorBlendState;

        wil::com_ptr<ID3D11RasterizerState> _rasterizerState;
        wil::com_ptr<ID3D11PixelShader> _textPixelShader;
        wil::com_ptr<ID3D11BlendState> _textBlendState;

        wil::com_ptr<ID3D11PixelShader> _wireframePixelShader;
        wil::com_ptr<ID3D11RasterizerState> _wireframeRasterizerState;

        wil::com_ptr<ID3D11Buffer> _constantBuffer;
        wil::com_ptr<ID3D11InputLayout> _textInputLayout;
        wil::com_ptr<ID3D11Buffer> _vertexBuffers[2];
        size_t _vertexBuffers1Size = 0;

        wil::com_ptr<ID3D11Texture2D> _perCellColor;
        wil::com_ptr<ID3D11ShaderResourceView> _perCellColorView;

        wil::com_ptr<ID3D11Texture2D> _customOffscreenTexture;
        wil::com_ptr<ID3D11ShaderResourceView> _customOffscreenTextureView;
        wil::com_ptr<ID3D11RenderTargetView> _customOffscreenTextureTargetView;
        wil::com_ptr<ID3D11VertexShader> _customVertexShader;
        wil::com_ptr<ID3D11PixelShader> _customPixelShader;
        wil::com_ptr<ID3D11Buffer> _customShaderConstantBuffer;
        wil::com_ptr<ID3D11SamplerState> _customShaderSamplerState;
        std::chrono::steady_clock::time_point _customShaderStartTime;

        // D2D resources
        wil::com_ptr<ID3D11Texture2D> _atlasBuffer;
        wil::com_ptr<ID3D11ShaderResourceView> _atlasView;
        wil::com_ptr<ID2D1DeviceContext> _d2dRenderTarget;
        wil::com_ptr<ID2D1DeviceContext4> _d2dRenderTarget4; // Optional. Supported since Windows 10 14393.
        wil::com_ptr<ID2D1SolidColorBrush> _brush;
        Buffer<DWRITE_FONT_AXIS_VALUE> _textFormatAxes[2][2];
        wil::com_ptr<ID2D1StrokeStyle> _dottedStrokeStyle;

        wil::com_ptr<ID2D1Bitmap> _d2dBackgroundBitmap;
        wil::com_ptr<ID2D1BitmapBrush> _d2dBackgroundBrush;

        til::generation_t _fontGeneration;
        til::generation_t _generation;
        u16x2 _cellCount;

        u32 _brushColor = 0;
    };
}
