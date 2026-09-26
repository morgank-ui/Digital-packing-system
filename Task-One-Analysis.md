# Task One: System Analysis — ParkSmart KE

**Client brief:** an automated parking system for Kenya. Drivers see available bays before entry.
The system records vehicles on arrival, calculates time parked and the fee at exit, and opens the
barrier only once the fee is paid.

This document covers the three things Task One asks for: (a) an algorithm for each module, (b) the
data structures used and why, and (c) the design of a dynamic database. It reflects the actual
working system submitted alongside it (see `README.md`), so the two are consistent.

---

## 1. Analysis of the brief -> modules

Reading the client's terms of reference line by line:

| Client requirement | Module it implies |
|---|---|
| "drivers to see (visual display) of the parking slots available before entry" | **Availability Display** |
| "records vehicles on arrival" | **Vehicle Entry** |
| "on exit, the system automatically calculates total time spent ... and amount to pay" | **Fee Calculation** |
| "the barrier opens to allow exit on the payment of the parking fees" | **Payment**, then **Barrier Control** |
| (implicit — someone has to run the car park day to day) | **Administration & Reporting** |
| (implicit — every module above needs somewhere to keep its data) | **Data Storage** |

That gives seven modules: Availability Display, Vehicle Entry, Fee Calculation, Payment, Barrier
Control, Administration & Reporting, and Data Storage. They are described below in the order a
vehicle passes through them.

---

## 2. Algorithm for each module

Pseudocode only; see `src/*.hpp` for the C++ that implements each one.

### 2.1 Availability Display

```
FUNCTION getAvailability():
    FOR EACH slot IN allSlots:
        add slot.id, slot.zone, slot.label, slot.status to result
    counts <- count slots by status (FREE / OCCUPIED / MAINTENANCE)
    zoneTotals <- group counts by zone
    RETURN counts, zoneTotals, result, currentTariff
```

Runs on every page load and again every few seconds so the sign and the bay map stay current
without the driver refreshing anything.

### 2.2 Vehicle Entry

```
FUNCTION registerEntry(plate, chosenSlot = NONE):
    plate <- normalise(plate)                       // uppercase, strip spaces/dashes
    IF plate fails format check:                     RETURN error "invalid plate"
    IF plate already has an ACTIVE ticket:            RETURN error "already inside"
    IF no free slot exists:                           RETURN error "car park full"

    IF chosenSlot given:
        IF chosenSlot is not FREE:                    RETURN error "slot unavailable"
        slot <- chosenSlot
    ELSE:
        slot <- the FREE slot with the smallest id     // nearest to the entrance

    mark slot OCCUPIED
    entryTime <- now()
    create ticket(plate, slot, entryTime, status = ACTIVE)
    RETURN ticket, slot, "barrier open"
```

### 2.3 Fee Calculation

```
// tariff is an ordered list of tiers, e.g.
// [ (<=30 min, 0), (<=2h, 50), (<=4h, 100), (<=6h, 300), (>6h, 500) ]

FUNCTION feeFor(seconds):
    i <- first tier whose upper limit >= seconds      // binary search
    IF no such tier: i <- last tier (open-ended)
    RETURN tier[i].fee

FUNCTION quote(plate):
    ticket <- the ACTIVE ticket for plate
    IF none:                                          RETURN error "not inside"
    seconds <- now() - ticket.entryTime
    fee <- feeFor(seconds)
    due <- max(0, fee - ticket.amountAlreadyPaid)
    RETURN seconds, fee, due
```

### 2.4 Payment

```
FUNCTION pay(plate, method, amountShownToDriver):
    q <- quote(plate)
    IF q.due == 0:                                    RETURN error "nothing to pay"
    IF amountShownToDriver != q.due:                  // fee band changed while they waited
        RETURN error "amount changed, re-check"
    record payment(ticket, q.due, method, timestamp)
    ticket.amountPaid <- ticket.amountPaid + q.due
    RETURN receipt
```

### 2.5 Barrier Control

```
FUNCTION openBarrier(plate):
    q <- quote(plate)
    IF q.due > 0:                                     RETURN error "payment required" (barrier stays shut)
    ticket.exitTime <- now()
    ticket.status <- COMPLETED
    mark ticket.slot FREE
    RETURN "barrier open", durationParked, totalFee
```

