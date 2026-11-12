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
            "SELECT SUM(d.duration_ms), SUM(CASE WHEN d.day=? THEN d.duration_ms ELSE 0 END) FROM apps a "
            "JOIN daily_usage d ON d.app_id=a.id "
            "WHERE a.path=? COLLATE NOCASE GROUP BY a.id",
            (datetime.date.today().isoformat(), executable),
        ).fetchone()
    return row


def records(path, executable):
    with closing(sqlite3.connect(path)) as db:
        return db.execute(
            "SELECT d.run_id,d.day,d.duration_ms,d.add_timestamp,a.tag FROM daily_usage d "
            "JOIN apps a ON a.id=d.app_id WHERE a.path=? COLLATE NOCASE ORDER BY d.id",
            (executable,),
        ).fetchall()


def main():
    exe = Path(sys.argv[1] if len(sys.argv) > 1 else r"build\MinTracker.exe").resolve()
    if user32.FindWindowW("MinTrackerWindow", None):
        raise RuntimeError("Exit the existing MinTracker instance before running this test.")
    if subprocess.run([exe, "--self-test"], timeout=15).returncode:
        raise AssertionError("Database self-test failed")

    processes = []
    with tempfile.TemporaryDirectory(prefix="mintracker-test-") as directory:
        database = Path(directory) / "usage.db"
        with closing(sqlite3.connect(database)) as db:
            db.executescript(
                "CREATE TABLE apps (id INTEGER PRIMARY KEY, path TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                "name TEXT NOT NULL, total_ms INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE daily_usage (app_id INTEGER NOT NULL REFERENCES apps(id),"
                "day TEXT NOT NULL, duration_ms INTEGER NOT NULL, PRIMARY KEY(app_id,day));"
                "INSERT INTO apps(path,name,total_ms) VALUES('legacy-test','legacy',1234);"
                "INSERT INTO daily_usage VALUES(1,'2020-01-01',1234);"
            )
            db.commit()
        try:
            tracker = subprocess.Popen([exe, "--database", database])
            processes.append(tracker)
            window = wait_for(lambda: user32.FindWindowW("MinTrackerWindow", None), "tracker window")
            assert user32.IsWindowVisible(window)
            with closing(sqlite3.connect(database)) as db:
                assert db.execute("PRAGMA user_version").fetchone()[0] == 1
                assert db.execute("SELECT COUNT(*) FROM apps WHERE path='legacy-test'").fetchone()[0] == 0
                assert "total_ms" not in {row[1] for row in db.execute("PRAGMA table_info(apps)")}
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
            first_records = records(database, executable)
            assert len(first_records) == 1 and first_records[0][4] == ""
            datetime.datetime.strptime(first_records[0][3], "%Y-%m-%dT%H:%M:%S.%fZ")
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
            run_records = records(database, executable)
            assert len(run_records) == 1 and run_records[0][2] == stopped[0]
            assert run_records[0][3] == first_records[0][3], "Insertion timestamp changed"
            with closing(sqlite3.connect(database)) as db:
                db.execute("UPDATE apps SET tag='focus' WHERE path=? COLLATE NOCASE", (executable,))
                db.commit()

            next_title = f"MinTracker later test app {os.getpid()}"
            next_child = subprocess.Popen([sys.executable, __file__, "--window", next_title])
            processes.append(next_child)
            next_window = wait_for(lambda: user32.FindWindowW(None, next_title), "later test app")
            wait_for(lambda: len(records(database, executable)) == 2, "second run record")
            post(next_window, WM_CLOSE)
            assert next_child.wait(timeout=10) == 0
            time.sleep(5)
            new_records = records(database, executable)
            assert len(new_records) == 2 and new_records[0][0] != new_records[1][0]
            assert all(row[2] > 0 and row[4] == "focus" for row in new_records)
            stopped = usage(database, executable)
            assert stopped[0] == sum(row[2] for row in new_records)

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
