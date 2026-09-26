// ============================================================================
//  ParkingService.hpp  -  The business logic of ParkSmart KE
//
//  MODULES implemented here (see README.md for the full design):
//    1. Vehicle entry / registration     -> registerEntry()
//    2. Slot availability display        -> status()
//    3. Exit quote (time + fee)          -> quote()
//    4. Payment                          -> pay()
//    5. Barrier control / vehicle exit   -> openBarrier()
//    6. Administration & reporting       -> activeTickets(), history(), report(),
//                                           addSlots(), setMaintenance(), events()
//
//  Fee maths lives in FeeCalculator, slot bookkeeping in SlotManager, storage in
//  Database. This class glues them together and keeps the database and the
//  in-memory data structures consistent.
//
//  DATA STRUCTURES used here
//    unordered_map<plate, ActiveTicket>  -> O(1) "is this vehicle inside?" lookup
//                                           at entry (duplicate check) and exit.
//    deque<Event> (bounded to 40)        -> a rolling activity feed; push_back /
//                                           pop_front are both O(1).
//
//  Every public method takes `mtx_`, so the many HTTP worker threads can never
//  interleave two operations (no double-allocation of one slot, no double exit).
//
//  Methods return ready-made JSON strings to keep the example compact.
// ============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "Database.hpp"
#include "FeeCalculator.hpp"
#include "Json.hpp"
#include "SlotManager.hpp"

// An error that maps straight onto an HTTP status code + a message for the user.
struct ApiError : std::runtime_error {
    int status;
    ApiError(int httpStatus, const std::string& message)
        : std::runtime_error(message), status(httpStatus) {}
};

// A vehicle that is currently inside the car park (mirror of an ACTIVE ticket row).
struct ActiveTicket {
    long long id;
    std::string plate;  // normalised, e.g. "KDA123A"
    int slotId;
    long long entryTime;  // epoch seconds
    int paidKes;          // total paid so far for this visit
};

class ParkingService {
public:
    ParkingService(const std::string& dbPath, bool demoMode) : db_(dbPath), demo_(demoMode) {
        seedIfEmpty();  // first run: create default slots + tariff
        loadState();    // rebuild the in-memory structures from the database
    }

    bool demoMode() const { return demo_; }

    // ======================================================================
    //  MODULE 2 - Availability display (what drivers see before entering)
    // ======================================================================
    std::string status() const {
        std::lock_guard<std::mutex> lock(mtx_);
        SlotCounts c = slots_.counts();
        std::vector<Slot> all = slots_.snapshot();

        // Per-zone totals, e.g. zone A: 10 bays, 7 free.
        std::map<std::string, std::pair<int, int>> perZone;  // zone -> {total, free}
        std::vector<std::string> slotJson;
        for (const Slot& s : all) {
            auto& z = perZone[s.zone];
            ++z.first;
            if (s.status == SlotStatus::Free) ++z.second;
            slotJson.push_back(json::Object()
                                   .set("id", s.id)
                                   .set("label", s.label)
                                   .set("zone", s.zone)
                                   .set("number", s.number)
                                   .set("status", toString(s.status))
                                   .dump());
        }
        std::vector<std::string> zoneJson;
        for (const auto& kv : perZone) {
            zoneJson.push_back(json::Object()
                                   .set("zone", kv.first)
                                   .set("total", kv.second.first)
                                   .set("free", kv.second.second)
                                   .dump());
        }
        return json::Object()
            .set("ok", true)
            .raw("summary", json::Object()
                                .set("total", c.total)
                                .set("free", c.free)
                                .set("occupied", c.occupied)
                                .set("maintenance", c.maintenance)
                                .dump())
            .raw("zones", json::array(zoneJson))
            .raw("slots", json::array(slotJson))
            .raw("tariff", tariffJson())
            .dump();
    }

