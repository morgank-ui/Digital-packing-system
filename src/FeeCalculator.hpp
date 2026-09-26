// ============================================================================
//  FeeCalculator.hpp  -  MODULE: Fee calculation
//
//  Tariff required by the client:
//        up to 30 minutes ........ free
//        up to 2 hours ........... Kshs   50
//        up to 4 hours ........... Kshs  100
//        up to 6 hours ........... Kshs  300
//        over 6 hours ............ Kshs  500
//
//  DATA STRUCTURE : a sorted std::vector of tiers + a parallel vector of upper
//                   limits (in seconds).
//  ALGORITHM      : binary search (std::lower_bound) for the first limit that is
//                   >= the time spent.  O(log T) where T = number of tiers.
//                   A linear scan would also work for 5 tiers, but binary search
//                   keeps the lookup fast if the operator adds many more bands.
//
//  The tiers are loaded from the `fee_tiers` database table, so the tariff can
//  be changed without touching this code.
// ============================================================================
#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct FeeTier {
    int maxMinutes;           // upper limit of the band; negative => no limit
    int fee;                  // flat fee in Kenya shillings for this band
    std::string description;  // text shown to drivers
};

class FeeCalculator {
public:
    // Replace the tariff. Tiers must be sorted by maxMinutes (ascending) and
    // ONLY the last tier may be open-ended.
    void setTiers(std::vector<FeeTier> tiers) {
        if (tiers.empty()) throw std::invalid_argument("Tariff needs at least one fee tier");
        for (std::size_t i = 0; i < tiers.size(); ++i) {
            const bool isLast = (i + 1 == tiers.size());
            if (isLast != (tiers[i].maxMinutes < 0))
                throw std::invalid_argument("Only the final fee tier must be open-ended");
            if (tiers[i].fee < 0) throw std::invalid_argument("Fees cannot be negative");
            if (i > 0 && !isLast && tiers[i].maxMinutes <= tiers[i - 1].maxMinutes)
                throw std::invalid_argument("Fee tiers must have increasing time limits");
        }
        tiers_ = std::move(tiers);
        limitsSeconds_.clear();
        for (std::size_t i = 0; i + 1 < tiers_.size(); ++i)
            limitsSeconds_.push_back(tiers_[i].maxMinutes * 60LL);
    }

    // Index of the tier that applies after `seconds` of parking.
    // "up to X" is INCLUSIVE, so exactly 30:00 is still free and 30:01 costs Kshs 50.
    std::size_t indexFor(long long seconds) const {
        if (seconds < 0) seconds = 0;
        return static_cast<std::size_t>(
            std::lower_bound(limitsSeconds_.begin(), limitsSeconds_.end(), seconds) -
            limitsSeconds_.begin());
    }

    int feeFor(long long seconds) const { return tiers_[indexFor(seconds)].fee; }
    const FeeTier& tierFor(long long seconds) const { return tiers_[indexFor(seconds)]; }

    // Seconds until the driver crosses into the next (dearer) band, or -1 when
    // already in the last band. Used to warn drivers before the fee rises.
    long long secondsUntilNextTier(long long seconds) const {
        if (seconds < 0) seconds = 0;
        std::size_t idx = indexFor(seconds);
        if (idx >= limitsSeconds_.size()) return -1;
        return limitsSeconds_[idx] - seconds + 1;
    }
    // Fee of the next band (only meaningful when secondsUntilNextTier() >= 0).
    int nextFee(long long seconds) const {
        std::size_t idx = indexFor(seconds);
        return tiers_[std::min(idx + 1, tiers_.size() - 1)].fee;
    }

    const std::vector<FeeTier>& tiers() const { return tiers_; }

private:
    std::vector<FeeTier> tiers_;
    std::vector<long long> limitsSeconds_;  // upper limits of every band except the last
};
