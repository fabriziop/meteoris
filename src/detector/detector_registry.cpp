#include "detector.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace meteoris {
namespace detector {

std::vector<ConfigField> peakTrackerSchema();
void validatePeakTrackerConfig(const Config &, const Environment &);
std::unique_ptr<IDetector> makePeakTrackerDetector(const Config &, const Environment &);

std::vector<ConfigField> echoesAutomaticSchema();
void validateEchoesAutomaticConfig(const Config &, const Environment &);
Requirements echoesAutomaticRequirements(const Config &);
std::unique_ptr<IDetector> makeEchoesAutomaticDetector(const Config &, const Environment &);

namespace {
std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string unquote(const std::string &s)
{
    const std::string t = trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                          (t.front() == '\'' && t.back() == '\'')))
        return t.substr(1, t.size() - 2);
    return t;
}
} // namespace

void Config::set(const std::string &key, const std::string &rawValue)
{
    _values[key] = trim(rawValue);
}

bool Config::has(const std::string &key) const
{
    return _values.find(key) != _values.end();
}

std::string Config::raw(const std::string &key, const std::string &defaultValue) const
{
    const auto it = _values.find(key);
    return it == _values.end() ? defaultValue : it->second;
}

std::string Config::stringValue(const std::string &key, const std::string &defaultValue) const
{
    const auto it = _values.find(key);
    return it == _values.end() ? defaultValue : unquote(it->second);
}

bool Config::boolValue(const std::string &key, bool defaultValue) const
{
    const auto it = _values.find(key);
    if (it == _values.end()) return defaultValue;
    const std::string v = unquote(it->second);
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    throw std::runtime_error("detector." + key + " must be a boolean");
}

size_t Config::sizeValue(const std::string &key, size_t defaultValue) const
{
    const auto it = _values.find(key);
    if (it == _values.end()) return defaultValue;
    try { return static_cast<size_t>(std::stoull(trim(it->second))); }
    catch (...) { throw std::runtime_error("detector." + key + " must be an unsigned integer"); }
}

double Config::doubleValue(const std::string &key, double defaultValue) const
{
    const auto it = _values.find(key);
    if (it == _values.end()) return defaultValue;
    try { return std::stod(trim(it->second)); }
    catch (...) { throw std::runtime_error("detector." + key + " must be numeric"); }
}

std::vector<std::string> pluginNames()
{
    return {"peak_tracker", "echoes_automatic"};
}

std::vector<ConfigField> schema(const std::string &plugin)
{
    if (plugin == "peak_tracker") return peakTrackerSchema();
    if (plugin == "echoes_automatic") return echoesAutomaticSchema();
    throw std::runtime_error("unknown detector plugin: " + plugin);
}

void validate(const Selection &selection, const Environment &environment)
{
    // Config files may contain sections for several compiled-in plugins. Reject
    // keys that are not declared by any schema so detector option typos do not
    // become silently ignored raw values.
    for (const auto &kv : selection.config.values())
    {
        bool known = false;
        for (const std::string &plugin : pluginNames())
        {
            const std::vector<ConfigField> fields = schema(plugin);
            for (const ConfigField &field : fields)
            {
                if (kv.first == field.key) { known = true; break; }
            }
            if (known) break;
        }
        if (!known)
            throw std::runtime_error("unknown detector configuration key: detector." + kv.first);
    }
    if (selection.plugin == "peak_tracker")
        return validatePeakTrackerConfig(selection.config, environment);
    if (selection.plugin == "echoes_automatic")
        return validateEchoesAutomaticConfig(selection.config, environment);
    throw std::runtime_error("unknown detector plugin: " + selection.plugin);
}

Requirements requirements(const Selection &selection, const Environment &environment)
{
    validate(selection, environment);
    if (selection.plugin == "echoes_automatic")
        return echoesAutomaticRequirements(selection.config);
    return Requirements();
}

std::unique_ptr<IDetector> create(const Selection &selection,
                                  const Environment &environment)
{
    validate(selection, environment);
    if (selection.plugin == "peak_tracker")
        return makePeakTrackerDetector(selection.config, environment);
    if (selection.plugin == "echoes_automatic")
        return makeEchoesAutomaticDetector(selection.config, environment);
    throw std::runtime_error("unknown detector plugin: " + selection.plugin);
}

} // namespace detector
} // namespace meteoris