    // ======================================================================
    //  MODULE 1 - Vehicle entry (record the vehicle on arrival)
    //  preferredSlot: id picked on the display, or 0 to auto-assign the nearest.
    // ======================================================================
    std::string registerEntry(const std::string& plateRaw, int preferredSlot) {
        const std::string plate = normalizePlate(plateRaw);
        std::lock_guard<std::mutex> lock(mtx_);

        // 1) One vehicle = one active ticket.
        if (activeByPlate_.count(plate))
            throw ApiError(409, prettyPlate(plate) + " is already inside the car park.");

        // 2) Is there room?
        if (slots_.freeCount() == 0)
            throw ApiError(409, "The car park is full. Please wait for a slot to be released.");

        // 3) Allocate a slot: the driver's choice, or the nearest free one.
        int slotId = 0;
        if (preferredSlot > 0) {
            if (!slots_.allocate(preferredSlot))
                throw ApiError(409, "That slot is not available. Pick another slot, or let the "
                                    "system choose the nearest one.");
            slotId = preferredSlot;
        } else {
            slotId = slots_.allocateNearest();
        }

        // 4) Persist. If anything fails, undo the in-memory allocation.
        const long long entry = now();
        long long ticketId = 0;
        try {
            Transaction tx(db_);
            Statement ins(db_.handle(),
                          "INSERT INTO tickets(plate, slot_id, entry_time) VALUES(?,?,?)");
            ins.bind(1, plate).bind(2, slotId).bind(3, entry).run();
            ticketId = db_.lastInsertId();

            Statement upd(db_.handle(),
                          "UPDATE parking_slots SET status='OCCUPIED' WHERE slot_id=?");
            upd.bind(1, slotId).run();

            auditDb("ENTRY", ticketCode(ticketId) + " " + prettyPlate(plate) + " -> " +
                                 slots_.find(slotId)->label);
            tx.commit();
        } catch (...) {
            slots_.release(slotId);
            throw;
        }

        // 5) Update the fast in-memory index.
        activeByPlate_[plate] = ActiveTicket{ticketId, plate, slotId, entry, 0};
        const Slot* slot = slots_.find(slotId);
        pushEvent("ENTRY", prettyPlate(plate) + " entered, slot " + slot->label);

        return json::Object()
            .set("ok", true)
            .set("ticket", ticketCode(ticketId))
            .set("plate", prettyPlate(plate))
            .set("slotId", slotId)
            .set("slot", slot->label)
            .set("entryTime", entry)
            .set("message", "Ticket issued. Barrier open - proceed to slot " + slot->label + ".")
            .dump();
    }

    // ======================================================================
    //  MODULE 3 - Exit quote: time spent + amount to pay right now
    // ======================================================================
    std::string quote(const std::string& plateRaw) const {
        const std::string plate = normalizePlate(plateRaw);
        std::lock_guard<std::mutex> lock(mtx_);
        return quoteJson(quoteLocked(plate));
    }

    // ======================================================================
    //  MODULE 4 - Payment
    //  expectedAmount: the amount the driver saw on screen (-1 = not supplied).
    //  If the fee moved to a higher band in the meantime we refuse rather than
    //  silently charging a different amount.
    // ======================================================================
    std::string pay(const std::string& plateRaw, const std::string& methodRaw,
                    std::string reference, int expectedAmount) {
        const std::string plate = normalizePlate(plateRaw);
        std::string method;
        for (char c : methodRaw) method += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (method != "MPESA" && method != "CASH" && method != "CARD")
            throw ApiError(400, "Choose a payment method: M-Pesa, cash or card.");

        std::lock_guard<std::mutex> lock(mtx_);
        Quote q = quoteLocked(plate);
        if (q.due == 0) throw ApiError(409, "Nothing to pay - the barrier can be opened.");
        if (expectedAmount >= 0 && expectedAmount != q.due)
            throw ApiError(409, "The fee changed to Kshs " + std::to_string(q.due) +
                                    ". Please review the new amount and pay again.");
        if (reference.size() > 40) reference.resize(40);
        // Demo only: a real deployment would receive the M-Pesa receipt from the
        // Safaricom Daraja API callback; here we generate a simulated reference.
        if (reference.empty())
            reference = "SIM-" + method + "-" + std::to_string(q.ticket->id) + "-" + std::to_string(q.now % 100000);

        const long long paidAt = now();
        long long paymentId = 0;
        {
            Transaction tx(db_);
            Statement ins(db_.handle(),
                          "INSERT INTO payments(ticket_id, amount_kes, method, reference, paid_at) "
                          "VALUES(?,?,?,?,?)");
            ins.bind(1, q.ticket->id).bind(2, q.due).bind(3, method).bind(4, reference).bind(5, paidAt).run();
            paymentId = db_.lastInsertId();
            auditDb("PAYMENT", ticketCode(q.ticket->id) + " Kshs " + std::to_string(q.due) + " via " + method);
            tx.commit();
        }
        const int amount = q.due;
        activeByPlate_[plate].paidKes += amount;
        pushEvent("PAYMENT", prettyPlate(plate) + " paid Kshs " + std::to_string(amount) + " (" + method + ")");

        return json::Object()
            .set("ok", true)
            .set("receipt", "RCT-" + zeroPad(paymentId, 6))
            .set("amount", amount)
            .set("method", method)
            .set("reference", reference)
            .set("paidAt", paidAt)
            .set("message", "Payment received. The barrier can now be opened.")
            .dump();
    }

