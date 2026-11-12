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
user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetDlgItem.restype = wintypes.HWND
user32.IsWindowEnabled.argtypes = [wintypes.HWND]
user32.IsWindowEnabled.restype = wintypes.BOOL
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
kernel32.VirtualFreeEx.restype = wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

WM_CLOSE = 0x0010
WM_ENDSESSION = 0x0016
LVM_GETITEMCOUNT = 0x1004
LVM_GETITEMTEXTW = 0x1073
LVM_SETITEMSTATE = 0x102B
WM_COMMAND = 0x0111
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
LVIS_SELECTED = 0x0002


class LVITEM(ctypes.Structure):
    _fields_ = [
        ("mask", wintypes.UINT), ("iItem", ctypes.c_int), ("iSubItem", ctypes.c_int),
        ("state", wintypes.UINT), ("stateMask", wintypes.UINT), ("pszText", ctypes.c_void_p),
        ("cchTextMax", ctypes.c_int), ("iImage", ctypes.c_int), ("lParam", ctypes.c_ssize_t),
        ("iIndent", ctypes.c_int), ("iGroupId", ctypes.c_int), ("cColumns", wintypes.UINT),
        ("puColumns", ctypes.c_void_p), ("piColFmt", ctypes.c_void_p), ("iGroup", ctypes.c_int),
    ]


class ListReader:
    def __init__(self, pid):
        self.process = kernel32.OpenProcess(0x438, False, pid)
        if not self.process:
            raise ctypes.WinError(ctypes.get_last_error())
        self.remote = kernel32.VirtualAllocEx(self.process, None, 8192, 0x3000, 0x04)
        if not self.remote:
            kernel32.CloseHandle(self.process)
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        kernel32.VirtualFreeEx(self.process, self.remote, 0, 0x8000)
        kernel32.CloseHandle(self.process)

    def write(self, item):
        if not kernel32.WriteProcessMemory(self.process, self.remote, ctypes.byref(item), ctypes.sizeof(item), None):
            raise ctypes.WinError(ctypes.get_last_error())

    def text(self, list_view, index, column):
        item = LVITEM()
        item.iSubItem = column
        item.pszText = self.remote + ctypes.sizeof(item)
        item.cchTextMax = 2048
        self.write(item)
        length = user32.SendMessageW(list_view, LVM_GETITEMTEXTW, index, self.remote)
        result = ctypes.create_unicode_buffer(2048)
        if not kernel32.ReadProcessMemory(self.process, item.pszText, result, (length + 1) * 2, None):
            raise ctypes.WinError(ctypes.get_last_error())
        return result.value

    def index(self, list_view, path):
        count = user32.SendMessageW(list_view, LVM_GETITEMCOUNT, 0, 0)
        for index in range(count):
            if self.text(list_view, index, 4).casefold() == path.casefold():
                return index
        return -1

    def select(self, list_view, index):
        item = LVITEM()
        item.state = LVIS_SELECTED
        item.stateMask = LVIS_SELECTED
        self.write(item)
        if not user32.SendMessageW(list_view, LVM_SETITEMSTATE, index, self.remote):
            raise RuntimeError("Could not select list item")


def field_text(field):
    result = ctypes.create_unicode_buffer(2048)
    user32.SendMessageW(field, WM_GETTEXT, len(result), ctypes.addressof(result))
    return result.value


def set_field_text(field, text):
    value = ctypes.create_unicode_buffer(text)
    if not user32.SendMessageW(field, WM_SETTEXT, 0, ctypes.addressof(value)):
        raise RuntimeError("Could not set tag editor text")


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
    reader = None
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
            active_list = user32.GetDlgItem(window, 1101)
            inactive_list = user32.GetDlgItem(window, 1102)
            tag_edit = user32.GetDlgItem(window, 1103)
            save_tag = user32.GetDlgItem(window, 1104)
            assert active_list and inactive_list and tag_edit and save_tag
            reader = ListReader(tracker.pid)
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
            index = wait_for(
                lambda: (i + 1 if (i := reader.index(active_list, executable)) >= 0 else None),
                "test app in active panel",
            ) - 1
            assert reader.index(inactive_list, executable) == -1
            reader.select(active_list, index)
            assert user32.IsWindowEnabled(tag_edit) and user32.IsWindowEnabled(save_tag), "Selection did not enable tag editor"
            assert field_text(tag_edit) == ""
            set_field_text(tag_edit, "active \u03a9")
            time.sleep(3)
            assert field_text(tag_edit) == "active \u03a9", "Refresh overwrote unsaved edit"
            assert user32.IsWindowEnabled(save_tag), "Refresh disabled tag editor"
            user32.SendMessageW(window, WM_COMMAND, 1104, save_tag)
            wait_for(lambda: records(database, executable)[0][4] == "active \u03a9", "active tag save")
            index = reader.index(active_list, executable)
            assert index >= 0 and reader.text(active_list, index, 3) == "active \u03a9"
            first = usage(database, executable)

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
            index = wait_for(
                lambda: (i + 1 if (i := reader.index(inactive_list, executable)) >= 0 else None),
                "test app in inactive panel",
            ) - 1
            assert reader.index(active_list, executable) == -1
            reader.select(inactive_list, index)
            assert field_text(tag_edit) == "active \u03a9"
            set_field_text(tag_edit, "focus")
            user32.SendMessageW(window, WM_COMMAND, 1104, save_tag)
            wait_for(lambda: records(database, executable)[0][4] == "focus", "inactive tag save")

            next_title = f"MinTracker later test app {os.getpid()}"
            next_child = subprocess.Popen([sys.executable, __file__, "--window", next_title])
            processes.append(next_child)
            next_window = wait_for(lambda: user32.FindWindowW(None, next_title), "later test app")
            wait_for(lambda: len(records(database, executable)) == 2, "second run record")
            index = wait_for(
                lambda: (i + 1 if (i := reader.index(active_list, executable)) >= 0 else None),
                "restarted app in active panel",
            ) - 1
            assert reader.index(inactive_list, executable) == -1
            assert field_text(tag_edit) == "focus", "Selection was lost when app changed panels"
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
            assert records(database, executable)[0][4] == "focus", "Tag did not persist across restart"
            reader.close()
            reader = ListReader(restarted.pid)
            inactive_list = user32.GetDlgItem(reopened, 1102)
            tag_edit = user32.GetDlgItem(reopened, 1103)
            save_tag = user32.GetDlgItem(reopened, 1104)
            index = wait_for(
                lambda: (i + 1 if (i := reader.index(inactive_list, executable)) >= 0 else None),
                "previous run in inactive panel after restart",
            ) - 1
            reader.select(inactive_list, index)
            assert field_text(tag_edit) == "focus"
            set_field_text(tag_edit, "")
            user32.SendMessageW(reopened, WM_COMMAND, 1104, save_tag)
            assert records(database, executable)[0][4] == "", "Clearing tag did not persist"
            post(reopened, WM_ENDSESSION, 1)
            assert restarted.wait(timeout=10) == 0
            print(f"PASS: tracked {stopped[0]} ms; overlap, hide, restore, exit, and persistence verified")
        finally:
            if reader:
                reader.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == "__main__":
    main()
