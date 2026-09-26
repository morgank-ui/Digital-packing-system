// ============================================================================
//  SlotManager.hpp  -  MODULE: Slot management & availability display data
//
//  DATA STRUCTURES (and why)
//  -------------------------
//  std::unordered_map<int, Slot>  slots_
//        Hash table: find any slot by its id in O(1) average time.
//  std::set<int>                  free_
//        Ordered set (red-black tree) of the ids of FREE slots.
//          - begin()   -> the free slot nearest the entrance     O(1)
//          - insert()  -> a slot becomes free again              O(log n)
//          - erase(id) -> allocate a specific slot the driver
//                         picked on the display                  O(log n)
//        A plain queue cannot remove an arbitrary slot and a plain vector needs
//        an O(n) scan to find a free one; the set does both efficiently.
//        Slot ids are handed out in the order bays were created (the default
//        car park is laid out nearest-first: zone A, then B, then C), so the
//        smallest free id is treated as the bay closest to the entrance.
//  std::unordered_map<string,int> maxNumber_
//        Highest bay number used in each zone, so new bays continue the sequence.
// ============================================================================
#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

enum class SlotStatus { Free, Occupied, Maintenance };

inline const char* toString(SlotStatus s) {
    switch (s) {
        case SlotStatus::Free:        return "FREE";
        case SlotStatus::Occupied:    return "OCCUPIED";
        case SlotStatus::Maintenance: return "MAINTENANCE";
    }
    return "FREE";
}

inline SlotStatus slotStatusFromString(const std::string& s) {
    if (s == "OCCUPIED") return SlotStatus::Occupied;
    if (s == "MAINTENANCE") return SlotStatus::Maintenance;
    return SlotStatus::Free;
}

struct Slot {
    int id;
    std::string zone;
    int number;
    std::string label;  // e.g. "A-01"
    SlotStatus status;
};

struct SlotCounts {
    int total = 0, free = 0, occupied = 0, maintenance = 0;
};

class SlotManager {
public:
    // Register a slot (used when loading from the database or adding new bays).
    void add(const Slot& slot) {
        slots_[slot.id] = slot;
        if (slot.status == SlotStatus::Free) free_.insert(slot.id);
        int& highest = maxNumber_[slot.zone];
        highest = std::max(highest, slot.number);
    }

    // Reserve the free slot closest to the entrance. Returns 0 if the lot is full.
    int allocateNearest() {
        if (free_.empty()) return 0;
        int id = *free_.begin();
        free_.erase(free_.begin());
        slots_[id].status = SlotStatus::Occupied;
        return id;
    }

    // Reserve a specific slot chosen by the driver. False if it is not free.
    bool allocate(int id) {
        auto it = slots_.find(id);
        if (it == slots_.end() || it->second.status != SlotStatus::Free) return false;
        free_.erase(id);
        it->second.status = SlotStatus::Occupied;
        return true;
    }

    // Give an occupied slot back to the pool.
    void release(int id) {
        auto it = slots_.find(id);
        if (it == slots_.end() || it->second.status != SlotStatus::Occupied) return;
        it->second.status = SlotStatus::Free;
        free_.insert(id);
    }

    // Close (on=true) or re-open (on=false) a slot. An occupied slot cannot be closed.
    bool setMaintenance(int id, bool on) {
        auto it = slots_.find(id);
        if (it == slots_.end()) return false;
        if (on) {
            if (it->second.status == SlotStatus::Occupied) return false;
            free_.erase(id);
            it->second.status = SlotStatus::Maintenance;
        } else if (it->second.status == SlotStatus::Maintenance) {
            it->second.status = SlotStatus::Free;
            free_.insert(id);
        }
        return true;
    }

    const Slot* find(int id) const {
        auto it = slots_.find(id);
        return it == slots_.end() ? nullptr : &it->second;
    }

    int freeCount() const { return static_cast<int>(free_.size()); }

    int nextNumberInZone(const std::string& zone) const {
        auto it = maxNumber_.find(zone);
        return (it == maxNumber_.end() ? 0 : it->second) + 1;
    }

    SlotCounts counts() const {
        SlotCounts c;
        for (const auto& kv : slots_) {
            ++c.total;
            switch (kv.second.status) {
                case SlotStatus::Free:        ++c.free; break;
                case SlotStatus::Occupied:    ++c.occupied; break;
                case SlotStatus::Maintenance: ++c.maintenance; break;
            }
        }
        return c;
    }

    // A copy of every slot, sorted for display (by zone, then bay number).
    std::vector<Slot> snapshot() const {
        std::vector<Slot> out;
        out.reserve(slots_.size());
        for (const auto& kv : slots_) out.push_back(kv.second);
        std::sort(out.begin(), out.end(), [](const Slot& a, const Slot& b) {
            return a.zone != b.zone ? a.zone < b.zone : a.number < b.number;
        });
        return out;
    }

private:
    std::unordered_map<int, Slot> slots_;
    std::set<int> free_;
    std::unordered_map<std::string, int> maxNumber_;
};
