#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winsqlite\winsqlite3.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTimer = 1;
constexpr UINT kOpen = 1001;
constexpr UINT kExit = 1002;
constexpr UINT_PTR kTrayId = 1;
constexpr uint64_t kTicksPerMs = 10000;
constexpr uint64_t kTicksPerDay = 864000000000ULL;
const wchar_t kClassName[] = L"MinTrackerWindow";

uint64_t FileTimeValue(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

FILETIME ToFileTime(uint64_t value) {
    ULARGE_INTEGER integer{};
    integer.QuadPart = value;
    return FILETIME{integer.LowPart, integer.HighPart};
}

uint64_t Now() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return FileTimeValue(time);
}

std::wstring ErrorMessage(const std::string& message) {
    int size = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"An unexpected error occurred.";
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, &result[0], size);
    result.pop_back();
    return result;
}

std::runtime_error WinError(const char* operation) {
    return std::runtime_error(std::string(operation) + " (Windows error " +
                              std::to_string(GetLastError()) + ")");
}

struct Statement {
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void Text(int index, const std::wstring& text) {
        if (sqlite3_bind_text16(value, index, text.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("Could not bind SQLite text.");
    }
    void Integer(int index, int64_t number) {
        if (sqlite3_bind_int64(value, index, number) != SQLITE_OK)
            throw std::runtime_error("Could not bind SQLite integer.");
    }
    void Done(sqlite3* db) {
        if (sqlite3_step(value) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    }
};

struct Row {
    std::wstring name;
    int64_t total;
    int64_t today;
};

class Database {
    sqlite3* db_ = nullptr;
public:
    explicit Database(const std::wstring& path) {
        if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
            std::string error = db_ ? sqlite3_errmsg(db_) : "Could not open database.";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(error);
        }
        try {
            Exec("PRAGMA foreign_keys=ON");
            Exec("CREATE TABLE IF NOT EXISTS apps ("
                 "id INTEGER PRIMARY KEY, path TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                 "name TEXT NOT NULL, total_ms INTEGER NOT NULL DEFAULT 0)");
            Exec("CREATE TABLE IF NOT EXISTS daily_usage ("
                 "app_id INTEGER NOT NULL REFERENCES apps(id), day TEXT NOT NULL,"
                 "duration_ms INTEGER NOT NULL, PRIMARY KEY(app_id, day))");
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }
    ~Database() { if (db_) sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    void Exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    void Add(const std::wstring& path, const std::wstring& name,
             const std::wstring& day, int64_t milliseconds) {
        if (milliseconds <= 0) return;
        Statement insertApp(db_, "INSERT OR IGNORE INTO apps(path,name) VALUES(?1,?2)");
        insertApp.Text(1, path);
        insertApp.Text(2, name);
        insertApp.Done(db_);
        Statement updateApp(db_, "UPDATE apps SET total_ms=total_ms+?3,name=?2 WHERE path=?1");
        updateApp.Text(1, path);
        updateApp.Text(2, name);
        updateApp.Integer(3, milliseconds);
        updateApp.Done(db_);
        Statement insertDaily(db_, "INSERT OR IGNORE INTO daily_usage(app_id,day,duration_ms) "
                                   "SELECT id,?2,0 FROM apps WHERE path=?1");
        insertDaily.Text(1, path);
        insertDaily.Text(2, day);
        insertDaily.Done(db_);
        Statement updateDaily(db_, "UPDATE daily_usage SET duration_ms=duration_ms+?3 "
                                   "WHERE app_id=(SELECT id FROM apps WHERE path=?1) AND day=?2");
        updateDaily.Text(1, path);
        updateDaily.Text(2, day);
        updateDaily.Integer(3, milliseconds);
        updateDaily.Done(db_);
    }
    std::vector<Row> Rows(const std::wstring& day) {
        Statement query(db_, "SELECT a.name,a.total_ms,COALESCE(d.duration_ms,0) FROM apps a "
                             "LEFT JOIN daily_usage d ON d.app_id=a.id AND d.day=?1 "
                             "ORDER BY a.name COLLATE NOCASE,a.path COLLATE NOCASE");
        query.Text(1, day);
        std::vector<Row> rows;
        int result;
        while ((result = sqlite3_step(query.value)) == SQLITE_ROW) {
            const auto* name = static_cast<const wchar_t*>(sqlite3_column_text16(query.value, 0));
            if (!name) throw std::runtime_error("Could not read app name from SQLite.");
            rows.push_back({name, sqlite3_column_int64(query.value, 1),
                            sqlite3_column_int64(query.value, 2)});
        }
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return rows;
    }
};

std::wstring Date(const SYSTEMTIME& local) {
    wchar_t result[11]{};
    swprintf_s(result, L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    return result;
}

SYSTEMTIME LocalAt(uint64_t ticks) {
    FILETIME file = ToFileTime(ticks);
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&file, &utc) ||
        !SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local))
        throw WinError("Could not convert time to local time");
    return local;
}

// Find the next local midnight in UTC, including daylight-saving transitions.
uint64_t NextMidnight(const SYSTEMTIME& local) {
    SYSTEMTIME midnight = local;
    midnight.wHour = midnight.wMinute = midnight.wSecond = midnight.wMilliseconds = 0;
    FILETIME calendar{};
    if (!SystemTimeToFileTime(&midnight, &calendar)) throw WinError("Invalid local date");
    calendar = ToFileTime(FileTimeValue(calendar) + kTicksPerDay);
    if (!FileTimeToSystemTime(&calendar, &midnight)) throw WinError("Invalid next date");
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTimeEx(nullptr, &midnight, &utc))
        throw WinError("Could not convert local midnight");
    FILETIME result{};
    if (!SystemTimeToFileTime(&utc, &result)) throw WinError("Invalid UTC date");
    return FileTimeValue(result);
}

void AddInterval(Database& db, const std::wstring& path, const std::wstring& name,
                 uint64_t begin, uint64_t end) {
    while (begin < end) {
        SYSTEMTIME local = LocalAt(begin);
        uint64_t boundary = NextMidnight(local);
        if (boundary <= begin) throw std::runtime_error("Invalid local midnight boundary.");
        uint64_t next = (std::min)(boundary, end);
        db.Add(path, name, Date(local), static_cast<int64_t>((next - begin) / kTicksPerMs));
        begin = next;
    }
}

std::wstring DatabasePath() {
    PWSTR folder = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder);
    if (FAILED(hr)) throw std::runtime_error("Could not locate LocalAppData.");
    std::wstring directory(folder);
    CoTaskMemFree(folder);
    directory += L"\\MinTracker";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw WinError("Could not create database directory");
    return directory + L"\\usage.db";
}

