// GameVault for PS5 - A small JSON reader and writer for the native app.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/gv_json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gv
{

namespace
{
const Json kNull;
}

class Parser
{
  public:
    explicit Parser(std::string_view text) : p_(text.data()), end_(text.data() + text.size())
    {
    }

    bool value(Json &out, int depth)
    {
        if (depth > 64)
            return false;
        ws();
        if (p_ >= end_)
            return false;
        switch (*p_)
        {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"':
            out.type_ = Json::Type::string;
            return string(out.string_);
        case 't':
            return literal("true", out, Json::Type::boolean, true);
        case 'f':
            return literal("false", out, Json::Type::boolean, false);
        case 'n':
            return literal("null", out, Json::Type::null, false);
        default:
            return number(out);
        }
    }

    bool at_end()
    {
        ws();
        return p_ == end_;
    }

  private:
    void ws()
    {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t'))
            ++p_;
    }

    bool literal(const char *word, Json &out, Json::Type type, bool b)
    {
        const std::size_t n = std::strlen(word);
        if (static_cast<std::size_t>(end_ - p_) < n || std::memcmp(p_, word, n) != 0)
            return false;
        p_ += n;
        out.type_ = type;
        out.bool_ = b;
        return true;
    }

    bool number(Json &out)
    {
        const char *start = p_;
        while (p_ < end_ && (std::strchr("+-0123456789.eE", *p_) != nullptr))
            ++p_;
        if (p_ == start)
            return false;
        out.type_ = Json::Type::number;
        out.number_ = std::strtod(std::string(start, p_).c_str(), nullptr);
        return true;
    }

    static void utf8(std::string &s, unsigned cp)
    {
        if (cp < 0x80)
            s += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned &v)
    {
        if (end_ - p_ < 4)
            return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool string(std::string &out)
    {
        ++p_; // opening quote
        while (p_ < end_ && *p_ != '"')
        {
            const char c = *p_++;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (p_ >= end_)
                return false;
            const char e = *p_++;
            switch (e)
            {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u')
                {
                    p_ += 2;
                    unsigned lo = 0;
                    if (!hex4(lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default:
                out += e;
            }
        }
        if (p_ >= end_)
            return false;
        ++p_;
        return true;
    }

    bool array(Json &out, int depth)
    {
        out.type_ = Json::Type::array;
        ++p_;
        ws();
        if (p_ < end_ && *p_ == ']')
        {
            ++p_;
            return true;
        }
        for (;;)
        {
            Json item;
            if (!value(item, depth + 1))
                return false;
            out.items_.push_back(std::move(item));
            ws();
            if (p_ < end_ && *p_ == ',')
            {
                ++p_;
                continue;
            }
            if (p_ < end_ && *p_ == ']')
            {
                ++p_;
                return true;
            }
            return false;
        }
    }

    bool object(Json &out, int depth)
    {
        out.type_ = Json::Type::object;
        ++p_;
        ws();
        if (p_ < end_ && *p_ == '}')
        {
            ++p_;
            return true;
        }
        for (;;)
        {
            ws();
            if (p_ >= end_ || *p_ != '"')
                return false;
            std::string key;
            if (!string(key))
                return false;
            ws();
            if (p_ >= end_ || *p_ != ':')
                return false;
            ++p_;
            Json item;
            if (!value(item, depth + 1))
                return false;
            out.keys_.push_back(std::move(key));
            out.items_.push_back(std::move(item));
            ws();
            if (p_ < end_ && *p_ == ',')
            {
                ++p_;
                continue;
            }
            if (p_ < end_ && *p_ == '}')
            {
                ++p_;
                return true;
            }
            return false;
        }
    }

    const char *p_;
    const char *end_;
};

Json Json::parse(std::string_view text, bool *ok)
{
    Json out;
    Parser parser(text);
    const bool good = parser.value(out, 0) && parser.at_end();
    if (ok)
        *ok = good;
    return good ? out : Json();
}

const Json &Json::operator[](std::string_view key) const
{
    if (type_ != Type::object)
        return kNull;
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key)
            return items_[i];
    return kNull;
}

const Json &Json::operator[](std::size_t index) const
{
    if (type_ != Type::array || index >= items_.size())
        return kNull;
    return items_[index];
}

std::string Json::str(std::string_view fallback) const
{
    if (type_ == Type::string)
        return string_;
    if (type_ == Type::number)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.15g", number_);
        return buf;
    }
    return std::string(fallback);
}

double Json::num(double fallback) const
{
    return type_ == Type::number ? number_ : fallback;
}

std::int64_t Json::i64(std::int64_t fallback) const
{
    return type_ == Type::number ? static_cast<std::int64_t>(number_) : fallback;
}

bool Json::boolean(bool fallback) const
{
    return type_ == Type::boolean ? bool_ : fallback;
}

std::string json_quote(std::string_view value)
{
    std::string out = "\"";
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += c;
        }
    }
    out += '"';
    return out;
}

} // namespace gv
