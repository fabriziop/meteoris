# Meteoris configuration files

All configuration-related source files live in this directory.

## Authoritative schema

`parameters.toml` is the single authoritative configuration schema used by the
C++ recorder and simulator. It defines, in human-readable TOML:

- parameter paths and scalar types;
- fallback/default values used when a key is absent from a selected profile;
- scalar ranges and enumerated choices;
- short descriptions and preserved explanatory comments from the reference
  TOML files;
- detector ownership/metadata; and
- declarative cross-parameter constraints.

Do not duplicate these values or schema tables in C++. Configuration behavior
that depends on runtime geometry or device capabilities may still be validated
in C++, but static configuration metadata belongs in `parameters.toml`.

`generate_config.py` reads the schema during the CMake build and writes the
build-tree `generated/config_schema.hpp`. The generated header provides typed
metadata/default accessors and validation data to the C++ code. It is a build
artifact and must not be edited or committed as another source of truth.

The comments copied from the reference TOMLs are intentionally retained in
`parameters.toml` beside their matching parameters. Comments belonging only to
the Python plotter are retained in a clearly marked source-only appendix,
because plotter settings are not part of the C++ schema.

## Human-editable profiles

The other TOML files are human-readable profiles/templates and keep their
comments:

- `meteoris.toml` is the normal receiver reference profile.
- `meteoris_sim.toml` is the simulator reference profile.
- `meteoris_plot.toml` configures the Python plotter and is not part of the C++
  parameter schema.

A profile is not a second definition of defaults. It contains explicit
operating values and may intentionally choose values different from schema
defaults.

For the C++ application, configuration precedence is:

```text
conf/parameters.toml schema defaults
        < selected TOML profile/site file
        < command-line overrides
```

A missing key therefore falls back to `parameters.toml`. Resetting a value to
the application default is conceptually equivalent to removing that override
from the site/profile configuration.

Unknown keys are rejected. Static scalar/range/choice and declarative
cross-parameter checks come from the schema; checks that require runtime DSP
geometry, selected hardware, or other live state remain in C++.

Detector plugins use the same generated schema for their default values,
descriptions, and static limits. Plugin C++ contains detector behavior,
conversion to private typed state, and runtime/geometry-dependent validation,
not duplicate default/schema tables.

## Editing rules

When adding or changing a C++ configuration parameter:

1. edit `parameters.toml` first;
2. add/update the reference profile value and explanatory comments where
   appropriate;
3. consume the generated accessor/metadata from C++ rather than restating the
   default or range; and
4. keep only genuinely runtime-dependent validation in C++.

Run the repository tests after schema changes. CMake regenerates
`generated/config_schema.hpp` automatically when `parameters.toml` or
`generate_config.py` changes.
