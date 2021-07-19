﻿/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- u8u16convert.h

Abstract:
- Defines classes which hold the status of the current partials handling.
- Defines functions for converting between UTF-8 and UTF-16 strings.

Tests have been made in order to investigate whether or not own algorithms
could overcome disadvantages of syscalls. Test results can be read up
in PR #4093 and the test algorithms are available in src\tools\U8U16Test.
Based on the results the decision was made to keep using the platform
functions MultiByteToWideChar and WideCharToMultiByte.
--*/

#pragma once

namespace til // Terminal Implementation Library. Also: "Today I Learned"
{
    struct u8accumulator
    {
        uint32_t buffer{};
        uint32_t remaining{};
    };

    struct u16accumulator
    {
        uint32_t buffer{};
    };

    // Routine Description:
    // - Takes a UTF-8 string and performs the conversion to UTF-16. NOTE: The function relies on getting complete UTF-8 characters at the string boundaries.
    // Arguments:
    // - in - UTF-8 string to be converted
    // - out - reference to the resulting UTF-16 string
    template<typename Output>
    void u8u16(const std::string_view& in, Output& out)
    {
        out.clear();

        if (in.empty())
        {
            return;
        }

        // The worst ratio of UTF-8 code units to UTF-16 code units is 1 to 1 if UTF-8 consists of ASCII only.
        const auto lengthRequired = gsl::narrow<int>(in.length());
        out.resize(in.length());
        const int lengthOut = MultiByteToWideChar(CP_UTF8, 0, in.data(), lengthRequired, out.data(), lengthRequired);
        out.resize(gsl::narrow_cast<size_t>(lengthOut));
        THROW_LAST_ERROR_IF(lengthOut == 0);
    }

    // Routine Description:
    // - Takes a UTF-8 string, complements and/or caches partials, and performs the conversion to UTF-16.
    // Arguments:
    // - in - UTF-8 string to be converted
    // - out - reference to the resulting UTF-16 string
    // - state - reference to a til::u8state class holding the status of the current partials handling
    template<typename Output>
    void u8u16(const std::string_view& in, Output& out, u8accumulator& state) noexcept
    {
        auto data = in.data();
        auto size = in.size();

        if (!size)
        {
            return;
        }

        if (auto remaining = state.remaining)
        {
            if (remaining > size)
            {
                remaining = size;
            }

            state.remaining -= remaining;

            do
            {
                state.buffer <<= 6;
                state.buffer |= *data++;
            } while (--remaining);

            if (!state.remaining)
            {
                if (state.buffer < 0x10000)
                {
                    out.append(static_cast<wchar_t>(state.buffer));
                }
                else
                {
                    wchar_t buffer[2];
                    buffer[0] = ((state.buffer >> 10) & 0x3FF) + 0xD800;
                    buffer[1] = (state.buffer & 0x3FF) + 0xDC00;
                    out.append(&buffer[0], 2);
                }
            }
        }

        {
            auto end = data + size;
            size_t count = 1;
            // Skip UTF-8 continuation bytes in the form of 0b10xxxxxx.
            while ((*--end & 0b11000000) == 0b10000000 && end != data) {
                ++count;
            }

            // A leading UTF-8 byte is either of:
            // * 0b110xxxxx
            // * 0b1110xxxx
            // * 0b11110xxx
            if (count != 1)
            {
                DWORD index;
                if (_BitScanReverse(&index, ~*end & 0xff))
                {
                    index -= 24;

                    if ((index <= 4) & (index > count))
                    {
                        auto ptr = end;
                        uint32_t buffer = *ptr++ & (0xff >> (7 - index));

                        for (size_t i = 0; i < count; ++i)
                        {
                            buffer <<= 6;
                            buffer |= *ptr++;
                        }

                        state.buffer = buffer;
                        state.remaining = index - count;
                    }
                }
                else
                {
                    ++end;
                }

                size = data - end;
            }
        }

        u16u8({ data, size }, out);
    }

