#include "runtime.hpp"
#include <algorithm>
namespace achievements::runtime {
std::uint64_t BoltTracker::pickups(const Snapshot&s){std::uint64_t n=0;for(auto x:s.pickups)n+=x;return n;}
void BoltTracker::rebase(const Snapshot&s,bool preserve){m_pickups=pickups(s);m_wallet=s.wallet;if(!preserve||!m_ready){m_pickup_high=m_pickups;m_wallet_high=m_wallet;m_rollback_guard=false;}else{m_pickup_high=std::max(m_pickup_high,m_pickups);m_wallet_high=std::max(m_wallet_high,m_wallet);m_rollback_guard=true;}m_ready=true;}
std::uint64_t BoltTracker::observe(const Snapshot&s){if(!m_ready){rebase(s,false);return 0;}const auto p=pickups(s);const auto pd=m_rollback_guard?(p>m_pickup_high?p-m_pickup_high:0):(p>=m_pickups?p-m_pickups:0);const auto wd=m_rollback_guard?(s.wallet>m_wallet_high?static_cast<std::uint64_t>(s.wallet-m_wallet_high):0):(s.wallet>=m_wallet?static_cast<std::uint64_t>(s.wallet-m_wallet):0);m_pickups=p;m_wallet=s.wallet;m_pickup_high=std::max(m_pickup_high,p);m_wallet_high=std::max(m_wallet_high,static_cast<std::uint64_t>(s.wallet));return std::max(pd,wd);}
} // namespace achievements::runtime
