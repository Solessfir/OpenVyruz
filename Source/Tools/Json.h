#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

struct FJsonMember;

// A parsed JSON value: what --compress reads back from the manifest and the extracted data.
struct FJsonValue
{
    enum class EType : uint8_t
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    // A member's value, or a null value when the object has none.
    const FJsonValue& operator[](std::string_view Name) const;
    // An array's element, or a null value past its end.
    const FJsonValue& operator[](size_t Index) const;
    std::string_view AsString() const { return String; }
    double AsNumber() const { return Number; }

    EType Type = EType::Null;
    bool bValue = false;
    double Number = 0.;
    std::string String;
    std::vector<FJsonValue> Elements;
    std::vector<FJsonMember> Members;
};

struct FJsonMember
{
    std::string Name;
    FJsonValue Value;
};

// Parses a whole JSON text; nullopt on any syntax error.
std::optional<FJsonValue> ParseJson(std::string_view Text);

// A small pretty-printing JSON writer for the extracted data: objects and arrays nest, and number arrays can stay on one line.
class FJsonWriter
{
public:
    void BeginObject() { Open('{'); }
    void EndObject() { Close('}'); }
    void BeginArray() { Open('['); }
    void EndArray() { Close(']'); }

    void Key(const std::string_view Name)
    {
        Separate();
        AppendString(Name);
        Text += ": ";
        bAfterKey = true;
    }

    void String(const std::string_view Value)
    {
        Separate();
        AppendString(Value);
    }

    void Number(const int64_t Value)
    {
        Separate();
        Text += std::to_string(Value);
    }

    void Number(const double Value)
    {
        Separate();
        Text += std::format("{}", Value);
    }

    void Bool(const bool bValue)
    {
        Separate();
        Text += bValue ? "true" : "false";
    }

    // A number array on one line, as a tile layer's data.
    void Numbers(const std::span<const int64_t> Values)
    {
        Separate();
        Text += '[';
        for (size_t Index = 0; Index < Values.size(); ++Index)
        {
            Text += Index == 0 ? "" : ",";
            Text += std::to_string(Values[Index]);
        }

        Text += ']';
    }

    template <typename T>
    void Field(const std::string_view Name, const T& Value)
    {
        Key(Name);
        if constexpr (std::is_same_v<T, bool>)
        {
            Bool(Value);
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            Number(double(Value));
        }
        else if constexpr (std::is_integral_v<T>)
        {
            Number(int64_t(Value));
        }
        else
        {
            String(Value);
        }
    }

    const std::string& GetText() const { return Text; }

private:
    void Open(const char Bracket)
    {
        Separate();
        Text += Bracket;
        bFirst.push_back(true);
    }

    void Close(const char Bracket)
    {
        const bool bEmpty = bFirst.back();
        bFirst.pop_back();
        if (!bEmpty)
        {
            NewLine();
        }

        Text += Bracket;
        if (bFirst.empty())
        {
            Text += '\n';
        }
    }

    // A comma before every value but a container's first, and a line break unless the value follows its key.
    void Separate()
    {
        if (std::exchange(bAfterKey, false) || bFirst.empty())
        {
            return;
        }

        Text += bFirst.back() ? "" : ",";
        bFirst.back() = false;
        NewLine();
    }

    void NewLine()
    {
        Text += '\n';
        Text.append(bFirst.size() * IndentWidth, ' ');
    }

    void AppendString(const std::string_view Value)
    {
        Text += '"';
        for (const char Character : Value)
        {
            switch (Character)
            {
            case '"':
                Text += "\\\"";
                break;
            case '\\':
                Text += "\\\\";
                break;
            case '\n':
                Text += "\\n";
                break;
            case '\r':
                Text += "\\r";
                break;
            case '\t':
                Text += "\\t";
                break;
            default:
                if (uint8_t(Character) < ControlLimit)
                {
                    Text += std::format("\\u{:04x}", uint8_t(Character));
                }
                else
                {
                    Text += Character;
                }
            }
        }

        Text += '"';
    }

    static constexpr size_t IndentWidth = 2;
    static constexpr uint8_t ControlLimit = 0x20;

    std::string Text;
    std::vector<bool> bFirst;
    bool bAfterKey = false;
};
