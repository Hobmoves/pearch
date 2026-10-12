#pragma once

#include <string>
#include <optional>
#include <sqlite3.h>

class Database {
public:
    Database() = default;
    ~Database();

    // Open (or create) the database at the given path. Returns true on success.
    bool open(const std::wstring& path) noexcept;
    void close() noexcept;

    // Execute a SQL statement (no results expected)
    bool execute(const char* sql) noexcept;

    // Prepare a statement
    sqlite3_stmt* prepare(const char* sql) noexcept;

    sqlite3* raw() noexcept { return db_; }

private:
    sqlite3* db_ = nullptr;
};