The rule that matters most to the client — *no exit without payment* — is enforced here: the
barrier function itself refuses to run if anything is still owed, rather than trusting the screen
to have checked already.

### 2.6 Administration & Reporting

```
FUNCTION dailyReport():
    revenueToday   <- SUM(payments WHERE paid today)
    enteredToday   <- COUNT(tickets WHERE entryTime today)
    exitedToday    <- COUNT(tickets WHERE exitTime today)
    avgStayToday   <- AVERAGE(exitTime - entryTime WHERE exitTime today)
    RETURN revenueToday, enteredToday, exitedToday, avgStayToday

FUNCTION addSlots(zone, count):
    nextNumber <- highest existing slot number in zone + 1
    FOR i = 1 TO count:
        create slot(zone, nextNumber, status = FREE); nextNumber += 1

FUNCTION setMaintenance(slotId, close):
    IF close AND slot.status == OCCUPIED:             RETURN error "vehicle inside"
    slot.status <- close ? MAINTENANCE : FREE
```

### 2.7 Data Storage

Every write above (entry, payment, exit, adding a slot) is wrapped in a single database
transaction: either every change in that step is saved, or none are. This stops a power cut or a
crash mid-operation from leaving a slot marked OCCUPIED with no matching ticket, or a payment
recorded against a ticket that was never closed. See §4 for the schema itself.

---

## 3. Data structures used, and why

| Structure | Holds | Operations needed | Why this structure |
|---|---|---|---|
| **Balanced ordered set** (e.g. a red-black tree / `std::set` of slot ids) | ids of *free* slots only | "give me any free slot" (ideally the nearest); "remove a specific slot when it's taken"; "add a slot back when it's freed" | A queue can pop the front but can't remove an arbitrary chosen slot. A plain list needs to be scanned (O(n)) to find or remove a slot. A tree does all three operations in O(log n), and because it stays sorted, the smallest id — the bay nearest the entrance — is always the first element, found in O(1). |
| **Hash table** keyed by slot id | full details of every slot (zone, label, status) | "look up slot #57" | Slot ids are looked up constantly (on every entry, exit and status refresh). A hash table gives O(1) average lookup instead of scanning an array. |
| **Hash table** keyed by number plate | the one active ticket for each vehicle currently inside | "is this plate already parked?" (entry); "find this plate's ticket" (exit) | Both operations happen on every single request and must reject a duplicate entry instantly. O(1) average lookup by plate is exactly what a tree or list cannot offer without extra indexing. |
| **Sorted array/list of fee tiers** + **binary search** | the tariff (five bands in the brief, but any number) | "which band does this many seconds fall into?" | The tariff is small and static between rate changes, so a sorted array with `lower_bound`-style binary search is O(log T) — simpler than a hash table and naturally ordered, which also makes "how long until the fee rises" a one-line calculation. |
| **Bounded queue / deque** | the most recent ~40 activity events (entries, payments, exits) | "add the newest event"; "drop the oldest once the limit is reached" | A deque adds to one end and removes from the other in O(1), which is exactly a rolling activity feed — no need to keep the entire history in memory since it is already durably stored in the database. |
| **Relational tables (see §4)** | everything that must survive a restart: slots, tariff, tickets, payments, an audit trail | insert, update, join, aggregate (daily revenue, history) | In-memory structures are fast but disappear on a crash or a restart. A relational database keeps the permanent record and lets the report module use `SUM`/`COUNT`/`GROUP BY` directly instead of hand-rolled aggregation code. |

**In short:** fast, temporary lookups (is this slot free? is this plate inside?) live in memory using
trees and hash tables chosen for their O(1)/O(log n) operations; the permanent record lives in the
database. On start-up the in-memory structures are rebuilt from the database, so nothing is lost
between the two.

---

## 4. Dynamic database design

"Dynamic" here means two specific things the client's operation needs, and neither should require
touching the program's source code:

1. **The car park's physical layout can change.** A manager adds bays, closes a bay for
   maintenance, or reopens one — all while the system keeps running.
2. **The tariff can change.** If prices rise, the new fees must take effect without a recompile.

Both are solved the same way: **the layout and the tariff are rows in tables, not constants in the
code.** The program reads them at start-up and whenever they're changed through the admin API.

### 4.1 Entity-relationship overview