    // ======================================================================
    //  MODULE 5 - Barrier control: opens ONLY when the fee is fully paid
    // ======================================================================
    std::string openBarrier(const std::string& plateRaw) {
        const std::string plate = normalizePlate(plateRaw);
        std::lock_guard<std::mutex> lock(mtx_);
        Quote q = quoteLocked(plate);

        // The rule at the heart of the assignment: no payment, no exit.
        if (q.due > 0)
            throw ApiError(402, "Barrier locked. Kshs " + std::to_string(q.due) + " is still due.");

        const long long exitTime = q.now;
        const ActiveTicket t = *q.ticket;  // copy: we erase the map entry below
        const Slot* slot = slots_.find(t.slotId);
        const std::string slotLabel = slot ? slot->label : "?";
        {
            Transaction tx(db_);
            Statement upd(db_.handle(),
                          "UPDATE tickets SET exit_time=?, fee_kes=?, status='COMPLETED' "
                          "WHERE ticket_id=?");
            upd.bind(1, exitTime).bind(2, q.fee).bind(3, t.id).run();

            Statement freeSlot(db_.handle(),
                               "UPDATE parking_slots SET status='FREE' WHERE slot_id=?");
            freeSlot.bind(1, t.slotId).run();

            auditDb("EXIT", ticketCode(t.id) + " " + prettyPlate(plate) + " left, fee Kshs " +
                                std::to_string(q.fee));
            tx.commit();
        }
        // Database committed -> now release the slot in memory and forget the ticket.
        slots_.release(t.slotId);
        activeByPlate_.erase(plate);
        pushEvent("EXIT", prettyPlate(plate) + " left, slot " + slotLabel + " is free");

        return json::Object()
            .set("ok", true)
            .set("barrier", "OPEN")
            .set("ticket", ticketCode(t.id))
            .set("plate", prettyPlate(plate))
            .set("slot", slotLabel)
            .set("entryTime", t.entryTime)
            .set("exitTime", exitTime)
            .set("durationSeconds", exitTime - t.entryTime)
            .set("fee", q.fee)
            .set("message", "Barrier open. Thank you for parking with ParkSmart KE.")
            .dump();
    }

    // ======================================================================
    //  MODULE 6 - Administration & reporting
    // ======================================================================

    // Vehicles currently inside, oldest first, with the fee they would pay now.
    std::string activeTickets() const {
        std::lock_guard<std::mutex> lock(mtx_);
        const long long t0 = now();
        std::vector<const ActiveTicket*> list;
        for (const auto& kv : activeByPlate_) list.push_back(&kv.second);
        std::sort(list.begin(), list.end(), [](const ActiveTicket* a, const ActiveTicket* b) {
            return a->entryTime < b->entryTime;
        });
        std::vector<std::string> rows;
        for (const ActiveTicket* a : list) {
            const long long secs = t0 - a->entryTime;
            const int fee = fees_.feeFor(secs);
            const Slot* s = slots_.find(a->slotId);
            rows.push_back(json::Object()
                               .set("ticket", ticketCode(a->id))
                               .set("plate", prettyPlate(a->plate))
                               .set("slot", s ? s->label : "?")
                               .set("entryTime", a->entryTime)
                               .set("durationSeconds", secs)
                               .set("fee", fee)
                               .set("paid", a->paidKes)
                               .set("due", std::max(0, fee - a->paidKes))
                               .dump());
        }
        return json::Object().set("ok", true).raw("tickets", json::array(rows)).dump();
    }

