#include "database.h"

#include <winsqlite\winsqlite3.h>

#include <stdexcept>
#include <set>

namespace {
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

std::set<std::string> Columns(sqlite3* db, const char* pragma) {
    Statement query(db, pragma);
    std::set<std::string> columns;
    int result;
    while ((result = sqlite3_step(query.value)) == SQLITE_ROW) {
        const auto* name = sqlite3_column_text(query.value, 1);
        if (!name) throw std::runtime_error("Could not read database schema.");
        columns.insert(reinterpret_cast<const char*>(name));
    }
    if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return columns;
}

int SchemaVersion(sqlite3* db) {
    Statement query(db, "PRAGMA user_version");
    if (sqlite3_step(query.value) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
    return sqlite3_column_int(query.value, 0);
}
} // namespace

Database::Database(const std::wstring& path) {
    if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "Could not open database.";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(error);
    }
    try {
        Exec("PRAGMA foreign_keys=ON");
        int version = SchemaVersion(db_);
        if (version == 0) {
            auto apps = Columns(db_, "PRAGMA table_info(apps)");
            auto usage = Columns(db_, "PRAGMA table_info(daily_usage)");
            bool empty = apps.empty() && usage.empty();
            bool legacy = apps == std::set<std::string>{"id", "path", "name", "total_ms"} &&
                          usage == std::set<std::string>{"app_id", "day", "duration_ms"};
            if (!empty && !legacy)
                throw std::runtime_error("Unrecognized database schema; no data was changed.");
            Exec("BEGIN IMMEDIATE");
            try {
                if (legacy) {
                    Exec("DROP TABLE daily_usage");
                    Exec("DROP TABLE apps");
                }
                Exec("CREATE TABLE apps ("
                     "id INTEGER PRIMARY KEY, path TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                     "name TEXT NOT NULL, tag TEXT NOT NULL DEFAULT '')");
                Exec("CREATE TABLE daily_usage ("
                     "id INTEGER PRIMARY KEY, app_id INTEGER NOT NULL REFERENCES apps(id),"
                     "run_id TEXT NOT NULL, day TEXT NOT NULL, duration_ms INTEGER NOT NULL,"
                     "add_timestamp TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                     "UNIQUE(run_id, day))");
                Exec("CREATE INDEX daily_usage_app_id ON daily_usage(app_id)");
                Exec("PRAGMA user_version=1");
                Exec("COMMIT");
            } catch (...) {
                try { Exec("ROLLBACK"); } catch (const std::exception&) {
                    throw std::runtime_error("Could not roll back database schema changes.");
                }
                throw;
            }
        } else if (version != 1) {
            throw std::runtime_error("Unsupported database schema version; no data was changed.");
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Database::~Database() { if (db_) sqlite3_close(db_); }

void Database::Exec(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void Database::EnsureApp(const std::wstring& path, const std::wstring& name) {
    Statement insertApp(db_, "INSERT OR IGNORE INTO apps(path,name) VALUES(?1,?2)");
    insertApp.Text(1, path);
    insertApp.Text(2, name);
    insertApp.Done(db_);
    Statement updateApp(db_, "UPDATE apps SET name=?2 WHERE path=?1");
    updateApp.Text(1, path);
    updateApp.Text(2, name);
    updateApp.Done(db_);
}

void Database::SetTag(int64_t id, const std::wstring& tag) {
    Statement update(db_, "UPDATE apps SET tag=?2 WHERE id=?1");
    update.Integer(1, id);
    update.Text(2, tag);
    update.Done(db_);
    if (sqlite3_changes(db_) != 1)
        throw std::runtime_error("Could not save tag: application no longer exists.");
}

void Database::Add(const std::wstring& path, const std::wstring& name,
                   const std::wstring& runId, const std::wstring& day, int64_t milliseconds) {
    if (milliseconds <= 0) return;
    EnsureApp(path, name);
    Statement insertDaily(db_, "INSERT INTO daily_usage(app_id,run_id,day,duration_ms) "
                               "SELECT id,?2,?3,?4 FROM apps WHERE path=?1");
    insertDaily.Text(1, path);
    insertDaily.Text(2, runId);
    insertDaily.Text(3, day);
    insertDaily.Integer(4, milliseconds);
    insertDaily.Done(db_);
    if (sqlite3_changes(db_) != 1)
        throw std::runtime_error("Could not insert app usage.");
}

std::vector<Row> Database::Rows(const std::wstring& day) {
    Statement query(db_, "SELECT a.id,a.path,a.name,a.tag,COALESCE(SUM(d.duration_ms),0),"
                         "COALESCE(SUM(CASE WHEN d.day=?1 THEN d.duration_ms ELSE 0 END),0) "
                         "FROM apps a LEFT JOIN daily_usage d ON d.app_id=a.id "
                         "GROUP BY a.id "
                         "ORDER BY a.name COLLATE NOCASE,a.path COLLATE NOCASE");
    query.Text(1, day);
    std::vector<Row> rows;
    int result;
    while ((result = sqlite3_step(query.value)) == SQLITE_ROW) {
        const auto* path = static_cast<const wchar_t*>(sqlite3_column_text16(query.value, 1));
        const auto* name = static_cast<const wchar_t*>(sqlite3_column_text16(query.value, 2));
        const auto* tag = static_cast<const wchar_t*>(sqlite3_column_text16(query.value, 3));
        if (!path || !name || !tag) throw std::runtime_error("Could not read app details from SQLite.");
        rows.push_back({sqlite3_column_int64(query.value, 0), path, name, tag,
                        sqlite3_column_int64(query.value, 4), sqlite3_column_int64(query.value, 5)});
    }
    if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return rows;
}