    // Routine Description:
    // - Takes a UTF-8 string and performs the conversion to UTF-16. NOTE: The function relies on getting complete UTF-8 characters at the string boundaries.
    // Arguments:
    // - in - UTF-8 string to be converted
    // Return Value:
    // - the resulting UTF-16 string
    // - NOTE: Throws HRESULT errors that the non-throwing sibling returns
    _TIL_INLINEPREFIX std::wstring u8u16(const std::string_view& in)
    {
        std::wstring out;
        u8u16(in, out);
        return out;
    }

    // Routine Description:
    // - Takes a UTF-16 string and performs the conversion to UTF-8. NOTE: The function relies on getting complete UTF-16 characters at the string boundaries.
    // Arguments:
    // - in - UTF-16 string to be converted
    // - out - reference to the resulting UTF-8 string
    template<typename Output>
    void u16u8(const std::wstring_view& in, Output& out) noexcept
    {
        out.clear();

        if (in.empty())
        {
            return;
        }

        // Code Point U+0000..U+FFFF: 1 UTF-16 code unit --> 1..3 UTF-8 code units.
        // Code Points >U+FFFF: 2 UTF-16 code units --> 4 UTF-8 code units.
        // Thus, the worst ratio of UTF-16 code units to UTF-8 code units is 1 to 3.
        const size_t lengthIn = in.length();
        const size_t lengthRequired = base::CheckMul(lengthIn, 3).ValueOrDie();
        out.resize(lengthRequired);
        const int lengthOut = WideCharToMultiByte(gsl::narrow_cast<UINT>(CP_UTF8), 0ul, in.data(), gsl::narrow<int>(lengthIn), out.data(), gsl::narrow<int>(lengthRequired), nullptr, nullptr);
        out.resize(lengthOut);
        THROW_LAST_ERROR_IF(lengthOut == 0);
    }

    // Routine Description:
    // - Takes a UTF-16 string, complements and/or caches partials, and performs the conversion to UTF-8.
    // Arguments:
    // - in - UTF-16 string to be converted
    // - out - reference to the resulting UTF-8 string
    // - state - reference to a til::u16state class holding the status of the current partials handling
    template<typename Output>
    void u16u8(const std::wstring_view& in, Output& out, u16accumulator& state) noexcept
    {
        auto data = in.data();
        auto size = in.size();

        if (!size)
        {
            return;
        }

        if (state.buffer)
        {
            if (*data >= 0xDC00 & *data <= 0xDFFF)
            {
                const auto buffer = (state.buffer << 10) | (*data - 0xDC00);
                char buffer[4];
                buffer[0] = 0b11110000 | ((buffer >> 18) & 0x3f);
                buffer[1] = 0b10000000 | ((buffer >> 12) & 0x3f);
                buffer[2] = 0b10000000 | ((buffer >> 6) & 0x3f);
                buffer[3] = 0b10000000 | ((buffer >> 0) & 0x3f);
                out.append(&buffer[0], 4);

                ++data;
                --size;
                if (!size)
                {
                    return;
                }
            }

            state = {};
        }

        if (auto end = data + size - 1; *end >= 0xD800 && *end <= 0xDBFF)
        {
            state.buffer = *end - 0xD800;

            --size;
            if (!size)
            {
                return;
            }
        }

        u16u8({ data, size }, out);
    }

    // Routine Description:
    // - Takes a UTF-16 string and performs the conversion to UTF-8. NOTE: The function relies on getting complete UTF-16 characters at the string boundaries.
    // Arguments:
    // - in - UTF-16 string to be converted
    // Return Value:
    // - the resulting UTF-8 string
    // - NOTE: Throws HRESULT errors that the non-throwing sibling returns
    _TIL_INLINEPREFIX std::string u16u8(const std::wstring_view& in)
    {
        std::string out{};
        u16u8(in, out);
        return out;
    }
}
