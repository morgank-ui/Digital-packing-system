-- ParkSmart KE database schema (SQLite). The same text is embedded in src/Database.hpp
-- and is applied automatically the first time the server starts.

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
