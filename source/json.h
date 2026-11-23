#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Null; }
    bool isArray() const { return m_type == Array; }
    bool isObject() const { return m_type == Object; }

    const Json* get(const char* key) const;
    const Json* at(size_t i) const;
    size_t size() const { return m_children.size(); }

    std::string string(const std::string& def = "") const { return m_type == String ? m_str : def; }
    double number(double def = 0) const { return m_type == Number ? m_num : def; }
    long long integer(long long def = 0) const { return m_type == Number ? (long long)m_num : def; }
    bool boolean(bool def = false) const { return m_type == Bool ? m_bool : def; }

private:
    friend struct JsonParser;
    Type m_type = Null;
    bool m_bool = false;
    double m_num = 0;
    std::string m_str;
    std::vector<Json> m_children;
    std::vector<std::string> m_keys;
};
