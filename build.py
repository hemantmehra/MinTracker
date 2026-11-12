"""Build MinTracker with the Microsoft C++ compiler and Windows SDK."""

import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
OUTPUT = ROOT / "build"


def find_developer_command():
    roots = [
        Path(value) / "Microsoft Visual Studio"
        for value in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"))
        if value
    ]
    for root in roots:
        vswhere = root / "Installer" / "vswhere.exe"
        if not vswhere.is_file():
            continue
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            check=True, capture_output=True, text=True,
        )
        location = result.stdout.strip()
        if location:
            command = Path(location) / "Common7" / "Tools" / "VsDevCmd.bat"
            if command.is_file():
                return command

    # Some Visual Studio installations do not include vswhere.
    candidates = []
    for root in roots:
        if root.is_dir():
            candidates.extend(root.glob(r"*\*\Common7\Tools\VsDevCmd.bat"))
    for command in sorted(candidates, reverse=True):
        installation = command.parent.parent.parent
        if (installation / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat").is_file():
            return command
    raise RuntimeError(
        "Visual Studio C++ tools were not found. Install the Desktop development "
        "with C++ workload and a Windows SDK, or run from a Developer Command Prompt."
    )


def compiler_environment():
    if shutil.which("cl.exe") and os.environ.get("INCLUDE") and os.environ.get("LIB"):
        return os.environ.copy()

    command = find_developer_command()
    environment = os.environ.copy()
    environment["MINTRACKER_VSDEVCMD"] = str(command)
    cmd = os.environ.get("COMSPEC", "cmd.exe")
    result = subprocess.run(
        f'"{cmd}" /d /s /c "call "%MINTRACKER_VSDEVCMD%" '
        '-arch=x64 -host_arch=x64 >nul && set"',
        env=environment, capture_output=True, text=True, errors="replace",
    )
    if result.returncode:
        raise RuntimeError(
            f"Visual Studio environment setup failed (exit {result.returncode}):\n"
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    for line in result.stdout.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            environment[key] = value
    if not shutil.which("cl.exe", path=environment.get("PATH")):
        raise RuntimeError("Visual Studio environment did not provide cl.exe.")
    return environment


def main():
    if sys.platform != "win32":
        raise RuntimeError("MinTracker requires Windows.")
    if len(sys.argv) != 1:
        raise RuntimeError("Usage: python build.py")
    environment = compiler_environment()
    compiler = shutil.which("cl.exe", path=environment.get("PATH"))
    if not compiler:
        raise RuntimeError("Could not locate cl.exe in the compiler environment.")
    OUTPUT.mkdir(exist_ok=True)
    executable = OUTPUT / "MinTracker.exe"
    command = [
        compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/permissive-",
        "/utf-8", "/O2", "/MD",
        "/DUNICODE", "/D_UNICODE", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
        str(ROOT / "src" / "main.cpp"), str(ROOT / "src" / "database.cpp"),
        "/Fo" + str(OUTPUT) + os.sep,
        "/Fe" + str(executable), "/link", "/SUBSYSTEM:WINDOWS",
        "user32.lib", "gdi32.lib", "comctl32.lib", "shell32.lib", "ole32.lib", "winsqlite3.lib",
    ]
    subprocess.run(command, cwd=ROOT, env=environment, check=True)
    print(f"Built {executable}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        sys.exit(1)
