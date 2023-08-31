// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "thread.hpp"

#include <til/atomic.h>

#include "renderer.hpp"

#pragma hdrstop

using namespace Microsoft::Console::Render;

RenderThread::~RenderThread()
{
    _state.store(State::ExitRequested, std::memory_order_relaxed);
    til::atomic_notify_all(_state);
    WaitForSingleObject(_hThread.get(), INFINITE);
}

// Method Description:
// - Create all of the Events we'll need, and the actual thread we'll be doing
//      work on.
// Arguments:
// - pRendererParent: the Renderer that owns this thread, and which we should
//      trigger frames for.
// Return Value:
// - S_OK if we succeeded, else an HRESULT corresponding to a failure to create
//      an Event or Thread.
void RenderThread::Initialize(Renderer* const pRendererParent)
{
    _pRenderer = pRendererParent;
    _hThread.reset(THROW_LAST_ERROR_IF_NULL(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &s_threadProc, this, 0, nullptr))));

    // SetThreadDescription only works on 1607 and higher. If we cannot find it,
    // then it's no big deal. Just skip setting the description.
    if (const auto func = GetProcAddressByFunctionDeclaration(GetModuleHandleW(L"kernel32.dll"), SetThreadDescription))
    {
        LOG_IF_FAILED(func(_hThread.get(), L"Rendering Output Thread"));
    }
}

void RenderThread::NotifyPaint() noexcept
{
    auto state = _state.load(std::memory_order::relaxed);

    while (state == State::Waiting)
    {
        if (_state.compare_exchange_weak(state, State::PaintRequested, std::memory_order::relaxed))
        {
            til::atomic_notify_all(_state);
            break;
        }
    }
}

void RenderThread::EnablePainting() noexcept
{
    auto expected = State::Disabled;
    if (_state.compare_exchange_strong(expected, State::Waiting, std::memory_order::relaxed))
    {
        til::atomic_notify_all(_state);
    }
}

void RenderThread::DisablePainting() noexcept
{
    _state.store(State::Disabled, std::memory_order::relaxed);
    til::atomic_notify_all(_state);
}

void RenderThread::WaitForPaintCompletion(const DWORD dwTimeoutMs) noexcept
{
}

unsigned WINAPI RenderThread::s_threadProc(void* parameter)
{
    return static_cast<RenderThread*>(parameter)->_threadProc();
}

unsigned int RenderThread::_threadProc()
{
    for (;;)
    {
        switch (auto state = _state.load(std::memory_order_relaxed))
        {
        case State::Disabled:
        case State::Waiting:
            til::atomic_wait(_state, state);
            break;
        case State::PaintRequested:
            _pRenderer->WaitUntilCanRender();
            LOG_IF_FAILED(_pRenderer->PaintFrame());
            // NOTE: This implementation is subtly broken.
            // In between us storing State::Waiting and PaintFrame() actually acquiring the console lock more calls to NotifyPaint() may arrive.
            // We cannot store it after the call either, because then we might accidentally miss a NotifyPaint().
            // This cannot be fixed at the time of writing, because NotifyPaint() is designed to be lock-free.
            // The rendering code needs to be redesigned to center around locking the rendering payload for all operations.
            if (_state.compare_exchange_strong(state, State::Waiting, std::memory_order_relaxed))
            {
            }
            break;
        case State::ExitRequested:
            return 0;
        }
    }
}
