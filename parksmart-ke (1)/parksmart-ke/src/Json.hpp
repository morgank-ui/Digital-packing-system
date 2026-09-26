// ============================================================================
//  Json.hpp  -  A tiny JSON *writer* (no parsing needed).
//
//  The web front-end talks to the server with ordinary form fields
//  (application/x-www-form-urlencoded), so the server only has to PRODUCE JSON.
//  Keeping this ~60 lines long avoids pulling in a third-party JSON library.
// ============================================================================
#pragma once

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace json {

// Escape a string so it is safe inside a JSON string literal.
inline std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {  // other control characters -> \u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Join already-serialised JSON values into an array:  [a,b,c]
inline std::string array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        out += items[i];
    }
    return out + "]";
}

// Fluent builder for a JSON object:  Object().set("a", 1).set("b", "x").dump()
class Object {
public:
    // Strings
    Object& set(const std::string& key, const std::string& value) {
        return raw(key, "\"" + escape(value) + "\"");
    }
    Object& set(const std::string& key, const char* value) {
        return set(key, std::string(value));
    }
    // Integers and booleans
    template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
    Object& set(const std::string& key, T value) {
        if constexpr (std::is_same_v<T, bool>) {
            return raw(key, value ? "true" : "false");
        } else {
            return raw(key, std::to_string(value));
        }
    }
    // Insert an already-serialised JSON value (nested object, array, null ...)
    Object& raw(const std::string& key, const std::string& rawJson) {
        fields_.emplace_back(key, rawJson);
        return *this;
    }

    std::string dump() const {
        std::string out = "{";
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            if (i) out += ',';
            out += "\"" + escape(fields_[i].first) + "\":" + fields_[i].second;
        }
        return out + "}";
    }

private:
    std::vector<std::pair<std::string, std::string>> fields_;
};

}  // namespace json
