// =============================================================================
// src/core/sqlite_util.h — the small statement wrapper MemoryStore is built on
// =============================================================================
// Internal to the memory store's translation units (memory.cpp,
// memory_facts.cpp). Moved out of memory.cpp's anonymous namespace in 7.2 so
// MemoryStore can be implemented across more than one file.

#pragma once
#include "sqlite3.h"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace funes::sqlite {

struct Stmt {
    sqlite3_stmt* p = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare failed: ") +
                                     sqlite3_errmsg(db) + " — SQL: " + sql);
    }
    ~Stmt() { sqlite3_finalize(p); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind_text(int i, const std::string& s) {
        sqlite3_bind_text(p, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
    }
    void bind_int64(int i, int64_t v) { sqlite3_bind_int64(p, i, v); }
    void bind_double(int i, double v)  { sqlite3_bind_double(p, i, v); }
    void bind_blob(int i, const void* data, size_t bytes) {
        sqlite3_bind_blob(p, i, data, static_cast<int>(bytes), SQLITE_TRANSIENT);
    }
    bool step() {
        int rc = sqlite3_step(p);
        if (rc == SQLITE_ROW)  return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step failed: rc=") + std::to_string(rc));
    }
    std::string col_text(int i) {
        const unsigned char* t = sqlite3_column_text(p, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    int64_t col_int64(int i) { return sqlite3_column_int64(p, i); }
    double  col_double(int i) { return sqlite3_column_double(p, i); }
    std::vector<float> col_floats(int i) {
        const void* data = sqlite3_column_blob(p, i);
        const int bytes  = sqlite3_column_bytes(p, i);
        std::vector<float> v(bytes > 0 ? bytes / sizeof(float) : 0);
        if (data && bytes > 0) std::memcpy(v.data(), data, bytes);
        return v;
    }
};

inline void exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown sqlite error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg + " — SQL: " + sql);
    }
}

// ALTER TABLE ADD COLUMN is not idempotent and sqlite has no IF NOT EXISTS for
// it, so check the current shape first. Used for columns added after v1.0 to
// databases that are already in the field.
inline void add_column_if_missing(sqlite3* db, const char* table, const char* column,
                           const char* decl) {
    Stmt s(db, ("PRAGMA table_info(" + std::string(table) + ")").c_str());
    while (s.step())
        if (s.col_text(1) == column) return;
    exec_or_throw(db, ("ALTER TABLE " + std::string(table) + " ADD COLUMN " +
                       column + " " + decl + ";").c_str());
}

} // namespace funes::sqlite