struct Process {
    DWORD pid;
    HANDLE handle;
};
struct App {
    std::wstring name;
    uint64_t last;
    std::vector<Process> processes;
};

class Tracker {
    Database& db_;
    std::map<std::wstring, App> apps_;
    static BOOL CALLBACK EnumWindow(HWND window, LPARAM parameter) {
        if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
            (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        auto* pids = reinterpret_cast<std::vector<DWORD>*>(parameter);
        if (pid && pid != GetCurrentProcessId()) pids->push_back(pid);
        return TRUE;
    }
public:
    explicit Tracker(Database& db) : db_(db) {}
    ~Tracker() {
        for (auto& pair : apps_)
            for (auto& process : pair.second.processes) CloseHandle(process.handle);
    }
    void Scan(uint64_t now) {
        std::vector<DWORD> pids;
        if (!EnumWindows(EnumWindow, reinterpret_cast<LPARAM>(&pids)))
            throw WinError("Could not enumerate desktop windows");
        std::sort(pids.begin(), pids.end());
        pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
        for (DWORD pid : pids) {
            bool known = false;
            for (const auto& pair : apps_)
                for (const auto& process : pair.second.processes)
                    if (process.pid == pid) known = true;
            if (known) continue;
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (!handle) continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (!QueryFullProcessImageNameW(handle, 0, path, &length) ||
                WaitForSingleObject(handle, 0) != WAIT_TIMEOUT) {
                CloseHandle(handle);
                continue;
            }
            std::wstring key(path, length);
            // Windows executable paths are case-insensitive.
            if (!CharLowerBuffW(&key[0], static_cast<DWORD>(key.size()))) {
                CloseHandle(handle);
                throw WinError("Could not normalize executable path");
            }
            size_t slash = key.find_last_of(L"\\/");
            std::wstring name(path + (slash == std::wstring::npos ? 0 : slash + 1));
            auto result = apps_.try_emplace(key, App{name, now, {}});
            result.first->second.processes.push_back({pid, handle});
        }
    }
    void Flush(uint64_t now) {
        struct Update { std::wstring path; uint64_t end; std::vector<DWORD> exited; };
        std::vector<Update> updates;
        for (const auto& pair : apps_) {
            uint64_t end = 0;
            std::vector<DWORD> exited;
            for (const auto& process : pair.second.processes) {
                DWORD status = WaitForSingleObject(process.handle, 0);
                if (status == WAIT_TIMEOUT) {
                    end = now;
                    continue;
                }
                if (status != WAIT_OBJECT_0) throw WinError("Could not inspect process");
                FILETIME creation{}, exit{}, kernel{}, user{};
                if (!GetProcessTimes(process.handle, &creation, &exit, &kernel, &user))
                    throw WinError("Could not read process exit time");
                end = (std::max)(end, (std::min)(FileTimeValue(exit), now));
                exited.push_back(process.pid);
            }
            updates.push_back({pair.first, end, std::move(exited)});
        }
        db_.Exec("BEGIN IMMEDIATE");
        try {
            for (const auto& update : updates) {
                const App& app = apps_.at(update.path);
                if (update.end > app.last)
                    AddInterval(db_, update.path, app.name, app.last, update.end);
            }
            db_.Exec("COMMIT");
        } catch (...) {
            try { db_.Exec("ROLLBACK"); } catch (const std::exception&) {
                throw std::runtime_error("Could not roll back a failed database update.");
            }
            throw;
        }
        for (const auto& update : updates) {
            App& app = apps_.at(update.path);
            app.last = (std::max)(app.last, update.end);
            auto it = app.processes.begin();
            while (it != app.processes.end()) {
                if (std::find(update.exited.begin(), update.exited.end(), it->pid) != update.exited.end()) {
                    CloseHandle(it->handle);
                    it = app.processes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto it = apps_.begin(); it != apps_.end();) {
            if (it->second.processes.empty()) it = apps_.erase(it);
            else ++it;
        }
    }
};

std::wstring FormatDuration(int64_t ms) {
    int64_t seconds = ms / 1000;
    wchar_t result[64]{};
    swprintf_s(result, L"%lld:%02lld:%02lld", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    return result;
}

class Application {
    Database db_;
    Tracker tracker_;
    HWND window_ = nullptr;
    HWND list_ = nullptr;
    bool tray_ = false;
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    NOTIFYICONDATAW icon_{};

    void AddTray() {
        icon_.cbSize = sizeof(icon_);
        icon_.hWnd = window_;
        icon_.uID = static_cast<UINT>(kTrayId);
        icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon_.uCallbackMessage = kTrayMessage;
        icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon_.szTip, L"MinTracker");
        if (!Shell_NotifyIconW(NIM_ADD, &icon_))
            throw std::runtime_error("Could not add the MinTracker notification icon.");
        tray_ = true;
    }
    void Refresh() {
        auto rows = db_.Rows(Date(LocalAt(Now())));
        ListView_DeleteAllItems(list_);
        for (size_t i = 0; i < rows.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(rows[i].name.c_str());
            int index = ListView_InsertItem(list_, &item);
            if (index < 0) throw std::runtime_error("Could not update the application list.");
            std::wstring total = FormatDuration(rows[i].total);
            std::wstring today = FormatDuration(rows[i].today);
            ListView_SetItemText(list_, index, 1, &total[0]);
            ListView_SetItemText(list_, index, 2, &today[0]);
        }
    }
    void Tick() {
        uint64_t now = Now();
        tracker_.Flush(now);
        tracker_.Scan(now);
        Refresh();
    }
    void Show() {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
    }
    void TrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) throw WinError("Could not create tray menu");
        AppendMenuW(menu, MF_STRING, kOpen, L"Open");
        AppendMenuW(menu, MF_STRING, kExit, L"Exit");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command == kOpen) Show();
        if (command == kExit) DestroyWindow(window_);
    }
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbarCreated_) { tray_ = false; AddTray(); return 0; }
        switch (message) {
        case WM_CREATE: {
            list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                0, 0, 0, 0, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!list_) throw WinError("Could not create application list");
            ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* names[] = {L"Application", L"Total runtime", L"Today's runtime"};
            const int widths[] = {270, 150, 150};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                column.pszText = const_cast<wchar_t*>(names[i]);
                column.cx = widths[i];
                column.iSubItem = i;
                if (ListView_InsertColumn(list_, i, &column) < 0)
                    throw std::runtime_error("Could not create list column.");
            }
            AddTray();
            Tick();
            if (!SetTimer(window_, kTimer, 2000, nullptr)) throw WinError("Could not start tracking timer");
            return 0;
        }
        case WM_SIZE:
            if (list_) MoveWindow(list_, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
            return 0;
        case WM_TIMER:
            if (wparam == kTimer) Tick();
            return 0;
        case kTrayMessage:
            if (lparam == WM_LBUTTONDBLCLK) Show();
            else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) TrayMenu();
            return 0;
        case WM_CLOSE:
            ShowWindow(window_, SW_HIDE);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTimer);
            tracker_.Flush(Now());
            if (tray_) { Shell_NotifyIconW(NIM_DELETE, &icon_); tray_ = false; }
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(window_, message, wparam, lparam);
        }
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<Application*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try {
            return app->Handle(message, wparam, lparam);
        } catch (const std::exception& error) {
            MessageBoxW(window, ErrorMessage(error.what()).c_str(), L"MinTracker error", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) return -1;
            if (message == WM_DESTROY) { PostQuitMessage(1); return 0; }
            KillTimer(window, kTimer);
            if (app->tray_) { Shell_NotifyIconW(NIM_DELETE, &app->icon_); app->tray_ = false; }
            PostQuitMessage(1);
            return 0;
        }
    }
