# `meteoris_config` interactive configuration wizard

`meteoris_config` helps create a complete, commented `meteoris.toml` without
having to remember every setting.

## Starting values

Run it from the directory where Meteoris is configured:

```bash
meteoris_config
```

If `./meteoris.toml` exists, the wizard reads it and proposes its current values
as the defaults shown in the prompts. To start from another file, pass it as the
positional argument:

```bash
meteoris_config /etc/meteoris/site.toml
```

When there is no current file, values and ordering come from the installed
canonical `config/meteoris.toml` template. Missing settings in a partial current
file are also filled from that canonical template.

## Prompt modes

At startup the wizard prints the Meteoris version and the input filename (or
reports that `meteoris.toml` was not found and the canonical template is being
used). It then offers the two modes by number:

```text
Configuration mode:
  1) smart  - ask only essential parameters
  2) expert - ask every parameter
Mode [1]:
```

- **1 / smart** asks only operationally essential parameters. Settings not asked
  are still written using the current value, or the canonical default when
  missing.
- **2 / expert** asks every known parameter.

The mode can also be selected on the command line:

```bash
meteoris_config --mode smart
meteoris_config --mode expert existing.toml
```

## Numbering and explanations

Every parameter is displayed with a `section.parameter` number, for example:

```text
8.11 detector.max_event_seconds
    Maximum event duration. For peak_tracker, an event reaching this limit is
    discarded completely from HDF5. 0 disables the limit.
    Value [15]:
```

When a current TOML is read, known sections and parameters are numbered in the
order in which they appear there. Missing parameters are appended in the order
from the canonical `config/meteoris.toml`. With no current file, canonical order
is used throughout.

The short explanation shown at each prompt is also written into the generated
TOML as a numbered comment.

Press Enter at a value prompt to accept the proposed value. Boolean prompts
accept `true`/`false` and also `yes`/`no`.

## Review, correction, and output

After the questions, the wizard displays the input review. In smart mode it
shows only the parameters that were asked (plus any extra parameter explicitly
modified during review); in expert mode it shows all parameters. It then asks:

```text
Input data OK? [Y/n]:
```

Press Enter or answer `y` to accept the review and continue directly to the
output filename. Answer `n` to enter correction mode:

```text
Parameter number to modify [Enter=review]:
```

Enter the displayed `section.parameter` number (for example `8.11`). The wizard
shows that parameter, its explanation, current value, and asks for the new
value. It then asks for another parameter number. Press Enter with no number to
finish corrections, show the review again, and ask `Input data OK?` again. This
review/edit cycle can be repeated until the values are accepted.

Once accepted, the wizard asks for the output path:

```text
Output TOML [meteoris.toml.new]:
```

Press Enter to use `meteoris.toml.new`. If the selected file already exists,
the wizard asks before overwriting it. The generated file is parsed again as
TOML before success is reported.

The existing input configuration is never overwritten unless the same path is
explicitly selected as the output and overwrite is confirmed.

## Requirements

`meteoris_config` uses only the Python standard library and requires Python
3.11 or newer for `tomllib`.
