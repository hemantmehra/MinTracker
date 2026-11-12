"""Windows desktop integration test; run with Python and a built MinTracker.exe."""

import ctypes
from ctypes import wintypes
from contextlib import closing
import datetime
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time


if len(sys.argv) > 1 and sys.argv[1] == "--window":
    import tkinter

    root = tkinter.Tk()
    root.title(sys.argv[2])
    root.mainloop()
    raise SystemExit(0)

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowW.restype = wintypes.HWND
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.PostMessageW.restype = wintypes.BOOL
user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowExW.restype = wintypes.HWND
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t

WM_CLOSE = 0x0010
WM_ENDSESSION = 0x0016
LVM_GETITEMCOUNT = 0x1004


def wait_for(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {description}")


def post(window, message, wparam=0):
    if not user32.PostMessageW(window, message, wparam, 0):
        raise ctypes.WinError(ctypes.get_last_error())


def usage(path, executable):
    if not path.exists():
        return None
    with closing(sqlite3.connect(path)) as db:
        row = db.execute(
            "SELECT a.total_ms, d.duration_ms FROM apps a "
            "JOIN daily_usage d ON d.app_id=a.id "
            "WHERE a.path=? COLLATE NOCASE AND d.day=?",
            (executable, datetime.date.today().isoformat()),
        ).fetchone()
    return row


def main():
    exe = Path(sys.argv[1] if len(sys.argv) > 1 else r"build\MinTracker.exe").resolve()
    if user32.FindWindowW("MinTrackerWindow", None):
        raise RuntimeError("Exit the existing MinTracker instance before running this test.")
    if subprocess.run([exe, "--self-test"], timeout=15).returncode:
        raise AssertionError("Database self-test failed")

    processes = []
    with tempfile.TemporaryDirectory(prefix="mintracker-test-") as directory:
        database = Path(directory) / "usage.db"
        try:
            tracker = subprocess.Popen([exe, "--database", database])
            processes.append(tracker)
            window = wait_for(lambda: user32.FindWindowW("MinTrackerWindow", None), "tracker window")
            assert user32.IsWindowVisible(window)
            list_view = wait_for(
                lambda: user32.FindWindowExW(window, None, "SysListView32", None),
                "application list",
            )
            title = f"MinTracker test app {os.getpid()}"
            child = subprocess.Popen([sys.executable, __file__, "--window", title])
            processes.append(child)
            child_window = wait_for(lambda: user32.FindWindowW(None, title), "test app")
            executable = str(Path(sys.executable).resolve())
            first = wait_for(lambda: usage(database, executable), "usage database record")
            assert first[0] == first[1] and first[0] > 0
            wait_for(lambda: user32.SendMessageW(list_view, LVM_GETITEMCOUNT, 0, 0) > 0, "list rows")

            other_title = f"MinTracker second test app {os.getpid()}"
            other = subprocess.Popen([sys.executable, __file__, "--window", other_title])
            processes.append(other)
            other_window = wait_for(lambda: user32.FindWindowW(None, other_title), "second test app")
            time.sleep(6)
            overlap = usage(database, executable)
            assert 3000 < overlap[0] - first[0] < 9000, "Overlapping processes counted more than once"

            post(window, WM_CLOSE)
            wait_for(lambda: not user32.IsWindowVisible(window), "window to hide")
            assert tracker.poll() is None, "Closing the window stopped the tracker"
            post(child_window, WM_CLOSE)
            assert child.wait(timeout=10) == 0
            second = wait_for(
                lambda: (row if (row := usage(database, executable)) and row[0] > overlap[0] else None),
                "tracking remaining process while hidden",
            )
            post(other_window, WM_CLOSE)
            assert other.wait(timeout=10) == 0
            final = wait_for(
                lambda: (row if (row := usage(database, executable)) and row[0] >= second[0] else None),
                "exit accounting",
            )
            time.sleep(5)
            stopped = usage(database, executable)
            assert stopped[0] >= final[0]
            time.sleep(3)
            assert usage(database, executable) == stopped, "Runtime increased after app exit"

            second_instance = subprocess.Popen([exe, "--database", database])
            processes.append(second_instance)
            assert second_instance.wait(timeout=10) == 0
            wait_for(lambda: user32.IsWindowVisible(window), "existing window to reopen")
            post(window, WM_ENDSESSION, 1)
            assert tracker.wait(timeout=10) == 0
            restarted = subprocess.Popen([exe, "--database", database])
            processes.append(restarted)
            reopened = wait_for(lambda: user32.FindWindowW("MinTrackerWindow", None), "restarted window")
            assert usage(database, executable) == stopped, "Runtime did not persist across restart"
            post(reopened, WM_ENDSESSION, 1)
            assert restarted.wait(timeout=10) == 0
            print(f"PASS: tracked {stopped[0]} ms; overlap, hide, restore, exit, and persistence verified")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == "__main__":
    main()
