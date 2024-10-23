/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#if !defined(AK_OS_WINDOWS)
#    include <cxxabi.h>
#else
#    include <windows.h>
// NOTE: Windows must go first
#    include <dbghelp.h>
#endif

namespace AK {

inline ByteString demangle(StringView name)
{
#if !defined(AK_OS_WINDOWS)
    int status = 0;
    auto* demangled_name = abi::__cxa_demangle(name.to_byte_string().characters(), nullptr, nullptr, &status);
    auto string = ByteString(status == 0 ? StringView { demangled_name, strlen(demangled_name) } : name);
    if (status == 0)
        free(demangled_name);
    return string;
#else
    char* realname = (char*)malloc(1024 * sizeof(char));
    realname ? realname[0] = 0, ::UnDecorateSymbolName(name.to_byte_string().characters(), realname, 1024, 0) : 0;
    ByteString string = ByteString(realname);
    free(realname);
    return string;
#endif
}

}

#if USING_AK_GLOBALLY
using AK::demangle;
#endif
