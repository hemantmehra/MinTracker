# MinTracker

MinTracker is a native Windows desktop app that records how long desktop apps
run while MinTracker is running. It needs Windows 10 or later and uses the
SQLite library included with Windows.

## Build

Install Python 3 and Visual Studio with the **Desktop development with C++**
workload and a Windows SDK. From the project directory, run:

```bat
python build.py
```

The script locates Visual Studio and builds an x64 release executable. If run
from a Visual Studio Developer Command Prompt, it uses that prompt's configured
compiler and target architecture instead. No CMake or Python packages are needed.

Run `build\MinTracker.exe`. The database is stored at
`%LOCALAPPDATA%\MinTracker\usage.db`. No network access is used.
To use a different database, run `MinTracker.exe --database <path>`; its parent
directory must already exist. Only one MinTracker instance runs per Windows
user session.

When first opening a database from the previous daily-aggregate schema, this
version resets its usage history to empty. New databases and databases already
using the new schema are not reset.

## Use

The window shows each executable's accumulated runtime and runtime for the
current local calendar day in hours:minutes:seconds. Apps are identified by their executable path; apps
with the same executable name but different paths are listed separately.
Multiple windows or processes of the same executable count only once while
they overlap. The display and database update approximately every two seconds.
The upper panel shows currently tracked apps; the lower panel shows apps that
have been tracked previously but are no longer running. Select an app in either
panel, edit its tag in the field below the lists, and select **Save tag**.
Clear the field and save to remove a tag. Tags are associated with executable
paths and persist across runs. The path column distinguishes apps with the
same name.
The `apps.tag` column defaults to an empty string. `daily_usage` stores one
record per tracked run per local day: a run spanning midnight has a separate
record for each day, with the same `run_id`. Each record's `duration_ms` grows
until that run ends (or the day ends), and `add_timestamp` is the UTC time when
the record was first inserted, in ISO 8601 format. Total runtime is the sum of
the app's records; today's runtime sums records for the current local date.

Closing the window hides it in the Windows notification area. Depending on
Windows settings, its icon may be inside the hidden icons menu. Double-click
the icon, or right-click it and select **Open**, to reopen the window.
Right-click and select **Exit** to stop tracking and quit. Windows controls
whether the icon is shown directly on the taskbar or in the hidden icons menu.

MinTracker discovers visible, unowned desktop windows by polling. Once an app
has been discovered, it remains tracked until its process exits, including
while minimized or hidden. Background services and apps without a qualifying
window are not tracked; an app that opens and exits between scans may be
missed. Tracking begins when MinTracker first discovers the app, not when the
app started if it was already running. Apps which cannot be inspected due to
Windows permissions are excluded. Time is tracked only while MinTracker is
running; it does not automatically start with Windows. A sudden termination
may lose up to one update interval.

## Self-test

Run `MinTracker.exe --self-test` from a command prompt. The exit code is zero
on success. The test uses an in-memory database and does not change usage data.
For an interactive desktop integration test, exit any running MinTracker
instance, install Python with Tkinter, and run
`python tests\integration.py build\MinTracker.exe`. It uses a temporary database.
