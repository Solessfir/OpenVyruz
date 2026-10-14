#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

// The game's two languages (LanguageGet at 00401030): this Russian build of Vyruz.exe holds English and, in the second language's place, Russian, which vzconf.txt still names French.
// The second language reads the archive's -fr texts and, in the Russian data, draws them with its Cyrillic fonts.
enum class ELanguage : uint8_t
{
    English = 1,
    Russian = 2,
};

void SetLanguage(ELanguage Language);
ELanguage GetLanguage();

// A text of the original in the language in use: its English as given, or its Russian as the Windows-1251 bytes the fonts are indexed by.
std::string_view Localize(std::string_view English);
// The suffix of the archive's texts in the language in use (story-en.txt, story-fr.txt).
std::string_view TextSuffix();

using FFormatArgument = std::variant<int64_t, double, std::string_view>;

// FUN_00454320's printf formatting of a text in the language in use: %d (with a width or zero fill), %s, %.Nf and %%; a missing argument prints nothing, as the Russian texts sometimes ask for one more.
std::string FormatText(std::string_view English, std::span<const FFormatArgument> Arguments);

template <typename... TArguments>
std::string Format(const std::string_view English, const TArguments&... Arguments)
{
    const auto Convert = []<typename T>(const T& Argument) -> FFormatArgument
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            return double(Argument);
        }
        else if constexpr (std::is_integral_v<T>)
        {
            return int64_t(Argument);
        }
        else
        {
            return std::string_view(Argument);
        }
    };
    const FFormatArgument Converted[] = { Convert(Arguments)..., FFormatArgument() };
    return FormatText(English, std::span(Converted, sizeof...(Arguments)));
}
