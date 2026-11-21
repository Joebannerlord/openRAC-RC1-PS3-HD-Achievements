#pragma once
#include "achievements/core.hpp"
#include <cstdint>
namespace achievements::runtime {
// Tracks only new ordinary-play bolt evidence. It does not own campaign keys,
// persistence, host lifecycle, or guest memory.
class BoltTracker {
public:
    void rebase(const Snapshot&, bool preserve_pickup_high_water);
    std::uint64_t observe(const Snapshot&);
private:
    static std::uint64_t pickups(const Snapshot&);
    std::uint64_t m_pickups=0,m_wallet=0,m_pickup_high=0,m_wallet_high=0;
    bool m_ready=false,m_rollback_guard=false;
};
} // namespace achievements::runtime
