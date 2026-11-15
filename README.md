# TOTK PC Recomp Auto Build

Single standalone executable — zero runtime dependencies.

## Table of Contents

- [Usage](#usage)
- [What it does](#what-it-does)
- [Extra Steps](#extra-steps)
- [Building from Source](#building-from-source)
- [Notes](#notes)

## Usage

```text
totk-auto-build [-h, --help] [-v, --version] [<folder>]
  -h, --help
    Display this help message and exit.

  -v, --version
    Display the version number and exit.

  <folder>
    Folder containing the TOTK base NSP/XCI, update NSP
    and prod.keys.

    Optionally contains a firmware/ directory
    holding firmware NCAs.

    Drag and drop the folder onto the executable
    to start the automatic rebuild.
```

### Full Example

```shell
totk-auto-build "./game files"
```

# What it does

### First run & file validation

- Fetches the latest release from [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (`windows.zip` on Windows, `.AppImage` on Linux) and extracts it into \_setup next to totk-auto-build. Subsequent runs check `_setup/VERSION` and skips downloading if up to date.
- Inspects the provided folder using hactool and prod.keys to verify matching Title IDs for the Base NSP/XCI and Update NSP.
  - Accepted updates are **1.1.0**, **1.2.1** and **1.4.3**, as per <ins>valentderah/totk-pc-recomp</ins> documentation.
  - Automatically selects the latest accepted update if multiple are present.
- Temporarily extracts `_setup/_auto_build.py` and runs it using the bundled Python runtime (`_setup/python/python.exe` on Windows or `_setup/python/bin/python3` on Linux), removing the script once finished.

> [!TIP]
> Running `totk-auto-build` without \<folder> (double-click) updates `_setup` if needed and calls `installer.gui` in rebuild mode for the executable's directory.
>
> ```shell
> python -m installer.gui --rebuild
> ```

### auto-build.py

This does not modify any files from <ins>valentderah/totk-pc-recomp</ins>. Instead, it imports `installer.gui` and populates the following fields before triggering the setup:

```python
app.vars["dest"].set(...)
app.vars["base"].set(...)
app.vars["update"].set(...)
app.vars["keys"].set(...)
app.vars["firmware"].set(...)
```

Each value is supplied by `totk-auto-build`:

- `dest` - the directory where `totk-auto-build` is located
- `base` - path to the Base NSP/XCI
- `update` - path to the selected Update NSP
- `keys` - path to `prod.keys`
- `firmware` - path to the `firmware/` directory holding firmware NCAs (if present)

> [!NOTE]
> When `auto-build.py` runs, a `totk-install.json` is created in `dest` so TOTK Setup can build in a non-empty folder:
>
> ```json
> {
>   "inputs": {},
>   "steps": {}
> }
> ```

# Extra Steps

- To include mods on the initial build without needing to rebuild again later, place a `mods/` folder next to `totk-auto-build`:
- For example:

```text
.
└── mods/
    └── MyCheats/
        └── Cheats/
            └── 277178B7DBA1B6D4.txt <-- contains Atmosphere cheats for 1.4.3
```

# Notes

- This repository and its releases do not contain, distribute, or supply means of obtaining any game files, keys, or firmware files.
- I do not own or modify the work done by the developers of [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp). All credits and thanks go to them for creating it.