```
 fee_tiers                    parking_slots                    tickets                     payments
 ---------                    -------------                    -------                     --------
 tier_id (PK)                 slot_id (PK)          1        * ticket_id (PK)      1     * payment_id (PK)
 max_minutes (nullable)       zone                  <----------- slot_id (FK)      <------- ticket_id (FK)
 fee_kes                      slot_number                        plate                       amount_kes
 description                  label (unique)                     entry_time                  method
                               status                              exit_time (nullable)         reference
                                                                   fee_kes (nullable)            paid_at
                                                                   status

                                                       audit_log
                                                       ---------
                                                       log_id (PK), event, details, created_at
```

`parking_slots.slot_id  1 --- * tickets.slot_id`  (one bay hosts many visits over time, one at a time)
`tickets.ticket_id      1 --- * payments.ticket_id` (one visit can involve more than one payment)

### 4.2 Table design and the reasoning behind each choice

**`parking_slots`** — one row per physical bay.
- `zone` + `slot_number` are separate columns (rather than just `label`) so the system can order
  bays naturally and work out the next free number when a manager adds more.
- A `UNIQUE` constraint on `(zone, slot_number)` and on `label` stops two bays from being created
  with the same identity by mistake.
- `status` is constrained (`CHECK`) to `FREE`, `OCCUPIED` or `MAINTENANCE` — nothing else can ever
  be stored there, which rules out an entire class of bugs at the database level rather than
  trusting every part of the code to only write valid values.
- **Why this makes the layout dynamic:** adding a bay is `INSERT INTO parking_slots`; closing one
  for repair is `UPDATE ... SET status='MAINTENANCE'`. Neither needs a code change or a restart.

**`fee_tiers`** — one row per pricing band.
- `max_minutes` is nullable specifically so the *last* tier (over 6 hours, in the brief) can be
  open-ended — `NULL` means "no upper limit" instead of a magic number like `999999`.
- **Why this makes the tariff dynamic:** raising the Kshs 300 threshold to Kshs 350, or adding a
  new "over 12 hours" band, is a row `INSERT`/`UPDATE` against this table. The fee-calculation
  module always reads whatever rows are currently there — it has no fee amounts written into it.

**`tickets`** — one row per vehicle visit, from entry to exit.
- `exit_time` and `fee_kes` are nullable because they genuinely don't exist yet for a vehicle still
  parked; the row is created at entry and completed at exit rather than being created twice.
- A **partial unique index** on `plate` where `status='ACTIVE'` is the database itself enforcing
  "one vehicle can't have two active tickets" — even if two entry requests for the same plate
  arrived at the exact same instant, the database would reject the second one.
- `entry_time`/`exit_time` are indexed because reporting (today's arrivals, average stay) filters
  and sorts on them constantly.

**`payments`** — one row per amount received.
- Kept separate from `tickets` (rather than one payment column) because a single visit can
  legitimately involve more than one payment — a driver who pays, then stays long enough to cross
  into a dearer band, owes a top-up, which is a second row rather than overwriting the first.
- `method` is constrained to `MPESA` / `CASH` / `CARD` so reporting by payment method (something a
  manager will realistically ask for) is a simple `GROUP BY`.

**`audit_log`** — an append-only trail of what happened and when (entries, payments, exits, admin
changes). Nothing is ever deleted from it; if a dispute comes up ("I paid but the barrier didn't
open"), this table is the record of exactly what the system did and in what order.

### 4.3 Why a relational design rather than files or a single flat table

A single flat table (e.g. one row per vehicle with every field in it) cannot cleanly represent
"a bay hosts many visits over its lifetime" or "one visit can have more than one payment" — both
are one-to-many relationships. Separating them into `parking_slots`, `tickets` and `payments` and
linking them by ids means:
- an update only ever touches the table it actually concerns (paying doesn't rewrite the ticket),
- a report can `JOIN` exactly the tables it needs without scanning irrelevant columns,
- and constraints (uniqueness, checked status values, the one-active-ticket-per-plate rule) are
  enforced by the database itself, rather than relying on every part of the program to remember to
  check them.

---

## 5. How this maps to the delivered system

Everything above is implemented, not just designed: `src/Database.hpp` creates exactly this schema
on first start-up (also in `docs/schema.sql`), `src/SlotManager.hpp` and `src/FeeCalculator.hpp`
hold the in-memory structures from §3, and `src/ParkingService.hpp` is the pseudocode from §2
written in C++. Module-by-module and structure-by-structure detail with file references is in
`README.md`.
