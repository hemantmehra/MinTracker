#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

struct Row {
    std::wstring name;
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
    void Add(const std::wstring& path, const std::wstring& name,
             const std::wstring& day, int64_t milliseconds);
    std::vector<Row> Rows(const std::wstring& day);
};
