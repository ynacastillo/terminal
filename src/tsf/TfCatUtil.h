/*++

Copyright (c) Microsoft Corporation.
Licensed under the MIT license.

Module Name:

    TfCatUtil.h

Abstract:

    This file defines the CicCategoryMgr Class.

Author:

Revision History:

Notes:

--*/

#pragma once

class CicCategoryMgr
{
public:
    [[nodiscard]] HRESULT GetGUIDFromGUIDATOM(TfGuidAtom guidatom, GUID* pguid);
    [[nodiscard]] HRESULT InitCategoryInstance();

    ITfCategoryMgr* GetCategoryMgr();

private:
    wil::com_ptr_nothrow<ITfCategoryMgr> m_pcat;
};
