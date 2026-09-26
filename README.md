# ParkSmart KE

An automated, web-based car park system written in **C++17** for the Multimedia University of Kenya
*Data Structures and Algorithms* assignment ("Actual System Development").

Drivers see a live map of free bays before they enter. The system records each vehicle on arrival,
works out the time parked and the fee when it leaves, and the exit barrier opens only after the fee is paid.

## Tariff (from the client's brief)

| Time parked          | Fee        |
|----------------------|------------|
| Up to 30 minutes     | Free       |
| Up to 2 hours        | Kshs 50    |
| Up to 4 hours        | Kshs 100   |
| Up to 6 hours        | Kshs 300   |
| Over 6 hours         | Kshs 500   |

"Up to" is inclusive: exactly 30:00 is free, 30:01 costs Kshs 50. Each band is a flat fee (not cumulative).
The tariff is stored in the database (`fee_tiers`), so it can be changed without recompiling.

## Run it

Requirements: a C++17 compiler and the SQLite3 development library.
`httplib.h` (cpp-httplib, MIT licence) is already included in `third_party/`.

```bash
# Ubuntu / Debian / WSL
sudo apt install g++ make libsqlite3-dev
# macOS:   brew install sqlite
# Windows: install MSYS2, then  pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sqlite3 make

make            # build
make test       # run the self-checks (fees, slots, plates)
make run        # start in demo mode -> open http://localhost:8080
```

CMake works too: `cmake -B build && cmake --build build && ./build/parksmart --demo`

Options: `--port 8080  --host 127.0.0.1  --db parksmart.db  --web web  --demo`
Manager password: environment variable `PARK_ADMIN_PASSWORD` (default `admin123`; change it before real use).
`--demo` enables buttons that move a vehicle's arrival time back, so you can show every fee band without waiting hours.

## Try the whole flow (about 2 minutes)

1. **Availability** tab: LED sign with free bays, per-zone counts, the tariff and the bay map.
2. **Entry gate**: type `KDA 123A`, optionally click a free bay, press *Issue ticket*. The barrier opens and a ticket is shown.
3. **Manager** tab: sign in, then use the demo buttons on the vehicle to move its arrival back by 3 h.
4. **Exit and payment**: enter the plate, see time parked and Kshs 100 due, choose M-Pesa / Cash / Card, pay, then *Open barrier*.
   Trying the barrier before paying is refused by the server (HTTP 402).

## Modules

| # | Module | Where | Job |
|---|--------|-------|-----|
| 1 | Vehicle entry | `ParkingService::registerEntry` | validate plate, reject duplicates, allocate a bay, issue ticket |
| 2 | Availability display | `ParkingService::status`, `web/index.html` | live slot map, counts per zone, tariff |
| 3 | Fee calculation | `FeeCalculator.hpp` | time parked -> fee |
| 4 | Payment | `ParkingService::pay` | record M-Pesa / cash / card payment, top-ups if a higher band is reached |
| 5 | Barrier control | `ParkingService::openBarrier` | open only when amount due is 0, close the ticket, free the bay |
| 6 | Administration | `ParkingService` admin methods | revenue report, vehicles inside, history, add bays, close bays for maintenance |
| 7 | Storage | `Database.hpp` | SQLite wrapper, transactions, schema |

## Data structures and why

| Structure | Used for | Why |
|-----------|----------|-----|
| `std::set<int>` (balanced tree) | ids of free bays | nearest free bay = `begin()` in O(1); free / reserve any bay in O(log n) |
| `std::unordered_map<int, Slot>` | all bays | look up a bay by id in O(1) |
| `std::unordered_map<string, ActiveTicket>` | vehicles currently inside, keyed by plate | O(1) duplicate check at entry and O(1) lookup at exit |
| sorted `std::vector<FeeTier>` + binary search | tariff | fee lookup in O(log T) |
| `std::deque<Event>` (max 40) | recent activity feed | O(1) push at the back, O(1) drop from the front |

The in-memory structures are rebuilt from the database at start-up, so a restart loses nothing.

## Algorithms (summary)

**Entry:** normalise plate -> reject if already inside -> reject if no free bay -> allocate (chosen bay or `set.begin()`) -> one DB transaction (ticket + bay status + audit) -> update memory.

**Fee:** `seconds = now - entry`; `i = lower_bound(limits, seconds)`; `fee = tiers[i].fee`.

**Exit:** `due = max(0, fee(now) - paid)`. Pay records exactly `due`. Barrier opens only when `due == 0`, then in one transaction the ticket is completed and the bay freed.
If a driver pays and then lingers into a dearer band, only the difference is due.

## Database (SQLite, `docs/schema.sql`)

```
parking_slots (slot_id PK, zone, slot_number, label UNIQUE, status FREE|OCCUPIED|MAINTENANCE)
fee_tiers     (tier_id PK, max_minutes NULL=unlimited, fee_kes, description)
tickets       (ticket_id PK, plate, slot_id FK->parking_slots, entry_time, exit_time, fee_kes, status ACTIVE|COMPLETED)
payments      (payment_id PK, ticket_id FK->tickets, amount_kes, method MPESA|CASH|CARD, reference, paid_at)
audit_log     (log_id PK, event, details, created_at)
```

It is *dynamic*: bays and tariff bands are rows, not constants. A manager can add bays and close bays at run time.
A partial unique index guarantees one active ticket per plate even if two requests arrive together.

## REST API

| Method and path | Purpose |
|----------------|---------|
| `GET /api/status` | slots, counts, tariff |
| `POST /api/entry` (`plate`, optional `slot_id`) | issue a ticket |
| `GET /api/exit/quote?plate=` | time parked, fee, amount due |
| `POST /api/exit/pay` (`plate`, `method`, optional `reference`, `amount`) | pay |
| `POST /api/exit/barrier` (`plate`) | open barrier (402 if unpaid) |
| `GET /api/admin/{tickets,history,report,events}` | manager views (header `X-Admin-Key`) |
| `POST /api/admin/slots` (`zone`, `count`) | add bays |
| `POST /api/admin/maintenance` (`slot_id`, `on=1|0`) | close or open a bay |
| `POST /api/admin/simulate` (`plate`, `minutes`) | demo only |

## Limits and assumptions

* M-Pesa is simulated: a real deployment would use the Safaricom Daraja STK Push API and its callback.
* The barrier is a simulated on-screen device; a real one would be driven through a relay or controller.
* Number plates are typed in; a real car park would use camera-based recognition (ANPR).
* Manager login is a single shared password for a course project. Production needs hashed passwords, sessions and HTTPS.
* Times are stored as UTC epoch seconds; "today" in the report is midnight to midnight East Africa Time (UTC+3).

## Publish to GitHub

```bash
git init
git add .
git commit -m "ParkSmart KE: automated parking system in C++"
git branch -M main
git remote add origin https://github.com/<your-username>/parksmart-ke.git
git push -u origin main
```

## Project layout

```
src/main.cpp              web server and REST routes
src/ParkingService.hpp    business logic (modules 1-6)
src/SlotManager.hpp       bay allocation data structures
src/FeeCalculator.hpp     tariff and fee lookup
src/Database.hpp          SQLite wrapper and schema
src/Json.hpp              tiny JSON writer
web/index.html            single-page web interface
tests/test_core.cpp       self-checks
docs/schema.sql           database schema
third_party/httplib.h     cpp-httplib (MIT)
```
