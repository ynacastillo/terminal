// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"
#include "CommonState.hpp"

#include "../types/inc/CodepointWidthDetector.hpp"

using namespace WEX::Logging;

static constexpr std::wstring_view emoji = L"\xD83E\xDD22"; // U+1F922 nauseated face

static constexpr std::wstring_view ambiguous = L"\x414"; // U+0414 cyrillic capital de

// codepoint and utf16 encoded string
static constexpr std::array testData = {
    std::pair{ L"\a", 1 }, // BEL
    std::pair{ L" ", 1 },
    std::pair{ L"9", 1 },
    std::pair{ L"\x414", 1 }, // U+0414 cyrillic capital de
    std::pair{ L"\x1104", 2 }, // U+1104 hangul choseong ssangtikeut
    std::pair{ L"\x306A", 2 }, // U+306A hiragana na
    std::pair{ L"\x30CA", 2 }, // U+30CA katakana na
    std::pair{ L"\x72D7", 2 }, // U+72D7
    std::pair{ L"\xD83D\xDC7E", 2 }, // U+1F47E alien monster
    std::pair{ L"\xD83D\xDD1C", 2 } // U+1F51C SOON
};

class CodepointWidthDetectorTests
{
    TEST_CLASS(CodepointWidthDetectorTests);

    TEST_METHOD(CanLookUpEmoji)
    {
        CodepointWidthDetector widthDetector;
        VERIFY_IS_TRUE(widthDetector.IsWide(emoji));
    }

    TEST_METHOD(CanGetWidths)
    {
        CodepointWidthDetector widthDetector;
        for (const auto& [wstr, expected] : testData)
        {
            const auto result = widthDetector.GetWidth({ wstr });
            VERIFY_ARE_EQUAL(result, expected);
        }
    }

    static bool FallbackMethod(const std::wstring_view glyph)
    {
        if (glyph.size() < 1)
        {
            return false;
        }
        else
        {
            return (glyph.at(0) % 2) == 1;
        }
    }

    TEST_METHOD(AmbiguousCache)
    {
        // Set up a detector with fallback.
        CodepointWidthDetector widthDetector;
        widthDetector.SetFallbackMethod(std::bind(&FallbackMethod, std::placeholders::_1));

        // Ensure fallback cache is empty.
        VERIFY_ARE_EQUAL(0u, widthDetector._fallbackCache.size());

        // Lookup ambiguous width character.
        widthDetector.IsWide(ambiguous);

        // Cache should hold it.
        VERIFY_ARE_EQUAL(1u, widthDetector._fallbackCache.size());

        // Cached item should match what we expect
        const auto it = widthDetector._fallbackCache.begin();
        VERIFY_ARE_EQUAL(ambiguous[0], it->first);
        VERIFY_ARE_EQUAL(FallbackMethod(ambiguous) ? 2u : 1u, it->second);

        // Cache should empty when font changes.
        widthDetector.NotifyFontChanged();
        VERIFY_ARE_EQUAL(0u, widthDetector._fallbackCache.size());
    }
};
