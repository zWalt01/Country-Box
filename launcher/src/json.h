// Country-Box launcher: a small JSON reader and writer helpers, enough for the game's --settings-dump / --settings-set
// output (settings.c) and the launcher's own command-line answers.
#pragma once
#include <string>
#include <vector>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::string> keys;   // an object's keys, in document order; its values are arr

    const Json &operator[](const char *key) const;   // a Null when missing or not an object
    const Json &operator[](size_t i) const;          // a Null when out of range
    bool is_null() const { return type == Null; }
    std::string s(const char *def = "") const { return type == String ? str : def; }
    double n(double def = 0) const { return type == Number ? num : def; }
    bool truthy() const { return type == Bool ? b : type != Null; }
    size_t size() const { return type == Array || type == Object ? arr.size() : 0; }
};

// Parses text; false (with err set) on a syntax error.
bool json_parse(const std::string &text, Json &out, std::string *err = nullptr);
// A JSON string literal of s (quotes included).
std::string json_quote(const std::string &s);
