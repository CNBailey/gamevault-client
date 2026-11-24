// GameVault for PS5 - A small JSON reader and writer for the native app.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gv
{

class Json
{
  public:
    enum class Type : std::uint8_t
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    static Json parse(std::string_view text, bool *ok = nullptr);

    Type type() const
    {
        return type_;
    }
    bool is_null() const
    {
        return type_ == Type::null;
    }
    bool is_object() const
    {
        return type_ == Type::object;
    }
    bool is_array() const
    {
        return type_ == Type::array;
    }

    // Object member, or a null value when absent.
    const Json &operator[](std::string_view key) const;
    // Array element, or a null value when out of range.
    const Json &operator[](std::size_t index) const;
    std::size_t size() const
    {
        return type_ == Type::array ? items_.size() : type_ == Type::object ? keys_.size() : 0;
    }
    const std::vector<Json> &items() const
    {
        return items_;
    }
    const std::vector<std::string> &keys() const
    {
        return keys_;
    }

    std::string str(std::string_view fallback = {}) const;
    double num(double fallback = 0.0) const;
    std::int64_t i64(std::int64_t fallback = 0) const;
    bool boolean(bool fallback = false) const;

  private:
    friend class Parser;
    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;            // array items, or object values
    std::vector<std::string> keys_;      // object keys, parallel to items_
};

// Writing: small helpers for request bodies.
std::string json_quote(std::string_view value);

} // namespace gv