    // The most recent completed visits.
    std::string history(int limit) const {
        limit = std::max(1, std::min(limit, 200));
        std::lock_guard<std::mutex> lock(mtx_);
        Statement q(db_.handle(),
                    "SELECT t.ticket_id, t.plate, s.label, t.entry_time, t.exit_time, t.fee_kes, "
                    "       (SELECT COALESCE(SUM(amount_kes),0) FROM payments p "
                    "         WHERE p.ticket_id = t.ticket_id) "
                    "FROM tickets t JOIN parking_slots s ON s.slot_id = t.slot_id "
                    "WHERE t.status = 'COMPLETED' ORDER BY t.exit_time DESC LIMIT ?");
        q.bind(1, limit);
        std::vector<std::string> rows;
        while (q.step()) {
            rows.push_back(json::Object()
                               .set("ticket", ticketCode(q.getInt(0)))
                               .set("plate", prettyPlate(q.getText(1)))
                               .set("slot", q.getText(2))
                               .set("entryTime", q.getInt(3))
                               .set("exitTime", q.getInt(4))
                               .set("durationSeconds", q.getInt(4) - q.getInt(3))
                               .set("fee", q.getInt(5))
                               .set("paid", q.getInt(6))
                               .dump());
        }
        return json::Object().set("ok", true).raw("visits", json::array(rows)).dump();
    }

    // Daily figures for the manager's dashboard ("today" = midnight to midnight EAT).
    std::string report() const {
        std::lock_guard<std::mutex> lock(mtx_);
        const long long start = startOfTodayEAT(now());
        sqlite3* h = db_.handle();

        auto scalar = [&](const std::string& sql, long long param) {
            Statement q(h, sql);
            if (param >= 0) q.bind(1, param);
            return q.step() ? q.getInt(0) : 0LL;
        };
        const long long revenueToday =
            scalar("SELECT COALESCE(SUM(amount_kes),0) FROM payments WHERE paid_at >= ?", start);
        const long long revenueAll = scalar("SELECT COALESCE(SUM(amount_kes),0) FROM payments", -1);
        const long long enteredToday =
            scalar("SELECT COUNT(*) FROM tickets WHERE entry_time >= ?", start);
        const long long exitedToday =
            scalar("SELECT COUNT(*) FROM tickets WHERE exit_time >= ?", start);
        const long long avgSeconds = scalar(
            "SELECT CAST(COALESCE(AVG(exit_time - entry_time),0) AS INTEGER) FROM tickets "
            "WHERE status='COMPLETED' AND exit_time >= ?", start);

        std::vector<std::string> methods;
        {
            Statement q(h, "SELECT method, SUM(amount_kes) FROM payments WHERE paid_at >= ? "
                           "GROUP BY method ORDER BY method");
            q.bind(1, start);
            while (q.step())
                methods.push_back(json::Object().set("method", q.getText(0)).set("amount", q.getInt(1)).dump());
        }
        SlotCounts c = slots_.counts();
        return json::Object()
            .set("ok", true)
            .set("revenueToday", revenueToday)
            .set("revenueAllTime", revenueAll)
            .set("enteredToday", enteredToday)
            .set("exitedToday", exitedToday)
            .set("averageSecondsToday", avgSeconds)
            .set("insideNow", static_cast<int>(activeByPlate_.size()))
            .set("totalSlots", c.total)
            .raw("byMethod", json::array(methods))
            .dump();
    }

