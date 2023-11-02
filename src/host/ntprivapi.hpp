/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- userdpiapi.hpp

Abstract:
- This module is used for abstracting calls to ntdll DLL APIs to break DDK dependencies.

Author(s):
- Michael Niksa (MiNiksa) July-2016
--*/
#pragma once

#include "conddkrefs.h"

class NtPrivApi sealed
{
public:
    NtPrivApi() noexcept;

    [[nodiscard]] NTSTATUS GetProcessParentId(_Inout_ PULONG ProcessId) const noexcept;

private:
    // clang-format off
    NTSTATUS(*_fnNtOpenProcess)(HANDLE ProcessHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID* ClientId) = nullptr;
    NTSTATUS(*_fnNtQueryInformationProcess)(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength) = nullptr;
    NTSTATUS(*_fnNtClose)(HANDLE Handle) = nullptr;
    // clang-format no
    bool _complete = false;
};
