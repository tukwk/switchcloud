#include "json.h"
#include <cstdlib>
#include <cstring>
#include <cmath>

struct JsonParser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    }

    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(uint32_t& out) {
        if (end - p < 4) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (uint32_t)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return fail("expected string");
        p++;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control char in string");
            if (c != '\\') { out += c; continue; }
            if (p >= end) return fail("bad escape");
            char e = *p++;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        const char* save = p;
                        p += 2;
                        uint32_t lo;
                        if (!parseHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p = save;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parseValue(Json& v) {
        if (++depth > 64) return fail("too deep");
        skipWs();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        bool ok = false;
        if (c == '{') ok = parseObject(v);
        else if (c == '[') ok = parseArray(v);
        else if (c == '"') {
            v.m_type = Json::String;
            ok = parseString(v.m_str);
        } else if (c == 't') {
            if (end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; v.m_type = Json::Bool; v.m_bool = true; ok = true; }
            else return fail("bad token");
        } else if (c == 'f') {
            if (end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; v.m_type = Json::Bool; v.m_bool = false; ok = true; }
            else return fail("bad token");
        } else if (c == 'n') {
            if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; v.m_type = Json::Null; ok = true; }
            else return fail("bad token");
        } else {
            char* endp = nullptr;
            v.m_num = strtod(p, &endp);
            if (endp == p) return fail("bad number");
            p = endp;
            v.m_type = Json::Number;
            ok = true;
        }
        depth--;
        return ok;
    }

    bool parseArray(Json& v) {
        v.m_type = Json::Array;
        p++; // [
        skipWs();
        if (p < end && *p == ']') { p++; return true; }
        while (p < end) {
            v.m_children.emplace_back();
            if (!parseValue(v.m_children.back())) return false;
            skipWs();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return fail("expected , or ]");
        }
        return fail("unterminated array");
    }

    bool parseObject(Json& v) {
        v.m_type = Json::Object;
        p++; // {
        skipWs();
        if (p < end && *p == '}') { p++; return true; }
        while (p < end) {
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (p >= end || *p != ':') return fail("expected :");
            p++;
            v.m_children.emplace_back();
            if (!parseValue(v.m_children.back())) return false;
            v.m_keys.push_back(std::move(key));
            skipWs();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { p++; skipWs(); continue; }
            if (*p == '}') { p++; return true; }
            return fail("expected , or }");
        }
        return fail("unterminated object");
    }
};

bool Json::parse(const std::string& text, Json& out, std::string* error) {
    JsonParser parser{text.data(), text.data() + text.size()};
    out = Json();
    if (!parser.parseValue(out)) {
        if (error) *error = parser.err;
        return false;
    }
    parser.skipWs();
    if (parser.p != parser.end) {
        if (error) *error = "trailing data";
        return false;
    }
    return true;
}

const Json* Json::get(const char* key) const {
    if (m_type != Object) return nullptr;
    for (size_t i = 0; i < m_keys.size(); i++)
        if (m_keys[i] == key) return &m_children[i];
    return nullptr;
}

const Json* Json::at(size_t i) const {
    if (m_type != Array || i >= m_children.size()) return nullptr;
    return &m_children[i];
}
