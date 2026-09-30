#pragma once

#include <cctype>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace toml {

struct KeyValue {
    std::string key;
    std::string raw_value;
    std::size_t line = 0;
};

inline std::string trim_copy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

inline std::string strip_inline_comment(const std::string& line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
            quoted = !quoted;
            continue;
        }
        if (line[i] == '#' && !quoted) {
            return line.substr(0, i);
        }
    }
    return line;
}

inline std::string unquote_copy(const std::string& value) {
    const std::string trimmed = trim_copy(value);
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '"' && trimmed.back() == '"') ||
         (trimmed.front() == '\'' && trimmed.back() == '\''))) {
        return trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

inline std::vector<KeyValue> parse_entries_from_stream(std::istream& input, const std::string& source_name) {
    std::vector<KeyValue> out;
    std::string section;
    std::string line;
    std::size_t line_no = 0;

    while (std::getline(input, line)) {
        ++line_no;
        std::string cleaned = trim_copy(strip_inline_comment(line));
        if (cleaned.empty()) {
            continue;
        }

        if (cleaned.front() == '[' && cleaned.back() == ']') {
            section = trim_copy(cleaned.substr(1, cleaned.size() - 2));
            if (section.empty()) {
                throw std::runtime_error(source_name + ":" + std::to_string(line_no) + ": empty section header");
            }
            continue;
        }

        const std::size_t eq = cleaned.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error(source_name + ":" + std::to_string(line_no) + ": expected key = value");
        }

        const std::string key = trim_copy(cleaned.substr(0, eq));
        const std::string raw = trim_copy(cleaned.substr(eq + 1));
        if (key.empty()) {
            throw std::runtime_error(source_name + ":" + std::to_string(line_no) + ": empty key");
        }

        KeyValue kv;
        kv.key = section.empty() ? key : (section + "." + key);
        kv.raw_value = raw;
        kv.line = line_no;
        out.push_back(kv);
    }

    return out;
}

inline std::vector<KeyValue> parse_entries_from_file(const std::string& path) {
    std::ifstream input(path.c_str());
    if (!input) {
        throw std::runtime_error("cannot open config file: " + path);
    }
    return parse_entries_from_stream(input, path);
}

inline std::vector<KeyValue> parse_entries_from_text(const std::string& text, const std::string& source_name) {
    std::istringstream input(text);
    return parse_entries_from_stream(input, source_name);
}

}  // namespace toml
