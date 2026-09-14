#!/usr/bin/env python3
"""Install a standard Rainmeter .rmskin into XDG data and launch its entry skin."""

import argparse
import configparser
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from extract_rmskin import extract_package


def xdg_data_home() -> Path:
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))


def read_config(path: Path) -> configparser.RawConfigParser:
    config = configparser.RawConfigParser(interpolation=None, strict=False)
    for encoding in ("utf-8-sig", "cp1252"):
        try:
            with path.open(encoding=encoding) as source:
                config.read_file(source)
            return config
        except UnicodeDecodeError:
            continue
    raise RuntimeError(f"Unable to read {path}")


def case_path(root: Path, portable: str) -> Path:
    current = root
    for part in Path(portable.replace("\\", "/")).parts:
        if part in (".", ""):
            continue
        matches = [entry for entry in current.iterdir() if entry.name.casefold() == part.casefold()]
        if len(matches) != 1:
            raise RuntimeError(f"Package launch skin not found: {portable}")
        current = matches[0]
    return current


def runtime_path(explicit: str | None) -> str:
    if explicit:
        return explicit
    development = Path(__file__).resolve().parents[1] / "build" / "rainmeter"
    if development.is_file():
        return str(development)
    installed = shutil.which("rainmeter")
    if installed:
        return installed
    raise RuntimeError("rainmeter controller was not found; pass --runtime PATH")


def install(package: Path) -> dict[str, object]:
    target = xdg_data_home() / "rainmeter-linux"
    skins = target / "Skins"
    fonts = target / "Fonts"
    with tempfile.TemporaryDirectory(prefix="rainmeter-rmskin-") as temporary:
        stage = Path(temporary)
        metadata = extract_package(package, stage)
        legacy_configs = list(stage.rglob("Rainstaller.cfg"))
        modern_configs = list(stage.rglob("RMSKIN.ini"))
        if len(legacy_configs) == 1:
            config_file = legacy_configs[0]
            package_config = read_config(config_file)
            if not package_config.has_section("Rainstaller"):
                raise RuntimeError("Rainstaller.cfg has no [Rainstaller] section")
            source_skins = config_file.parent / "Skins"
            source_fonts = config_file.parent / "Fonts"
            section = "Rainstaller"
            name_key = "Name"
            launch_type = package_config.get(section, "LaunchType", fallback="").casefold()
            launch_command = package_config.get(section, "LaunchCommand", fallback="")
            package_format = "Rainstaller"
        elif len(modern_configs) == 1:
            config_file = modern_configs[0]
            package_config = read_config(config_file)
            if not package_config.has_section("rmskin"):
                raise RuntimeError("RMSKIN.ini has no [rmskin] section")
            source_skins = config_file.parent / "Skins"
            source_fonts = config_file.parent / "Fonts"
            section = "rmskin"
            name_key = "Name"
            launch_type = "load"
            launch_command = package_config.get(section, "Load", fallback="")
            package_format = "RMSKIN"
        else:
            raise RuntimeError("Package has neither one Rainstaller.cfg nor one RMSKIN.ini")
        if not source_skins.is_dir():
            raise RuntimeError("Package has no Skins directory")
        skins.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source_skins, skins, dirs_exist_ok=True)
        if source_fonts.is_dir():
            fonts.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source_fonts, fonts, dirs_exist_ok=True)
        entry = case_path(skins, launch_command) if launch_type == "load" and launch_command else None
        entries = sorted(str(path.relative_to(source_skins)) for path in source_skins.rglob("*.ini"))
        return {
            **metadata,
            "name": package_config.get(section, name_key, fallback=package.stem),
            "skins": str(skins),
            "entry": str(entry) if entry else None,
            "entries": entries,
            "launchType": launch_type or None,
            "format": package_format,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="Rainmeter .rmskin package")
    parser.add_argument("--runtime", help="rainmeter-linux executable")
    parser.add_argument("--monitor", help="X11 output name, such as HDMI-0")
    parser.add_argument("--position", default="0,0", help="position relative to --monitor")
    parser.add_argument("--auto-exit-ms", type=int, help="close the launched skin after this many milliseconds")
    parser.add_argument("--autostart", action="store_true", help="create an XDG autostart entry for this package")
    parser.add_argument("--all", action="store_true", help="launch every config when the package has no Load entry")
    parser.add_argument("--no-launch", action="store_true", help="install only")
    args = parser.parse_args()
    try:
        result = install(args.package)
        entry = result["entry"]
        executable = runtime_path(args.runtime)
        command = [executable, "--position", args.position]
        if args.monitor:
            command += ["--monitor", args.monitor]
        if args.auto_exit_ms is not None:
            if args.auto_exit_ms < 0:
                raise RuntimeError("--auto-exit-ms must be non-negative")
            command += ["--auto-exit-ms", str(args.auto_exit_ms)]
        bundled = [str(Path(result["skins"]) / path) for path in result["entries"]]
        if entry:
            targets = [entry]
        elif args.all:
            targets = bundled
        else:
            targets = []
            result["notice"] = "No Load entry: installed without guessing an active skin; choose modules in Manage Rainmeter or use --all."
        if not bundled:
            raise RuntimeError("Package contains no skin .ini files")
        result["launch"] = targets
        if args.autostart:
            autostart = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "autostart"
            autostart.mkdir(parents=True, exist_ok=True)
            desktop = autostart / f"rainmeter-linux-{args.package.stem}.desktop"
            if len(targets) != 1:
                raise RuntimeError("--autostart requires a package with one declared Load entry")
            quoted = " ".join(shlex.quote(part) for part in command + [targets[0]])
            desktop.write_text("[Desktop Entry]\nType=Application\nName=" + str(result["name"]) +
                               "\nExec=" + quoted + "\nX-GNOME-Autostart-enabled=true\n", encoding="utf-8")
            result["autostart"] = str(desktop)
        print(json.dumps(result, indent=2))
        if not args.no_launch:
            if not targets:
                os.execv(executable, [executable])
            if len(targets) == 1:
                os.execv(executable, command + targets)
            for target in targets:
                subprocess.Popen(command + [target], start_new_session=True)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"rainmeter-rmskin: {error}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
