#include "Tools/Json.h"

#include <charconv>

namespace
{
const FJsonValue NullValue;

class FJsonParser
{
public:
    explicit FJsonParser(const std::string_view InText) : Text(InText) {}

    std::optional<FJsonValue> ParseDocument()
    {
        std::optional<FJsonValue> Value = ParseValue(0);
        SkipSpace();
        return Value && Position == Text.size() ? Value : std::nullopt;
    }

private:
    // Deeper nesting than any extracted file holds is refused rather than recursed into.
    static constexpr size_t MaxDepth = 64;

    void SkipSpace()
    {
        while (Position < Text.size() && (Text[Position] == ' ' || Text[Position] == '\n' || Text[Position] == '\r' || Text[Position] == '\t'))
        {
            ++Position;
        }
    }

    bool Consume(const std::string_view Word)
    {
        if (Text.substr(Position, Word.size()) != Word)
        {
            return false;
        }

        Position += Word.size();
        return true;
    }

    std::optional<FJsonValue> ParseValue(const size_t Depth)
    {
        SkipSpace();
        if (Position >= Text.size() || Depth > MaxDepth)
        {
            return std::nullopt;
        }

        FJsonValue Value;
        const char Next = Text[Position];
        if (Next == '{')
        {
            ++Position;
            Value.Type = FJsonValue::EType::Object;
            SkipSpace();
            if (Consume("}"))
            {
                return Value;
            }

            for (;;)
            {
                SkipSpace();
                std::optional<std::string> Name = ParseString();
                SkipSpace();
                if (!Name || !Consume(":"))
                {
                    return std::nullopt;
                }

                std::optional<FJsonValue> Member = ParseValue(Depth + 1);
                if (!Member)
                {
                    return std::nullopt;
                }

                Value.Members.push_back({ .Name = std::move(*Name), .Value = std::move(*Member) });
                SkipSpace();
                if (Consume("}"))
                {
                    return Value;
                }

                if (!Consume(","))
                {
                    return std::nullopt;
                }
            }
        }

        if (Next == '[')
        {
            ++Position;
            Value.Type = FJsonValue::EType::Array;
            SkipSpace();
            if (Consume("]"))
            {
                return Value;
            }

            for (;;)
            {
                std::optional<FJsonValue> Element = ParseValue(Depth + 1);
                if (!Element)
                {
                    return std::nullopt;
                }

                Value.Elements.push_back(std::move(*Element));
                SkipSpace();
                if (Consume("]"))
                {
                    return Value;
                }

                if (!Consume(","))
                {
                    return std::nullopt;
                }
            }
        }

        if (Next == '"')
        {
            std::optional<std::string> String = ParseString();
            if (!String)
            {
                return std::nullopt;
            }

            Value.Type = FJsonValue::EType::String;
            Value.String = std::move(*String);
            return Value;
        }

        if (Consume("true") || Consume("false"))
        {
            Value.Type = FJsonValue::EType::Bool;
            Value.bValue = Text[Position - 1] == 'e' && Text.substr(Position - 4, 4) == "true";
            return Value;
        }

        if (Consume("null"))
        {
            return Value;
        }

        const char* const First = Text.data() + Position;
        const auto [End, Error] = std::from_chars(First, Text.data() + Text.size(), Value.Number);
        if (Error != std::errc{})
        {
            return std::nullopt;
        }

        Position += size_t(End - First);
        Value.Type = FJsonValue::EType::Number;
        return Value;
    }

    // A string with its escapes, \u ones written out as UTF-8.
    std::optional<std::string> ParseString()
    {
        if (!Consume("\""))
        {
            return std::nullopt;
        }

        std::string Out;
        while (Position < Text.size() && Text[Position] != '"')
        {
            char Character = Text[Position++];
            if (Character != '\\')
            {
                Out += Character;
                continue;
            }

            if (Position >= Text.size())
            {
                return std::nullopt;
            }

            Character = Text[Position++];
            switch (Character)
            {
            case 'n':
                Out += '\n';
                break;
            case 'r':
                Out += '\r';
                break;
            case 't':
                Out += '\t';
                break;
            case 'b':
                Out += '\b';
                break;
            case 'f':
                Out += '\f';
                break;
            case 'u':
            {
                constexpr size_t HexDigits = 4;
                constexpr int Hex = 16;
                uint32_t Code = 0;
                if (Position + HexDigits > Text.size() ||
                    std::from_chars(Text.data() + Position, Text.data() + Position + HexDigits, Code, Hex).ptr != Text.data() + Position + HexDigits)
                {
                    return std::nullopt;
                }

                Position += HexDigits;
                if (Code < 0x80)
                {
                    Out += char(Code);
                }
                else if (Code < 0x800)
                {
                    Out += char(0xC0 | Code >> 6);
                    Out += char(0x80 | (Code & 0x3F));
                }
                else
                {
                    Out += char(0xE0 | Code >> 12);
                    Out += char(0x80 | (Code >> 6 & 0x3F));
                    Out += char(0x80 | (Code & 0x3F));
                }

                break;
            }
            default:
                Out += Character;
            }
        }

        return Consume("\"") ? std::optional(std::move(Out)) : std::nullopt;
    }

    std::string_view Text;
    size_t Position = 0;
};
}

const FJsonValue& FJsonValue::operator[](const std::string_view Name) const
{
    for (const auto& [MemberName, Value] : Members)
    {
        if (MemberName == Name)
        {
            return Value;
        }
    }

    return NullValue;
}

const FJsonValue& FJsonValue::operator[](const size_t Index) const
{
    return Index < Elements.size() ? Elements[Index] : NullValue;
}

std::optional<FJsonValue> ParseJson(const std::string_view Text)
{
    return FJsonParser(Text).ParseDocument();
}
