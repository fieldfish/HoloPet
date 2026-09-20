#pragma once
/**
 * mini_json.hpp — 完整 JSON 解析/序列化 (V6-R2, 纯 C++, 可单测)
 *
 * 替换脆弱的 extractStringField。覆盖:
 *   object/array/string/number/bool/null; 转义 \" \\ \/ \b \f \n \r \t \uXXXX
 *   (含 UTF-16 代理对 → UTF-8); 截断/超长/错误类型 → 结构化协议错误
 * 不崩溃、不越界; 解析失败不产生半成品消息。
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace holopet {

/** 结构化协议错误 */
struct ProtocolError {
    std::string reason;   // 机器可读原因
    size_t      pos = 0;  // 出错位置 (0 = 无位置信息)
};

/** JSON 值 (保序 object) */
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    // ---- 访问器 (缺字段/类型错 → nullopt, 不抛) ----
    const JsonValue* get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::optional<std::string> getString(const std::string& key) const {
        const JsonValue* v = get(key);
        if (v && v->type == Type::String) return v->str;
        return std::nullopt;
    }
    std::optional<double> getNumber(const std::string& key) const {
        const JsonValue* v = get(key);
        if (v && v->type == Type::Number) return v->num;
        return std::nullopt;
    }
    std::optional<bool> getBool(const std::string& key) const {
        const JsonValue* v = get(key);
        if (v && v->type == Type::Bool) return v->b;
        return std::nullopt;
    }
    bool has(const std::string& key) const { return get(key) != nullptr; }
};

/** 解析 JSON 文本 → 值 (失败: nullopt + err) */
std::optional<JsonValue> parseJson(const std::string& text, ProtocolError* err = nullptr);

/** 序列化 → 紧凑 JSON 文本 */
std::string serializeJson(const JsonValue& v);

// ================= 实现 =================

namespace detail {

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    std::optional<JsonValue> parseDocument() {
        auto v = parseValue();
        if (!v) return std::nullopt;
        skipWs();
        if (i_ != s_.size()) return fail("trailing characters after document");
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    std::optional<JsonValue> fail(const std::string& why) {
        err_.reason = why;
        err_.pos = i_;
        return std::nullopt;
    }

    void setError() const { /* err_ filled by fail */ }
    mutable ProtocolError err_;

    void skipWs() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }

    std::optional<JsonValue> parseValue() {
        skipWs();
        if (i_ >= s_.size()) return fail("unexpected end of input");
        char c = s_[i_];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        return fail(std::string("unexpected character '") + c + "'");
    }

    std::optional<JsonValue> parseObject() {
        JsonValue v; v.type = JsonValue::Type::Object;
        ++i_;  // '{'
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        while (true) {
            skipWs();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("object key must be string");
            auto key = parseString();
            if (!key || key->type != JsonValue::Type::String)
                return fail("bad object key");
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
            ++i_;
            auto val = parseValue();
            if (!val) return std::nullopt;
            v.obj.emplace_back(key->str, std::move(*val));
            skipWs();
            if (i_ >= s_.size()) return fail("unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return v; }
            return fail("expected ',' or '}'");
        }
    }

    std::optional<JsonValue> parseArray() {
        JsonValue v; v.type = JsonValue::Type::Array;
        ++i_;  // '['
        skipWs();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        while (true) {
            auto val = parseValue();
            if (!val) return std::nullopt;
            v.arr.push_back(std::move(*val));
            skipWs();
            if (i_ >= s_.size()) return fail("unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return v; }
            return fail("expected ',' or ']'");
        }
    }

    std::optional<JsonValue> parseString() {
        JsonValue v; v.type = JsonValue::Type::String;
        ++i_;  // '"'
        std::string out;
        while (true) {
            if (i_ >= s_.size()) return fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') { ++i_; v.str = std::move(out); return v; }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) return fail("bad escape at end");
                char e = s_[i_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        auto cp = parseHex4();
                        if (!cp) return std::nullopt;
                        std::uint32_t code = *cp;
                        // 代理对
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                                i_ += 2;
                                auto lo = parseHex4();
                                if (!lo) return std::nullopt;
                                if (*lo >= 0xDC00 && *lo <= 0xDFFF) {
                                    code = 0x10000 + ((code - 0xD800) << 10) + (*lo - 0xDC00);
                                } else {
                                    return fail("invalid low surrogate");
                                }
                            } else {
                                return fail("unpaired high surrogate");
                            }
                        } else if (code >= 0xDC00 && code <= 0xDFFF) {
                            return fail("unpaired low surrogate");
                        }
                        appendUtf8(out, code);
                        break;
                    }
                    default:
                        return fail(std::string("bad escape '\\") + e + "'");
                }
                continue;
            }
            if (c < 0x20) return fail("raw control char in string");
            out += static_cast<char>(c);
            ++i_;
        }
    }

    std::optional<std::uint32_t> parseHex4() {
        if (i_ + 4 > s_.size()) { fail("short \\u escape"); return std::nullopt; }
        std::uint32_t code = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            code <<= 4;
            if (c >= '0' && c <= '9')       code |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')  code |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')  code |= static_cast<std::uint32_t>(c - 'A' + 10);
            else { fail("bad hex digit"); return std::nullopt; }
        }
        return code;
    }

    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::optional<JsonValue> parseBool() {
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            JsonValue v; v.type = JsonValue::Type::Bool; v.b = true;
            return v;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            JsonValue v; v.type = JsonValue::Type::Bool; v.b = false;
            return v;
        }
        return fail("bad literal (expected true/false/null)");
    }

    std::optional<JsonValue> parseNull() {
        if (s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
            JsonValue v; v.type = JsonValue::Type::Null;
            return v;
        }
        return fail("bad literal (expected true/false/null)");
    }

    std::optional<JsonValue> parseNumber() {
        size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        if (i_ >= s_.size()) return fail("bad number");
        if (s_[i_] == '0') {
            ++i_;
            if (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
                return fail("leading zero in number");
        } else if (s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        } else {
            return fail("bad number");
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad fraction");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad exponent");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        JsonValue v; v.type = JsonValue::Type::Number;
        try {
            v.num = std::stod(s_.substr(start, i_ - start));
        } catch (...) {
            return fail("number out of range");
        }
        return v;
    }

public:
    const ProtocolError& error() const { return err_; }
};

inline void serializeTo(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Type::Null:
            out += "null"; break;
        case JsonValue::Type::Bool:
            out += v.b ? "true" : "false"; break;
        case JsonValue::Type::Number: {
            if (std::isfinite(v.num)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", v.num);
                out += buf;
            } else {
                out += "null";
            }
            break;
        }
        case JsonValue::Type::String: {
            out += '"';
            for (char c : v.str) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x",
                                          static_cast<unsigned>(static_cast<unsigned char>(c)));
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            out += '"';
            break;
        }
        case JsonValue::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                serializeTo(v.arr[i], out);
            }
            out += ']';
            break;
        case JsonValue::Type::Object:
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                serializeTo(JsonValue{JsonValue::Type::String, false, 0.0, v.obj[i].first, {}, {}}, out);
                out += ':';
                serializeTo(v.obj[i].second, out);
            }
            out += '}';
            break;
    }
}

} // namespace detail

inline std::optional<JsonValue> parseJson(const std::string& text, ProtocolError* err) {
    detail::JsonParser p(text);
    auto v = p.parseDocument();
    if (!v && err) *err = p.error();
    return v;
}

inline std::string serializeJson(const JsonValue& v) {
    std::string out;
    detail::serializeTo(v, out);
    return out;
}

} // namespace holopet
