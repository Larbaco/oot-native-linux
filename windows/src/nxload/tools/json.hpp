// A minimal streaming JSON writer: commas and nesting are tracked, strings escaped (bytes >= 0x80 as \u00XX, Latin-1).
// Rows (an object or array inside an array) and everything near the top start on a new line.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

class Json {
public:
    explicit Json(std::ostream& o) : o_(o) {}
    Json& obj() { return open('{'); }
    Json& arr() { return open('['); }
    Json& end() {
        o_ << closers_.back();
        closers_.pop_back();
        first_.pop_back();
        if (closers_.empty()) o_ << '\n';
        return *this;
    }
    Json& key(std::string_view k) {
        sep(false);
        str(k);
        o_ << ':';
        keyed_ = true;
        return *this;
    }
    Json& val(std::string_view s) { return sep(false), str(s), *this; }
    Json& val(const char* s) { return val(std::string_view(s)); }
    Json& val(bool b) { return sep(false), o_ << (b ? "true" : "false"), *this; }
    Json& val(uint64_t v) { return sep(false), o_ << v, *this; }
    Json& val(int64_t v) { return sep(false), o_ << v, *this; }
    Json& val(uint32_t v) { return val(uint64_t(v)); }
    Json& val(int v) { return val(int64_t(v)); }
    Json& val(double v) { return sep(false), o_ << v, *this; }
    template <class T> Json& kv(std::string_view k, const T& v) { return key(k).val(v); }

private:
    std::ostream& o_;
    std::vector<char> closers_;
    std::vector<bool> first_;
    bool keyed_ = false;

    Json& open(char c) {
        sep(true);
        o_ << c;
        closers_.push_back(c == '{' ? '}' : ']');
        first_.push_back(true);
        return *this;
    }
    void sep(bool opening) {
        if (keyed_) {
            keyed_ = false;
            return;
        }
        if (first_.empty()) return;
        if (!first_.back()) o_ << ',';
        first_.back() = false;
        if (closers_.size() <= 2 || (opening && closers_.back() == ']')) o_ << '\n';
    }
    void str(std::string_view s) {
        o_ << '"';
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') o_ << '\\' << char(c);
            else if (c >= 0x20 && c < 0x80) o_ << char(c);
            else {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o_ << b;
            }
        }
        o_ << '"';
    }
};
