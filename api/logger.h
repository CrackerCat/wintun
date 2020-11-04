/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2020 WireGuard LLC. All Rights Reserved.
 */

#pragma once

#include "wintun.h"
#include <Windows.h>

extern WINTUN_LOGGER_CALLBACK_FUNC Logger;

/**
 * @copydoc WINTUN_SET_LOGGER_FUNC
 */
void WINAPI
WintunSetLogger(_In_ WINTUN_LOGGER_CALLBACK_FUNC NewLogger);

static inline _Post_equals_last_error_ DWORD
LoggerLog(_In_ WINTUN_LOGGER_LEVEL Level, _In_z_ const WCHAR *LogLine)
{
    DWORD LastError = GetLastError();
    Logger(Level, LogLine);
    SetLastError(LastError);
    return LastError;
}

_Post_equals_last_error_ DWORD
LoggerError(_In_z_ const WCHAR *Prefix, _In_ DWORD Error);

static inline _Post_equals_last_error_ DWORD
LoggerLastError(_In_z_ const WCHAR *Prefix)
{
    DWORD LastError = GetLastError();
    LoggerError(Prefix, LastError);
    SetLastError(LastError);
    return LastError;
}

#define __L(x) L##x
#define _L(x) __L(x)
#define LOG(lvl, msg) (LoggerLog((lvl), _L(__FUNCTION__) L": " msg))
#define LOG_ERROR(msg, err) (LoggerError(_L(__FUNCTION__) L": " msg, (err)))
#define LOG_LAST_ERROR(msg) (LoggerLastError(_L(__FUNCTION__) L": " msg))
