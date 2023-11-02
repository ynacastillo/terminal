// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "NtPrivApi.hpp"

namespace
{
    struct PROCESS_BASIC_INFORMATION_EXPANDED
    {
        NTSTATUS ExitStatus;
        PVOID PebBaseAddress;
        ULONG_PTR AffinityMask;
        LONG BasePriority;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR InheritedFromUniqueProcessId;
    };
}

[[nodiscard]] NTSTATUS NtPrivApi::GetProcessParentId(_Inout_ PULONG ProcessId) const noexcept
{
    if (!_complete)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // TODO: Get Parent current not really available without winternl + NtQueryInformationProcess. http://osgvsowi/8394495
    OBJECT_ATTRIBUTES oa;
#pragma warning(suppress : 26477) // This macro contains a bare NULL
    InitializeObjectAttributes(&oa, nullptr, 0, nullptr, nullptr);

    CLIENT_ID ClientId;
    ClientId.UniqueProcess = UlongToHandle(*ProcessId);
    ClientId.UniqueThread = nullptr;

    HANDLE ProcessHandle;
    auto Status = _fnNtOpenProcess(&ProcessHandle, PROCESS_QUERY_LIMITED_INFORMATION, &oa, &ClientId);

    PROCESS_BASIC_INFORMATION_EXPANDED BasicInfo = { 0 };
    if (SUCCEEDED_NTSTATUS(Status))
    {
        Status = _fnNtQueryInformationProcess(ProcessHandle, ProcessBasicInformation, &BasicInfo, sizeof(BasicInfo), nullptr);
        LOG_IF_FAILED(_fnNtClose(ProcessHandle));
    }

    if (FAILED_NTSTATUS(Status))
    {
        *ProcessId = 0;
        return Status;
    }

    *ProcessId = (ULONG)BasicInfo.InheritedFromUniqueProcessId;
    return STATUS_SUCCESS;
}

NtPrivApi::NtPrivApi() noexcept
{
    if (const auto hNtDll = GetModuleHandleW(L"ntdll.dll"))
    {
        _fnNtOpenProcess = reinterpret_cast<decltype(_fnNtOpenProcess)>(GetProcAddress(hNtDll, "NtOpenProcess"));
        _fnNtQueryInformationProcess = reinterpret_cast<decltype(_fnNtQueryInformationProcess)>(GetProcAddress(hNtDll, "NtQueryInformationProcess"));
        _fnNtClose = reinterpret_cast<decltype(_fnNtClose)>(GetProcAddress(hNtDll, "NtClose"));
    }
    _complete = _fnNtOpenProcess && _fnNtQueryInformationProcess && _fnNtClose;
}