    // Add `count` new bays to a zone (creates the zone if it does not exist yet).
    std::string addSlots(std::string zone, int count) {
        for (char& c : zone) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (zone.empty() || zone.size() > 3 ||
            !std::all_of(zone.begin(), zone.end(), [](unsigned char c) { return std::isalnum(c); }))
            throw ApiError(400, "Zone must be 1-3 letters or digits, e.g. D.");
        if (count < 1 || count > 100) throw ApiError(400, "Add between 1 and 100 slots at a time.");

        std::lock_guard<std::mutex> lock(mtx_);
        if (slots_.counts().total + count > 2000) throw ApiError(400, "Slot limit (2000) reached.");

        std::vector<Slot> created;
        int number = slots_.nextNumberInZone(zone);
        {
            Transaction tx(db_);
            for (int i = 0; i < count; ++i, ++number) {
                Slot s{0, zone, number, slotLabel(zone, number), SlotStatus::Free};
                Statement ins(db_.handle(),
                              "INSERT INTO parking_slots(zone, slot_number, label) VALUES(?,?,?)");
                ins.bind(1, s.zone).bind(2, s.number).bind(3, s.label).run();
                s.id = static_cast<int>(db_.lastInsertId());
                created.push_back(s);
            }
            auditDb("ADMIN", "Added " + std::to_string(count) + " slot(s) to zone " + zone);
            tx.commit();
        }
        for (const Slot& s : created) slots_.add(s);
        pushEvent("ADMIN", "Added " + std::to_string(count) + " slot(s) to zone " + zone);
        return json::Object()
            .set("ok", true)
            .set("message", "Added " + std::to_string(count) + " slot(s) to zone " + zone + ".")
            .dump();
    }

    // Close a slot for maintenance (on=true) or re-open it (on=false).
    std::string setMaintenance(int slotId, bool on) {
        std::lock_guard<std::mutex> lock(mtx_);
        const Slot* s = slots_.find(slotId);
        if (!s) throw ApiError(404, "Unknown slot.");
        if (on && s->status == SlotStatus::Occupied)
            throw ApiError(409, "Slot " + s->label + " is occupied and cannot be closed yet.");
        const std::string label = s->label;
        {
            Transaction tx(db_);
            Statement upd(db_.handle(), "UPDATE parking_slots SET status=? WHERE slot_id=?");
            upd.bind(1, std::string(on ? "MAINTENANCE" : (s->status == SlotStatus::Occupied ? "OCCUPIED" : "FREE")))
                .bind(2, slotId)
                .run();
            auditDb("ADMIN", "Slot " + label + (on ? " closed for maintenance" : " re-opened"));
            tx.commit();
        }
        slots_.setMaintenance(slotId, on);
        pushEvent("ADMIN", "Slot " + label + (on ? " closed for maintenance" : " re-opened"));
        return json::Object()
            .set("ok", true)
            .set("message", "Slot " + label + (on ? " closed." : " re-opened."))
            .dump();
    }

    // Recent activity feed (newest first).
    std::string events() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::string> rows;
        for (auto it = events_.rbegin(); it != events_.rend(); ++it)
            rows.push_back(json::Object().set("time", it->time).set("type", it->type).set("text", it->text).dump());
        return json::Object().set("ok", true).raw("events", json::array(rows)).dump();
    }

    // DEMO ONLY: pretend a vehicle arrived `minutes` earlier so the fee bands can
    // be shown without waiting hours. Disabled unless the server runs with --demo.
    std::string simulateElapsed(const std::string& plateRaw, int minutes) {
        if (!demo_) throw ApiError(403, "Demo tools are disabled. Start the server with --demo.");
        const std::string plate = normalizePlate(plateRaw);
        if (minutes < 1 || minutes > 1440) throw ApiError(400, "Minutes must be between 1 and 1440.");
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = activeByPlate_.find(plate);
        if (it == activeByPlate_.end())
            throw ApiError(404, "No active parking session for " + prettyPlate(plate) + ".");
        Statement upd(db_.handle(), "UPDATE tickets SET entry_time = entry_time - ? WHERE ticket_id = ?");
        upd.bind(1, static_cast<long long>(minutes) * 60).bind(2, it->second.id).run();
        it->second.entryTime -= static_cast<long long>(minutes) * 60;
        pushEvent("DEMO", prettyPlate(plate) + " moved back " + std::to_string(minutes) + " min");
        return json::Object().set("ok", true).set("message", "Moved entry time back by " + std::to_string(minutes) + " minutes.").dump();
    }

    // ---------------------------------------------------------------------
    //  Small pure helpers (public so main.cpp can reuse them)
    // ---------------------------------------------------------------------
    static long long now() { return static_cast<long long>(std::time(nullptr)); }

    // "kda 123-a" -> "KDA123A". Rejects anything that is not a plausible plate.
    static std::string normalizePlate(const std::string& raw) {
        std::string p;
        for (unsigned char c : raw) {
            if (std::isalnum(c)) p += static_cast<char>(std::toupper(c));
            else if (c == ' ' || c == '-') continue;
            else throw ApiError(400, "Number plate may only contain letters and digits, e.g. KDA 123A.");
        }
        bool hasLetter = false, hasDigit = false;
        for (unsigned char c : p) (std::isalpha(c) ? hasLetter : hasDigit) = true;
        if (p.size() < 4 || p.size() > 9 || !hasLetter || !hasDigit)
            throw ApiError(400, "Enter a valid number plate, e.g. KDA 123A.");
        return p;
    }

    // "KDA123A" -> "KDA 123A" (standard Kenyan layout: 3 letters, 3 digits, optional letter).
    static std::string prettyPlate(const std::string& p) {
        if (p.size() >= 6 && p.size() <= 7 && std::isalpha((unsigned char)p[0]) &&
            std::isalpha((unsigned char)p[1]) && std::isalpha((unsigned char)p[2]) &&
            std::isdigit((unsigned char)p[3]) && std::isdigit((unsigned char)p[4]) &&
            std::isdigit((unsigned char)p[5]))
            return p.substr(0, 3) + " " + p.substr(3);
        return p;
    }

