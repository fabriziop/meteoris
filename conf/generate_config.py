#!/usr/bin/env python3
"""Generate C++ configuration metadata from conf/parameters.toml.

parameters.toml is the only hand-maintained source for C++ defaults and detector
schema metadata.  This generator deliberately has no third-party dependencies.
"""
from __future__ import annotations

import argparse
import ast
import math
import re
import tomllib
from pathlib import Path
from typing import Any


def cpp_string(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def toml_scalar(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return cpp_string(value)
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("non-finite schema default")
        text = repr(value)
        return text if any(c in text for c in ".eE") else text + ".0"
    raise TypeError(f"unsupported TOML scalar: {type(value)!r}")


def ident(path: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", path).strip("_")



def _attribute_path(node: ast.AST) -> str | None:
    parts: list[str] = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


def compile_expression(expression: str, types: dict[str, str]) -> str:
    normalized = expression.replace("&&", " and ").replace("||", " or ")
    normalized = re.sub(r"!(?!=)", " not ", normalized).strip()
    tree = ast.parse(normalized, mode="eval")

    def emit(node: ast.AST) -> str:
        path = _attribute_path(node)
        if path is not None and path in types:
            typ = types[path]
            if typ == "boolean": return f"v.boolean({cpp_string(path)})"
            if typ == "string": return f"v.stringValue({cpp_string(path)})"
            if typ == "integer": return f"v.integer({cpp_string(path)})"
            return f"v.number({cpp_string(path)})"
        if isinstance(node, ast.Constant):
            if isinstance(node.value, str): return cpp_string(node.value)
            if isinstance(node.value, bool): return "true" if node.value else "false"
            if isinstance(node.value, (int, float)): return repr(node.value)
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.Not): return "(!(" + emit(node.operand) + "))"
            if isinstance(node.op, ast.USub): return "(-(" + emit(node.operand) + "))"
            if isinstance(node.op, ast.UAdd): return "(+(" + emit(node.operand) + "))"
        if isinstance(node, ast.BoolOp):
            op = " && " if isinstance(node.op, ast.And) else " || "
            return "(" + op.join("(" + emit(x) + ")" for x in node.values) + ")"
        if isinstance(node, ast.BinOp):
            ops = {ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/", ast.Mod: "%"}
            op = next((symbol for cls, symbol in ops.items() if isinstance(node.op, cls)), None)
            if op is None: raise ValueError(f"unsupported operator in constraint: {expression}")
            return "((" + emit(node.left) + ") " + op + " (" + emit(node.right) + "))"
        if isinstance(node, ast.Compare):
            ops = {ast.Lt: "<", ast.LtE: "<=", ast.Gt: ">", ast.GtE: ">=", ast.Eq: "==", ast.NotEq: "!="}
            pieces=[]; left=node.left
            for opnode,right in zip(node.ops,node.comparators):
                op=next((symbol for cls,symbol in ops.items() if isinstance(opnode,cls)),None)
                if op is None: raise ValueError(f"unsupported comparison in constraint: {expression}")
                pieces.append("(("+emit(left)+") "+op+" ("+emit(right)+"))")
                left=right
            return "("+" && ".join(pieces)+")"
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "is_power_of_two" and len(node.args)==1:
            return "isPowerOfTwo(static_cast<unsigned long long>(" + emit(node.args[0]) + "))"
        raise ValueError(f"unsupported constraint expression node {ast.dump(node)} in {expression!r}")

    return emit(tree.body)


def generate(schema_path: Path, output_path: Path) -> None:
    data = tomllib.loads(schema_path.read_text(encoding="utf-8"))
    params = data.get("parameter", [])
    constraints = data.get("constraint", [])
    seen: set[str] = set()
    for p in params:
        path = p["path"]
        if path in seen:
            raise ValueError(f"duplicate parameter: {path}")
        seen.add(path)
        typ = p["type"]
        value = p["default"]
        if typ == "boolean" and not isinstance(value, bool):
            raise ValueError(f"{path}: boolean default required")
        if typ == "string" and not isinstance(value, str):
            raise ValueError(f"{path}: string default required")
        if typ == "integer" and (not isinstance(value, int) or isinstance(value, bool)):
            raise ValueError(f"{path}: integer default required")
        if typ == "float" and (not isinstance(value, (int, float)) or isinstance(value, bool)):
            raise ValueError(f"{path}: numeric default required")

    types = {p["path"]: p["type"] for p in params}
    compiled_constraints = [(c, compile_expression(c["expression"], types)) for c in constraints]

    lines: list[str] = []
    lines += [
        "// Generated by conf/generate_config.py from conf/parameters.toml.",
        "// Do not edit by hand.",
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <cctype>",
        "#include <cstddef>",
        "#include <map>",
        "#include <stdexcept>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace meteoris_config_schema {",
        "",
        "struct Parameter {",
        "    const char *path;",
        "    const char *type;",
        "    const char *defaultRaw;",
        "    const char *description;",
        "    bool hasMin;",
        "    double minValue;",
        "    bool hasMax;",
        "    double maxValue;",
        "    const char *choices;",
        "    const char *plugins;",
        "};",
        "",
        "struct Constraint {",
        "    const char *name;",
        "    const char *expression;",
        "    const char *description;",
        "};",
        "",
        "inline const Parameter *parameters(std::size_t &count)",
        "{",
        "    static const Parameter values[] = {",
    ]
    for p in params:
        choices = "|".join(str(x) for x in p.get("choices", []))
        plugins = "|".join(str(x) for x in p.get("plugins", []))
        lines.append(
            "        {%s, %s, %s, %s, %s, %s, %s, %s, %s, %s}," % (
                cpp_string(p["path"]), cpp_string(p["type"]), cpp_string(toml_scalar(p["default"])),
                cpp_string(p.get("description", "")),
                "true" if "min" in p else "false", repr(float(p.get("min", 0.0))),
                "true" if "max" in p else "false", repr(float(p.get("max", 0.0))),
                cpp_string(choices), cpp_string(plugins),
            )
        )
    lines += [
        "    };",
        "    count = sizeof(values) / sizeof(values[0]);",
        "    return values;",
        "}",
        "",
        "inline const Constraint *constraints(std::size_t &count)",
        "{",
        "    static const Constraint values[] = {",
    ]
    for c in constraints:
        lines.append("        {%s, %s, %s}," % (cpp_string(c["name"]), cpp_string(c["expression"]), cpp_string(c["description"])))
    lines += [
        "    };",
        "    count = sizeof(values) / sizeof(values[0]);",
        "    return values;",
        "}",
        "",
        "inline const Parameter &parameter(const std::string &path)",
        "{",
        "    std::size_t count = 0;",
        "    const Parameter *items = parameters(count);",
        "    for (std::size_t i = 0; i < count; ++i)",
        "        if (path == items[i].path) return items[i];",
        "    throw std::runtime_error(\"unknown configuration key: \" + path);",
        "}",
        "",
        "inline std::string unquote(const std::string &raw)",
        "{",
        "    if (raw.size() >= 2 && ((raw.front() == '\"' && raw.back() == '\"') ||",
        "                            (raw.front() == '\\'' && raw.back() == '\\'')))",
        "        return raw.substr(1, raw.size() - 2);",
        "    return raw;",
        "}",
        "",
        "inline bool defaultBool(const char *path)",
        "{",
        "    const std::string raw = parameter(path).defaultRaw;",
        "    if (raw == \"true\") return true;",
        "    if (raw == \"false\") return false;",
        "    throw std::runtime_error(std::string(path) + \" schema default is not boolean\");",
        "}",
        "inline long long defaultInteger(const char *path) { return std::stoll(parameter(path).defaultRaw); }",
        "inline std::size_t defaultSize(const char *path) { return static_cast<std::size_t>(std::stoull(parameter(path).defaultRaw)); }",
        "inline double defaultDouble(const char *path) { return std::stod(parameter(path).defaultRaw); }",
        "inline std::string defaultString(const char *path) { return unquote(parameter(path).defaultRaw); }",
        "inline int defaultHhMmMinutes(const char *path)",
        "{",
        "    const std::string value = defaultString(path);",
        "    if (value.size() != 5 || value[2] != ':') throw std::runtime_error(std::string(path) + \" schema default must be HH:MM\");",
        "    const int h = (value[0] - '0') * 10 + (value[1] - '0');",
        "    const int m = (value[3] - '0') * 10 + (value[4] - '0');",
        "    if (h < 0 || h > 23 || m < 0 || m > 59) throw std::runtime_error(std::string(path) + \" schema default must be HH:MM\");",
        "    return h * 60 + m;",
        "}",
        "",
        "inline bool listContains(const std::string &list, const std::string &value)",
        "{",
        "    std::size_t start = 0;",
        "    while (start <= list.size()) {",
        "        const std::size_t end = list.find('|', start);",
        "        if (list.substr(start, end == std::string::npos ? std::string::npos : end - start) == value) return true;",
        "        if (end == std::string::npos) break;",
        "        start = end + 1;",
        "    }",
        "    return false;",
        "}",
        "",
        "inline void validateRaw(const std::string &path, const std::string &raw)",
        "{",
        "    const Parameter &p = parameter(path);",
        "    try {",
        "        if (std::string(p.type) == \"boolean\") {",
        "            if (raw != \"true\" && raw != \"false\") throw std::runtime_error(\"boolean expected\");",
        "            return;",
        "        }",
        "        if (std::string(p.type) == \"string\") {",
        "            const std::string value = unquote(raw);",
        "            if (*p.choices && !listContains(p.choices, value)) throw std::runtime_error(\"unsupported choice\");",
        "            return;",
        "        }",
        "        double value = 0.0;",
        "        if (std::string(p.type) == \"integer\") {",
        "            std::size_t used = 0;",
        "            value = static_cast<double>(std::stoll(raw, &used));",
        "            if (used != raw.size()) throw std::runtime_error(\"integer expected\");",
        "        } else {",
        "            std::size_t used = 0;",
        "            value = std::stod(raw, &used);",
        "            if (used != raw.size()) throw std::runtime_error(\"number expected\");",
        "        }",
        "        if (p.hasMin && value < p.minValue) throw std::runtime_error(\"below minimum\");",
        "        if (p.hasMax && value > p.maxValue) throw std::runtime_error(\"above maximum\");",
        "    } catch (const std::exception &e) {",
        "        throw std::runtime_error(path + \" violates schema (\" + e.what() + \")\");",
        "    }",
        "}",
        "",
        "inline bool isPowerOfTwo(unsigned long long value) { return value != 0 && (value & (value - 1)) == 0; }",
        "",
        "class Values",
        "{",
        "public:",
        "    Values()",
        "    {",
        "        std::size_t count = 0;",
        "        const Parameter *items = parameters(count);",
        "        for (std::size_t i = 0; i < count; ++i) _raw[items[i].path] = items[i].defaultRaw;",
        "    }",
        "    void setRaw(const std::string &path, const std::string &raw) { validateRaw(path, raw); _raw[path] = raw; }",
        "    double number(const std::string &path) const { return std::stod(get(path)); }",
        "    long long integer(const std::string &path) const { return std::stoll(get(path)); }",
        "    bool boolean(const std::string &path) const",
        "    {",
        "        const std::string raw = get(path);",
        "        if (raw == \"true\") return true;",
        "        if (raw == \"false\") return false;",
        "        throw std::runtime_error(path + \" is not boolean\");",
        "    }",
        "    std::string stringValue(const std::string &path) const { return unquote(get(path)); }",
        "private:",
        "    const std::string &get(const std::string &path) const",
        "    {",
        "        const std::map<std::string, std::string>::const_iterator it = _raw.find(path);",
        "        if (it == _raw.end()) throw std::runtime_error(\"unknown configuration key: \" + path);",
        "        return it->second;",
        "    }",
        "    std::map<std::string, std::string> _raw;",
        "};",
        "",
        "inline void validateConstraints(const Values &v)",
        "{",
        "inline std::vector<const Parameter *> pluginParameters(const std::string &plugin)",
        "{",
        "    std::vector<const Parameter *> out;",
        "    std::size_t count = 0;",
        "    const Parameter *items = parameters(count);",
        "    for (std::size_t i = 0; i < count; ++i)",
        "        if (*items[i].plugins && listContains(items[i].plugins, plugin)) out.push_back(&items[i]);",
        "    return out;",
        "}",
        "",
    ]
    # Complete declarative cross-parameter validation generated from schema expressions.
    constraint_lines: list[str] = []
    for c, compiled in compiled_constraints:
        constraint_lines.append(f"    if (!({compiled})) throw std::runtime_error({cpp_string(c['description'])});")
    constraint_lines += ["}", ""]
    # The static list above opened validateConstraints immediately before pluginParameters;
    # splice the generated checks and closing brace at that location.
    plugin_marker = "inline std::vector<const Parameter *> pluginParameters(const std::string &plugin)"
    idx = lines.index(plugin_marker)
    lines[idx:idx] = constraint_lines

    # Named accessors make C++ call sites readable while keeping values generated.
    for p in params:
        name = ident(p["path"])
        typ = p["type"]
        if typ == "boolean": ret, fun = "bool", "defaultBool"
        elif typ == "string": ret, fun = "std::string", "defaultString"
        elif typ == "integer": ret, fun = "long long", "defaultInteger"
        else: ret, fun = "double", "defaultDouble"
        lines.append(f"inline {ret} default_{name}() {{ return {fun}({cpp_string(p['path'])}); }}")
    lines += ["", "} // namespace meteoris_config_schema", ""]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("schema", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    generate(args.schema, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
