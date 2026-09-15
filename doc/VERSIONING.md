# Meteoris versioning

Meteoris uses a single authoritative package version:

```text
VERSION
```

The file contains one semantic version (`MAJOR.MINOR.PATCH`). A release version
change must edit this file only.

## Consumers

CMake reads `VERSION` before the `project()` call and assigns it to
`PROJECT_VERSION`. During configuration it generates `generated/version.hpp`
from `src/version.hpp.in`; the `meteoris` executable uses that header for
`--version`, logging, and the HDF5 `software_version` attribute.

The Python commands (`meteoris_plot`, `meteoris_config`, and
`meteoris_recover_hdf5`) read the same `VERSION` file at runtime. They support
three layouts:

- source tree: repository-root `VERSION`;
- CMake build tree: CMake copies `VERSION` beside build-tree tools;
- installed tree: CMake installs it as `share/meteoris/VERSION` under the same
  prefix as `bin/`.

All user-facing commands therefore report the same package version:

```bash
meteoris --version
meteoris_plot --version
meteoris_config --version
meteoris_recover_hdf5 --version
```

## Release procedure

1. Edit only the root `VERSION` file.
2. Reconfigure/rebuild with CMake.
3. Run the version consistency tests.
4. Verify the installed commands above report the new value.

Do not add the current release number to source files or general documentation.
Historical release notes may name old versions when that historical value is
part of the information being documented.

## Consistency test

`tests/version_consistency_test.py` checks that `VERSION` is valid semantic
version syntax, CMake consumes it, C++ uses the generated header, and the
current release string has not been copied back into sources or documentation.
