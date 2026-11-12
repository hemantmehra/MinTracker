#include "database.h"

#include <winsqlite\winsqlite3.h>

#include <stdexcept>

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

Database::~Database() { if (db_) sqlite3_close(db_); }

void Database::Exec(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void Database::Add(const std::wstring& path, const std::wstring& name,
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

std::vector<Row> Database::Rows(const std::wstring& day) {
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
