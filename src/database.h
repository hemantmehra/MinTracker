#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

struct Row {
    int64_t id;
    std::wstring path;
    std::wstring name;
    std::wstring tag;
    int64_t total;
    int64_t today;
};

class Database {
    sqlite3* db_ = nullptr;
public:
    explicit Database(const std::wstring& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void Exec(const char* sql);
    void EnsureApp(const std::wstring& path, const std::wstring& name);
    void SetTag(int64_t id, const std::wstring& tag);
    void Add(const std::wstring& path, const std::wstring& name,
             const std::wstring& runId, const std::wstring& day, int64_t milliseconds);
    std::vector<Row> Rows(const std::wstring& day);
};
