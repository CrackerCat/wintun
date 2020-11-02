/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2020 WireGuard LLC. All Rights Reserved.
 */

#include "entry.h"
#include "logger.h"
#include "namespace.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wchar.h>

static BOOL HasInitialized = FALSE;
static CRITICAL_SECTION Initializing;
static BCRYPT_ALG_HANDLE AlgProvider;

static WCHAR *
NormalizeStringAlloc(_In_ NORM_FORM NormForm, _In_z_ const WCHAR *Source)
{
    int Len = NormalizeString(NormForm, Source, -1, NULL, 0);
    for (;;)
    {
        WCHAR *Str = HeapAlloc(ModuleHeap, 0, sizeof(WCHAR) * Len);
        if (!Str)
            return LOG(WINTUN_LOG_ERR, L"Out of memory"), NULL;
        Len = NormalizeString(NormForm, Source, -1, Str, Len);
        if (Len > 0)
            return Str;
        DWORD Result = GetLastError();
        HeapFree(ModuleHeap, 0, Str);
        if (Result != ERROR_INSUFFICIENT_BUFFER)
            return LOG_ERROR(L"Failed", Result), NULL;
        Len = -Len;
    }
}

static WINTUN_STATUS
NamespaceRuntimeInit(void)
{
    DWORD Result;

    EnterCriticalSection(&Initializing);
    if (HasInitialized)
    {
        LeaveCriticalSection(&Initializing);
        return ERROR_SUCCESS;
    }

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&AlgProvider, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
    {
        Result = ERROR_GEN_FAILURE;
        goto cleanupLeaveCriticalSection;
    }

    BYTE Sid[MAX_SID_SIZE];
    DWORD SidSize = MAX_SID_SIZE;
    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, Sid, &SidSize))
    {
        Result = GetLastError();
        goto cleanupBCryptCloseAlgorithmProvider;
    }

    HANDLE Boundary = CreateBoundaryDescriptorW(L"Wintun", 0);
    if (!Boundary)
    {
        Result = GetLastError();
        goto cleanupBCryptCloseAlgorithmProvider;
    }
    if (!AddSIDToBoundaryDescriptor(&Boundary, Sid))
    {
        Result = GetLastError();
        goto cleanupBCryptCloseAlgorithmProvider;
    }

    for (;;)
    {
        if (CreatePrivateNamespaceW(&SecurityAttributes, Boundary, L"Wintun"))
            break;
        Result = GetLastError();
        if (Result == ERROR_ALREADY_EXISTS)
        {
            if (OpenPrivateNamespaceW(Boundary, L"Wintun"))
                break;
            Result = GetLastError();
            if (Result == ERROR_PATH_NOT_FOUND)
                continue;
        }
        goto cleanupBCryptCloseAlgorithmProvider;
    }

    HasInitialized = TRUE;
    Result = ERROR_SUCCESS;
    goto cleanupLeaveCriticalSection;

cleanupBCryptCloseAlgorithmProvider:
    BCryptCloseAlgorithmProvider(AlgProvider, 0);
cleanupLeaveCriticalSection:
    LeaveCriticalSection(&Initializing);
    return Result;
}

_Check_return_
HANDLE
NamespaceTakePoolMutex(_In_z_ const WCHAR *Pool)
{
    HANDLE Mutex = NULL;

    if (NamespaceRuntimeInit() != ERROR_SUCCESS)
        return NULL;

    BCRYPT_HASH_HANDLE Sha256 = NULL;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(AlgProvider, &Sha256, NULL, 0, NULL, 0, 0)))
        return NULL;
    static const WCHAR mutex_label[] = L"Wintun Adapter Name Mutex Stable Suffix v1 jason@zx2c4.com";
    if (!BCRYPT_SUCCESS(BCryptHashData(Sha256, (PUCHAR)mutex_label, sizeof(mutex_label) /* Including NULL 2 bytes */, 0)))
        goto cleanupSha256;
    WCHAR *PoolNorm = NormalizeStringAlloc(NormalizationC, Pool);
    if (!PoolNorm)
        goto cleanupSha256;
    if (!BCRYPT_SUCCESS(BCryptHashData(Sha256, (PUCHAR)PoolNorm, (int)wcslen(PoolNorm) + 2 /* Add in NULL 2 bytes */, 0)))
        goto cleanupPoolNorm;
    BYTE Hash[32];
    if (!BCRYPT_SUCCESS(BCryptFinishHash(Sha256, Hash, sizeof(Hash), 0)))
        goto cleanupPoolNorm;
    static const WCHAR MutexNamePrefix[] = L"Wintun\\Wintun-Name-Mutex-";
    WCHAR MutexName[_countof(MutexNamePrefix) + sizeof(Hash) * 2];
    memcpy(MutexName, MutexNamePrefix, sizeof(MutexNamePrefix));
    for (size_t i = 0; i < sizeof(Hash); ++i)
        swprintf_s(&MutexName[_countof(MutexNamePrefix) - 1 + i * 2], 3, L"%02x", Hash[i]);
    Mutex = CreateMutexW(&SecurityAttributes, FALSE, MutexName);
    if (!Mutex)
        goto cleanupPoolNorm;
    switch (WaitForSingleObject(Mutex, INFINITE))
    {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        goto cleanupPoolNorm;
    }

    CloseHandle(Mutex);
    Mutex = NULL;
cleanupPoolNorm:
    HeapFree(ModuleHeap, 0, PoolNorm);
cleanupSha256:
    BCryptDestroyHash(Sha256);
    return Mutex;
}

_Check_return_
HANDLE
NamespaceTakeDriverInstallationMutex(void)
{
    HANDLE Mutex = NULL;

    if (NamespaceRuntimeInit() != ERROR_SUCCESS)
        return NULL;
    Mutex = CreateMutexW(&SecurityAttributes, FALSE, L"Wintun\\Wintun-Driver-Installation-Mutex");
    if (!Mutex)
        return NULL;
    switch (WaitForSingleObject(Mutex, INFINITE))
    {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        goto out;
    }

    CloseHandle(Mutex);
    Mutex = NULL;
out:
    return Mutex;
}

void
NamespaceReleaseMutex(_In_ HANDLE Mutex)
{
    ReleaseMutex(Mutex);
    CloseHandle(Mutex);
}

void
NamespaceInit(void)
{
    InitializeCriticalSection(&Initializing);
}

void
NamespaceCleanup(void)
{
    EnterCriticalSection(&Initializing);
    if (HasInitialized)
    {
        BCryptCloseAlgorithmProvider(AlgProvider, 0);
        HasInitialized = FALSE;
    }
    LeaveCriticalSection(&Initializing);
    DeleteCriticalSection(&Initializing);
}
