# Configuration architecture

Meteoris keeps C++ configuration metadata declarative and human-readable. The
central rule is that a parameter's static configuration definition is written
once, in `conf/parameters.toml`, and generated C++ consumes that definition.

## Files and responsibilities

```text
conf/
├── parameters.toml       authoritative C++ configuration schema
├── generate_config.py    schema-to-C++ build generator
├── meteoris.toml         receiver reference operating profile
├── meteoris_sim.toml     simulator reference operating profile
└── meteoris_plot.toml    Python plotter configuration
```

`conf/parameters.toml` defines the C++ recorder/simulator parameter paths,
scalar types, fallback defaults, ranges, choices, descriptions, ownership
metadata, and declarative cross-parameter constraints.

The other TOML files are editable profiles/templates. Their values are explicit
operating choices, not a second source of fallback defaults. A profile may
therefore contain a value different from the corresponding schema default.

`meteoris_plot.toml` belongs to the Python plotter and is not part of the C++
parameter schema.

## Effective configuration

For the C++ application, values are resolved with later sources overriding
earlier ones:

```text
conf/parameters.toml schema defaults
        < selected TOML configuration
        < command-line overrides
```

If a key is missing from a selected TOML file, the generated schema default is
used. Unknown keys are rejected rather than silently ignored.

This distinction is intentional. For example, changing a future schema default
can affect installations that never overrode that key, while an explicit value
in a site TOML remains an explicit operator choice.

## Build-time generation

CMake treats the schema as an input and runs:

```text
conf/parameters.toml
        |
        v
conf/generate_config.py
        |
        v
<build>/generated/config_schema.hpp
        |
        +--> meteoris C++ configuration/default accessors
        +--> detector metadata/defaults
        +--> simulator defaults
        +--> static validation metadata
```

`generated/config_schema.hpp` is a build artifact. It must not be manually
edited or promoted to another source definition.

The source C++ should not repeat schema defaults, static ranges, choices, or
parameter descriptions. C++ remains responsible for behavior and validation
that genuinely depends on runtime state such as selected hardware or computed
DSP geometry.

## Parameter entries

A typical schema entry is:

```toml
# Initial gain. When AGC is enabled this is the starting value.
[[parameter]]
path = "sdr.gain"
type = "float"
default = -1.0
description = "Initial gain. When AGC is enabled this is the starting value."
```

Entries may additionally define static limits or choices. The generator turns
these declarations into typed C++ metadata and accessors.

Descriptions are kept brief enough for programmatic use. Longer explanatory
comments from `meteoris.toml` and `meteoris_sim.toml` are also retained beside
the matching entries in `parameters.toml`, so configuration knowledge remains
visible at the authoritative definition without removing it from the
human-facing profiles.

Comments from `meteoris_plot.toml` are retained in a clearly marked source-only
appendix in `parameters.toml`; those keys are not emitted as C++ parameters.

## Cross-parameter constraints

Rules involving more than one setting are declared in the same schema when the
rule is static. For example:

```toml
[[constraint]]
name = "recording_detector_dependency"
expression = "recording.mode != \"triggered\" || detector.enabled"
description = "Triggered recording requires the detector to be enabled."
```

and:

```toml
[[constraint]]
name = "bandwidth_below_output_rate"
expression = "dsp.bandwidth_hz < sdr.sample_rate / (dsp.decimation_stage1 * dsp.decimation_stage2)"
description = "Processed bandwidth must be smaller than the final output sample rate."
```

This keeps static relationships out of scattered hand-written C++ checks.
Validation that requires live device capabilities, derived FFT/bin geometry, or
other runtime information remains in the relevant C++ component.

## Detector configuration

Detector implementations remain compile-time C++ plugins selected at runtime by
`detector.plugin`, but they no longer own duplicate hand-written default/schema
tables.

The schema defines common detector settings and plugin-specific settings. The
generated metadata supplies defaults, descriptions, limits, and declarative
constraints. A detector implementation owns only its private typed state,
conversion from the generic configuration view, detector behavior, and
runtime/geometry-dependent checks.

This also allows configurations to contain settings for multiple compiled-in
detectors while preserving unknown-key detection.

## Simulator configuration

The Soapy simulator consumes the same generated configuration schema as the
main C++ application. Simulator defaults therefore come from
`conf/parameters.toml`; `conf/meteoris_sim.toml` remains a readable reference
profile describing a useful synthetic operating scenario.

## Adding or changing a parameter

For a C++ recorder, detector, network, or simulator setting:

1. Add or modify the `[[parameter]]` entry in `conf/parameters.toml`.
2. Put the default, type, static limits/choices, description, and applicable
   declarative constraints there.
3. Preserve/add explanatory comments in the appropriate reference profile and
   mirror useful profile comments beside the schema entry.
4. Add or update the profile value in `conf/meteoris.toml` or
   `conf/meteoris_sim.toml` when the reference operating profile should expose
   it.
5. Consume generated metadata/accessors in C++; do not restate the same default
   or static range in source code.
6. Keep only runtime-dependent validation in C++.
7. Run the repository tests and schema-generation checks.

For a Python-plotter-only option, update `conf/meteoris_plot.toml` and the
plotter code; do not create a C++ schema parameter unless the C++ application
also consumes it.

## Why profiles and defaults are separate

The reference TOMLs are designed for people: they show complete, commented,
useful operating configurations. Schema defaults are designed to define what
the program does when a key is omitted. Treating these as separate concepts
avoids accidental behavior changes and lets a site configuration contain only
the overrides it actually intends to own.

For maintenance details see `conf/README.md`. For every user-facing parameter
and the current receiver reference values see `doc/CONFIG_FILE_REFERENCE.md`.
