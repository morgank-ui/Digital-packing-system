// ============================================================================
//  tests/test_core.cpp - quick self-checks for the core algorithms.
//  Build & run with:  make test
//  No test framework needed: CHECK() prints a line and the exit code is
//  non-zero if anything failed.
// ============================================================================
#include <iostream>

#include "../src/FeeCalculator.hpp"
#include "../src/ParkingService.hpp"  // for normalizePlate / prettyPlate
#include "../src/SlotManager.hpp"

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { ++failures; std::cout << "  FAIL  " << #cond << "  (line " << __LINE__ << ")\n"; } \
        else         { std::cout << "  ok    " << #cond << "\n"; }                \
    } while (0)

static FeeCalculator clientTariff() {
    FeeCalculator f;
    f.setTiers({{30, 0, "Up to 30 minutes"}, {120, 50, "Up to 2 hours"}, {240, 100, "Up to 4 hours"},
                {360, 300, "Up to 6 hours"}, {-1, 500, "Over 6 hours"}});
    return f;
}

int main() {
    std::cout << "Fee calculation (client tariff, boundaries are inclusive)\n";
    FeeCalculator f = clientTariff();
    CHECK(f.feeFor(0) == 0);
    CHECK(f.feeFor(30 * 60) == 0);          // exactly 30:00 -> still free
    CHECK(f.feeFor(30 * 60 + 1) == 50);     // 30:01 -> Kshs 50
    CHECK(f.feeFor(2 * 3600) == 50);        // exactly 2 h
    CHECK(f.feeFor(2 * 3600 + 1) == 100);
    CHECK(f.feeFor(4 * 3600) == 100);       // exactly 4 h
    CHECK(f.feeFor(4 * 3600 + 1) == 300);
    CHECK(f.feeFor(6 * 3600) == 300);       // exactly 6 h
    CHECK(f.feeFor(6 * 3600 + 1) == 500);   // over 6 h
    CHECK(f.feeFor(48 * 3600) == 500);
    CHECK(f.feeFor(-5) == 0);               // clock skew never produces a negative fee
    CHECK(f.secondsUntilNextTier(0) == 30 * 60 + 1);
    CHECK(f.secondsUntilNextTier(7 * 3600) == -1);

    bool rejected = false;
    try { FeeCalculator bad; bad.setTiers({{30, 0, "a"}, {20, 50, "b"}, {-1, 500, "c"}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);  // tiers out of order are refused

    std::cout << "Slot management\n";
    SlotManager m;
    m.add({1, "A", 1, "A-01", SlotStatus::Free});
    m.add({2, "A", 2, "A-02", SlotStatus::Free});
    m.add({3, "A", 3, "A-03", SlotStatus::Free});
    CHECK(m.allocateNearest() == 1);        // lowest id = nearest the entrance
    CHECK(m.allocate(3));                   // driver picks a specific bay
    CHECK(!m.allocate(3));                  // ...which can't be taken twice
    CHECK(m.allocateNearest() == 2);
    CHECK(m.allocateNearest() == 0);        // full
    m.release(1);
    CHECK(m.allocateNearest() == 1);        // released slot is reused
    m.release(2);
    CHECK(m.setMaintenance(2, true));       // close a free slot
    CHECK(m.allocateNearest() == 0);        // closed slot is not offered
    CHECK(!m.setMaintenance(3, true));      // an occupied slot cannot be closed
    m.setMaintenance(2, false);
    CHECK(m.freeCount() == 1);
    CHECK(m.nextNumberInZone("A") == 4);

    std::cout << "Number plates\n";
    CHECK(ParkingService::normalizePlate(" kda-123 a ") == "KDA123A");
    CHECK(ParkingService::prettyPlate("KDA123A") == "KDA 123A");
    CHECK(ParkingService::prettyPlate("KAA123") == "KAA 123");
    bool bad1 = false, bad2 = false;
    try { ParkingService::normalizePlate("!!"); } catch (const ApiError&) { bad1 = true; }
    try { ParkingService::normalizePlate("ABCDEF"); } catch (const ApiError&) { bad2 = true; }
    CHECK(bad1 && bad2);

    std::cout << (failures ? "\nSOME CHECKS FAILED\n" : "\nAll checks passed\n");
    return failures ? 1 : 0;
}
