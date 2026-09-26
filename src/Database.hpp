// ============================================================================
//  Database.hpp  -  MODULE: Persistent storage (SQLite)
//
//  * RAII wrappers around the SQLite C API (Database, Statement, Transaction)
//    so that handles are always released, even when an exception is thrown.
//  * The schema is created automatically on first start-up.
//
//  "Dynamic database": nothing about the car park is hard-coded in the code.
//  Slots (parking_slots) and the tariff (fee_tiers) are ROWS, so an operator can
//  add slots, close slots for maintenance or change the fees at run time without
//  recompiling the program.
// ============================================================================
#pragma once

#include <sqlite3.h>

#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
//  Database schema (kept as one string; a copy lives in docs/schema.sql)
// ---------------------------------------------------------------------------
// SCHEMA-BEGIN
static const char* const kSchema = R"SQL(
-- Every physical bay in the car park. Rows can be added at run time.
CREATE TABLE IF NOT EXISTS parking_slots (
    slot_id     INTEGER PRIMARY KEY AUTOINCREMENT,
    zone        TEXT    NOT NULL,                       -- e.g. 'A'
    slot_number INTEGER NOT NULL,                       -- 1, 2, 3 ... inside the zone
    label       TEXT    NOT NULL UNIQUE,                -- e.g. 'A-01'
    status      TEXT    NOT NULL DEFAULT 'FREE'
                CHECK (status IN ('FREE','OCCUPIED','MAINTENANCE')),
    UNIQUE (zone, slot_number)
);

-- The tariff. One row per band; max_minutes NULL means "no upper limit".
CREATE TABLE IF NOT EXISTS fee_tiers (
    tier_id     INTEGER PRIMARY KEY AUTOINCREMENT,
    max_minutes INTEGER,
    fee_kes     INTEGER NOT NULL CHECK (fee_kes >= 0),
    description TEXT    NOT NULL
);

-- One row per visit (arrival -> exit). Times are UNIX epoch seconds (UTC).
CREATE TABLE IF NOT EXISTS tickets (
    ticket_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    plate       TEXT    NOT NULL,                       -- normalised, e.g. 'KDA123A'
    slot_id     INTEGER NOT NULL REFERENCES parking_slots(slot_id),
    entry_time  INTEGER NOT NULL,
    exit_time   INTEGER,                                -- NULL while still parked
    fee_kes     INTEGER,                                -- final fee, set at exit
    status      TEXT    NOT NULL DEFAULT 'ACTIVE'
                CHECK (status IN ('ACTIVE','COMPLETED'))
);
-- A vehicle can only have ONE active ticket at a time (enforced by the database).
CREATE UNIQUE INDEX IF NOT EXISTS ux_tickets_active_plate
    ON tickets(plate) WHERE status = 'ACTIVE';
CREATE INDEX IF NOT EXISTS ix_tickets_entry ON tickets(entry_time);
CREATE INDEX IF NOT EXISTS ix_tickets_exit  ON tickets(exit_time);

-- Money received. A ticket can have several payments (e.g. a top-up if the
-- driver lingers after paying and moves into a higher fee band).
CREATE TABLE IF NOT EXISTS payments (
    payment_id  INTEGER PRIMARY KEY AUTOINCREMENT,
    ticket_id   INTEGER NOT NULL REFERENCES tickets(ticket_id),
    amount_kes  INTEGER NOT NULL CHECK (amount_kes > 0),
    method      TEXT    NOT NULL CHECK (method IN ('MPESA','CASH','CARD')),
    reference   TEXT,                                   -- M-Pesa code / receipt no.
    paid_at     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_payments_ticket ON payments(ticket_id);
CREATE INDEX IF NOT EXISTS ix_payments_paid   ON payments(paid_at);

-- Append-only trail of gate events (entry, payment, barrier, admin actions).
CREATE TABLE IF NOT EXISTS audit_log (
    log_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    event       TEXT    NOT NULL,
    details     TEXT,
    created_at  INTEGER NOT NULL
);
)SQL";
// SCHEMA-END

// ---------------------------------------------------------------------------
//  Statement: a prepared SQL statement (RAII).
// ---------------------------------------------------------------------------
class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQL prepare failed: ") + sqlite3_errmsg(db_));
        }
    }
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Bind parameters (index starts at 1). Binding keeps user input out of the
    // SQL text, which is what prevents SQL-injection.
    Statement& bind(int index, long long value) {
        check(sqlite3_bind_int64(stmt_, index, value));
        return *this;
    }
    Statement& bind(int index, const std::string& value) {
        check(sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                                SQLITE_TRANSIENT));
        return *this;
    }
    Statement& bindNull(int index) {
        check(sqlite3_bind_null(stmt_, index));
        return *this;
    }

    // Advance to the next row. Returns true while a row is available.
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("SQL step failed: ") + sqlite3_errmsg(db_));
    }
    // Execute a statement that returns no rows (INSERT / UPDATE / DELETE).
    void run() { step(); }

    // Read columns of the current row (index starts at 0).
    long long getInt(int col) const { return sqlite3_column_int64(stmt_, col); }
    std::string getText(int col) const {
        const unsigned char* t = sqlite3_column_text(stmt_, col);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    bool isNull(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

private:
    void check(int rc) const {
        if (rc != SQLITE_OK) {
            throw std::runtime_error(std::string("SQL bind failed: ") + sqlite3_errmsg(db_));
        }
    }
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

// ---------------------------------------------------------------------------
//  Database: owns the connection.
// ---------------------------------------------------------------------------
class Database {
public:
    explicit Database(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            if (db_) sqlite3_close(db_);
            throw std::runtime_error("Cannot open database '" + path + "': " + msg);
        }
        sqlite3_busy_timeout(db_, 5000);
        exec("PRAGMA foreign_keys = ON;");   // enforce REFERENCES
        exec("PRAGMA journal_mode = WAL;");  // readers do not block the writer
        exec(kSchema);                       // create tables on first run
    }
    ~Database() {
        if (db_) sqlite3_close(db_);
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Run one or more SQL statements that take no parameters.
    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("SQL error: " + msg);
        }
    }

    sqlite3* handle() { return db_; }
    long long lastInsertId() const { return sqlite3_last_insert_rowid(db_); }

private:
    sqlite3* db_ = nullptr;
};

// ---------------------------------------------------------------------------
//  Transaction: BEGIN ... COMMIT, or automatic ROLLBACK if commit() is never
//  reached (for example because an exception was thrown half-way through).
//  This keeps multi-table updates ALL-OR-NOTHING.
// ---------------------------------------------------------------------------
class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE;"); }
    ~Transaction() {
        if (!finished_) {
            try { db_.exec("ROLLBACK;"); } catch (...) { /* nothing more we can do */ }
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        db_.exec("COMMIT;");
        finished_ = true;
    }

private:
    Database& db_;
    bool finished_ = false;
};