public:
    explicit Application(const std::wstring& databasePath) : db_(databasePath), tracker_(db_) {}
    ~Application() { if (tray_) Shell_NotifyIconW(NIM_DELETE, &icon_); }
    int Run(HINSTANCE instance, int show) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
        if (!InitCommonControlsEx(&controls)) throw WinError("Could not initialize Windows controls");
        WNDCLASSW cls{};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = instance;
        cls.lpszClassName = kClassName;
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&cls)) throw WinError("Could not register window class");
        HWND window = CreateWindowExW(0, kClassName, L"MinTracker",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 620, 480,
            nullptr, nullptr, instance, this);
        if (!window) return 1;
        ShowWindow(window, show);
        UpdateWindow(window);
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (result == -1) throw WinError("Windows message loop failed");
        return static_cast<int>(message.wParam);
    }
};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int SelfTest() {
    Database db(L":memory:");
    SYSTEMTIME local{};
    GetLocalTime(&local);
    local.wHour = 12; local.wMinute = 0; local.wSecond = 0; local.wMilliseconds = 0;
    uint64_t midnight = NextMidnight(local);
    std::wstring first = Date(LocalAt(midnight - 1000 * kTicksPerMs));
    std::wstring second = Date(LocalAt(midnight + 1000 * kTicksPerMs));
    db.Exec("BEGIN IMMEDIATE");
    AddInterval(db, L"c:\\test\\app.exe", L"app.exe",
                midnight - 2000 * kTicksPerMs, midnight + 3000 * kTicksPerMs);
    db.Exec("COMMIT");
    auto before = db.Rows(first);
    auto after = db.Rows(second);
    Require(first != second && before.size() == 1 && after.size() == 1, "Date split failed.");
    Require(before[0].total == 5000 && before[0].today == 2000 &&
            after[0].total == 5000 && after[0].today == 3000, "Incorrect usage totals.");
    db.Exec("BEGIN IMMEDIATE");
    db.Add(L"c:\\test\\app.exe", L"app.exe", second, 1000);
    db.Exec("ROLLBACK");
    Require(db.Rows(second)[0].total == 5000, "Transaction rollback failed.");
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw WinError("Could not read command line");
        bool selfTest = argc == 2 && std::wstring(argv[1]) == L"--self-test";
        std::wstring databasePath;
        if (argc == 3 && std::wstring(argv[1]) == L"--database") databasePath = argv[2];
        else if (argc != 1 && !selfTest) {
            LocalFree(argv);
            throw std::runtime_error("Usage: MinTracker.exe [--database <path> | --self-test]");
        }
        LocalFree(argv);
        if (selfTest) return SelfTest();
        if (databasePath.empty()) databasePath = DatabasePath();
        HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MinTracker-SingleInstance");
        if (!mutex) throw WinError("Could not create application mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(kClassName, nullptr);
            if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
            CloseHandle(mutex);
            return 0;
        }
        int result;
        try { Application app(databasePath); result = app.Run(instance, show); }
        catch (...) { CloseHandle(mutex); throw; }
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return result;
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, ErrorMessage(error.what()).c_str(), L"MinTracker error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