private:
    // ---- a snapshot of "what would this vehicle pay right now?" ------------
    struct Quote {
        const ActiveTicket* ticket;
        long long now;
        long long seconds;
        int fee;   // total fee for the time spent so far
        int paid;  // already paid for this visit
        int due;   // fee - paid (never negative)
    };

    // Caller must hold mtx_.
    Quote quoteLocked(const std::string& plate) const {
        auto it = activeByPlate_.find(plate);
        if (it == activeByPlate_.end())
            throw ApiError(404, "No active parking session for " + prettyPlate(plate) +
                                    ". Check the number plate.");
        Quote q;
        q.ticket = &it->second;
        q.now = now();
        q.seconds = std::max(0LL, q.now - it->second.entryTime);
        q.fee = fees_.feeFor(q.seconds);
        q.paid = it->second.paidKes;
        q.due = std::max(0, q.fee - q.paid);
        return q;
    }

    std::string quoteJson(const Quote& q) const {
        const Slot* s = slots_.find(q.ticket->slotId);
        const long long next = fees_.secondsUntilNextTier(q.seconds);
        return json::Object()
            .set("ok", true)
            .set("ticket", ticketCode(q.ticket->id))
            .set("plate", prettyPlate(q.ticket->plate))
            .set("slot", s ? s->label : "?")
            .set("entryTime", q.ticket->entryTime)
            .set("now", q.now)
            .set("durationSeconds", q.seconds)
            .set("tier", fees_.tierFor(q.seconds).description)
            .set("fee", q.fee)
            .set("paid", q.paid)
            .set("due", q.due)
            .set("canExit", q.due == 0)
            .set("nextIncreaseInSeconds", next)  // -1 when already in the top band
            .set("nextFee", next >= 0 ? fees_.nextFee(q.seconds) : q.fee)
            .dump();
    }

    std::string tariffJson() const {
        std::vector<std::string> rows;
        for (const FeeTier& t : fees_.tiers()) {
            json::Object o;
            if (t.maxMinutes >= 0) o.set("maxMinutes", t.maxMinutes); else o.raw("maxMinutes", "null");
            rows.push_back(o.set("fee", t.fee).set("description", t.description).dump());
        }
        return json::array(rows);
    }

    // ---- start-up ----------------------------------------------------------
    // First run only: build a small default car park (3 zones x 10 bays) and the
    // client's tariff. After that the database is the source of truth.
    void seedIfEmpty() {
        Transaction tx(db_);
        {
            Statement q(db_.handle(), "SELECT COUNT(*) FROM parking_slots");
            if (q.step() && q.getInt(0) == 0) {
                for (const char* zone : {"A", "B", "C"}) {
                    for (int n = 1; n <= 10; ++n) {
                        Statement ins(db_.handle(),
                                      "INSERT INTO parking_slots(zone, slot_number, label) VALUES(?,?,?)");
                        ins.bind(1, std::string(zone)).bind(2, n).bind(3, slotLabel(zone, n)).run();
                    }
                }
            }
        }
        {
            Statement q(db_.handle(), "SELECT COUNT(*) FROM fee_tiers");
            if (q.step() && q.getInt(0) == 0) {
                struct Row { int maxMin; int fee; const char* text; };
                const Row rows[] = {{30, 0, "Up to 30 minutes"},
                                    {120, 50, "Up to 2 hours"},
                                    {240, 100, "Up to 4 hours"},
                                    {360, 300, "Up to 6 hours"},
                                    {-1, 500, "Over 6 hours"}};
                for (const Row& r : rows) {
                    Statement ins(db_.handle(),
                                  "INSERT INTO fee_tiers(max_minutes, fee_kes, description) VALUES(?,?,?)");
                    if (r.maxMin < 0) ins.bindNull(1); else ins.bind(1, r.maxMin);
                    ins.bind(2, r.fee).bind(3, std::string(r.text)).run();
                }
            }
        }
        tx.commit();
    }

    // Rebuild every in-memory structure from the database.
    void loadState() {
        std::vector<FeeTier> tiers;
        {
            Statement q(db_.handle(),
                        "SELECT max_minutes, fee_kes, description FROM fee_tiers "
                        "ORDER BY (max_minutes IS NULL), max_minutes");
            while (q.step())
                tiers.push_back(FeeTier{q.isNull(0) ? -1 : static_cast<int>(q.getInt(0)),
                                        static_cast<int>(q.getInt(1)), q.getText(2)});
        }
        fees_.setTiers(std::move(tiers));

        {
            Statement q(db_.handle(),
                        "SELECT slot_id, zone, slot_number, label, status FROM parking_slots "
                        "ORDER BY slot_id");
            while (q.step())
                slots_.add(Slot{static_cast<int>(q.getInt(0)), q.getText(1),
                                static_cast<int>(q.getInt(2)), q.getText(3),
                                slotStatusFromString(q.getText(4))});
        }
        {
            Statement q(db_.handle(),
                        "SELECT t.ticket_id, t.plate, t.slot_id, t.entry_time, "
                        "       COALESCE(SUM(p.amount_kes), 0) "
                        "FROM tickets t LEFT JOIN payments p ON p.ticket_id = t.ticket_id "
                        "WHERE t.status = 'ACTIVE' GROUP BY t.ticket_id");
            while (q.step()) {
                ActiveTicket a{q.getInt(0), q.getText(1), static_cast<int>(q.getInt(2)),
                               q.getInt(3), static_cast<int>(q.getInt(4))};
                activeByPlate_[a.plate] = a;
            }
        }
    }

    // ---- formatting helpers -----------------------------------------------
    static std::string zeroPad(long long n, int width) {
        std::string s = std::to_string(n);
        return s.size() >= static_cast<std::size_t>(width) ? s : std::string(width - s.size(), '0') + s;
    }
    static std::string ticketCode(long long id) { return "PS-" + zeroPad(id, 6); }
    static std::string slotLabel(const std::string& zone, int number) {
        return zone + "-" + zeroPad(number, 2);
    }
    // Kenya is UTC+3 all year (no daylight saving).
    static long long startOfTodayEAT(long long t) {
        const long long offset = 3 * 3600;
        return ((t + offset) / 86400) * 86400 - offset;
    }

    // ---- audit trail + activity feed ---------------------------------------
    void auditDb(const std::string& event, const std::string& details) {
        Statement ins(db_.handle(), "INSERT INTO audit_log(event, details, created_at) VALUES(?,?,?)");
        ins.bind(1, event).bind(2, details).bind(3, now()).run();
    }
    struct Event { long long time; std::string type; std::string text; };
    void pushEvent(const std::string& type, const std::string& text) {
        events_.push_back(Event{now(), type, text});
        if (events_.size() > 40) events_.pop_front();  // keep only the latest 40
    }

    // ---- state ---------------------------------------------------------------
    mutable Database db_;  // mutable: read-only (const) queries still need a DB handle
    SlotManager slots_;
    FeeCalculator fees_;
    std::unordered_map<std::string, ActiveTicket> activeByPlate_;
    std::deque<Event> events_;
    mutable std::mutex mtx_;
    bool demo_;
};
