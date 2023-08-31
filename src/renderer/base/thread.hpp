/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- Thread.hpp

Abstract:
- This is the definition of our rendering thread designed to throttle and compartmentalize drawing operations.

Author(s):
- Michael Niksa (MiNiksa) Feb 2016
--*/

#pragma once

namespace Microsoft::Console::Render
{
    class Renderer;

    // _state is a frequently accessed atomic and so it benefits from being on its own cache line.
    // Using alignas() on the entire class instead of just the member has the benefit that the total size of the class is
    // only 1 cache line instead of 2. This works well, because the other 2 members are constant across the class lifetime.
    class alignas(std::hardware_destructive_interference_size) RenderThread
    {
    public:
        ~RenderThread();

        void Initialize(Renderer* pRendererParent);
        void NotifyPaint() noexcept;
        void EnablePainting() noexcept;
        void DisablePainting() noexcept;
        void WaitForPaintCompletion(const DWORD dwTimeoutMs) noexcept;

    private:
        static constexpr uint32_t Request_DisabledFlag = 0b01;
        static constexpr uint32_t Request_ExitFlag = 0b10;
        static constexpr uint32_t Request_CountMask = ~0b11;
        enum class State : uint8_t
        {
            Disabled = 0,
            Waiting,
            PaintRequested,
            ExitRequested,
        };

        static unsigned int WINAPI s_threadProc(void* parameter);
        unsigned int _threadProc();

        Renderer* _pRenderer = nullptr; // Non-ownership pointer
        wil::unique_handle _hThread;
        std::atomic<State> _state{ State::Disabled };
#pragma warning(suppress : 4324) // 'Microsoft::Console::Render::RenderThread': structure was padded due to alignment specifier
    };
}
