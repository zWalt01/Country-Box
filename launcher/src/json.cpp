// Country-Box launcher: JSON (json.h). Recursive descent over RFC 8259; \u escapes become UTF-8.
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const Json kNull;

const Json &Json::operator[](const char *key) const {
    if (type != Object) return kNull;
    for (size_t i = 0; i < keys.size(); i++)
        if (keys[i] == key) return arr[i];
    return kNull;
}

const Json &Json::operator[](size_t i) const { return type == Array && i < arr.size() ? arr[i] : kNull; }

namespace {

struct Parser {
    const char *p, *end;
    std::string err;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool fail(const char *what) {
        if (err.empty()) err = what;
        return false;
    }
    static void utf8(std::string &o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 0x3F)), o += (char)(0x80 | (c & 0x3F));
        else
            o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 0x3F)), o += (char)(0x80 | (c >> 6 & 0x3F)),
                o += (char)(0x80 | (c & 0x3F));
    }
    bool hex4(unsigned &v) {
        if (end - p < 4) return fail("short \\u escape");
        char t[5] = {p[0], p[1], p[2], p[3], 0};
        char *e;
        v = (unsigned)strtoul(t, &e, 16);
        if (e != t + 4) return fail("bad \\u escape");
        p += 4;
        return true;
    }
    bool string(std::string &o) {
        if (p >= end || *p != '"') return fail("expected a string");
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (p >= end) return fail("bad escape");
            c = *p++;
            switch (c) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, v);
                break;
            }
            default: o += c;
            }
        }
        if (p >= end) return fail("unterminated string");
        p++;
        return true;
    }
    bool value(Json &v, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        if (*p == '{') {
            v.type = Json::Object;
            p++;
            ws();
            if (p < end && *p == '}') return p++, true;
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                p++;
                v.keys.push_back(std::move(k));
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') return p++, true;
                return fail("expected ',' or '}'");
            }
        }
        if (*p == '[') {
            v.type = Json::Array;
            p++;
            ws();
            if (p < end && *p == ']') return p++, true;
            for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') return p++, true;
                return fail("expected ',' or ']'");
            }
        }
        if (*p == '"') return v.type = Json::String, string(v.str);
        if (end - p >= 4 && !strncmp(p, "true", 4)) return v.type = Json::Bool, v.b = true, p += 4, true;
        if (end - p >= 5 && !strncmp(p, "false", 5)) return v.type = Json::Bool, v.b = false, p += 5, true;
        if (end - p >= 4 && !strncmp(p, "null", 4)) return v.type = Json::Null, p += 4, true;
        char *e;
        std::string t(p, (size_t)(end - p < 64 ? end - p : 64));
        v.num = strtod(t.c_str(), &e);
        if (e == t.c_str()) return fail("unexpected character");
        v.type = Json::Number;
        p += e - t.c_str();
        return true;
    }
};

}  // namespace

bool json_parse(const std::string &text, Json &out, std::string *err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    out = Json();
    bool ok = ps.value(out, 0);
    if (ok) {
        ps.ws();
        if (ps.p != ps.end) ok = ps.fail("trailing text");
    }
    if (!ok && err) *err = ps.err;
    return ok;
}

std::string json_quote(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += (char)c;
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else o += (char)c;
    }
    return o + "\"";
}
