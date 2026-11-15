import argparse
import json
import os
from pathlib import Path
import sys

# Ensure _setup is on sys.path so installer modules can be imported.
SETUP_DIR = os.path.dirname(os.path.abspath(__file__))
if SETUP_DIR not in sys.path:
    sys.path.insert(0, SETUP_DIR)

from installer import dpi  # noqa: E402

# Must run before tkinter is imported so Windows sets DPI awareness prior to Tk startup.
dpi.enable()

import tkinter as tk  # noqa: E402

from installer import layout as layouts  # noqa: E402
from installer.gui import App, fix_tcl_paths  # noqa: E402
from installer.guilogic import windows_ui_language  # noqa: E402


def parse_args() -> tuple[str, str, str, str, str]:
    parser = argparse.ArgumentParser(
        description="TOTK PC Recomp Auto Build"
    )
    parser.add_argument(
        "--dest",
        required=True,
        help="Destination directory",
    )
    parser.add_argument(
        "--base",
        required=True,
        help="Path to Base NSP/XCI",
    )
    parser.add_argument(
        "--update",
        required=True,
        help="Path to Update NSP",
    )
    parser.add_argument(
        "--keys",
        required=True,
        help="Path to prod.keys",
    )
    parser.add_argument(
        "--firmware",
        default="",
        help="Path to firmware folder (optional)",
    )

    args = parser.parse_args()

    abs_dest = os.path.abspath(args.dest)
    abs_base = os.path.abspath(args.base)
    abs_update = os.path.abspath(args.update)
    abs_keys = os.path.abspath(args.keys)
    abs_firmware = (
        os.path.abspath(args.firmware)
        if args.firmware.strip()
        else ""
    )

    return abs_dest, abs_base, abs_update, abs_keys, abs_firmware


def main() -> None:
    # 1. Receive paths from the C++ bootstrap.
    abs_dest, abs_base, abs_update, abs_keys, abs_firmware = parse_args()

    # 2. Create the initial installation state file.
    os.makedirs(abs_dest, exist_ok=True)

    install_file = os.path.join(
        abs_dest,
        "totk-install.json",
    )

    with open(
        install_file,
        "w",
        encoding="utf-8",
    ) as f:
        json.dump(
            {
                "inputs": {},
                "steps": {},
            },
            f,
            indent=2,
        )

    # 3. Initialize the installer GUI.
    layout = layouts.find(Path(SETUP_DIR))

    fix_tcl_paths()

    root = tk.Tk()

    ui_lang = windows_ui_language()

    app = App(
        root,
        ui_lang,
        layout,
    )

    # 4. Inject paths supplied by main.cpp.
    app.vars["dest"].set(abs_dest)
    app.vars["base"].set(abs_base)
    app.vars["update"].set(abs_update)
    app.vars["keys"].set(abs_keys)
    app.vars["firmware"].set(abs_firmware)

    app.dlss_var.set(True)

    # 5. Start the recompilation.
    root.after(100, app.start)

    # 6. Run installer GUI.
    root.mainloop()


if __name__ == "__main__":
    main()