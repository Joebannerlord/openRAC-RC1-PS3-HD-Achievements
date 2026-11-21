#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace achievements {
enum class Region { pal, ntsc };
enum class Grade { bronze, silver, gold, platinum };
struct Trophy { const char* id; const char* name; const char* description; Grade grade; };
struct Snapshot {
  std::bitset<30> skills; std::bitset<64> owned, obtained, gold_weapons;
  unsigned gold_bolts = 0; bool premium = false, ultra = false, drek_beaten = false,
       qwark_defeated = false; std::uint32_t wallet = 0;
  std::array<std::uint32_t,19> pickups{};
};
struct CampaignProgress { std::uint64_t weapons = 0, bolts = 0; std::uint64_t pickup_lower_bound = 0; };
struct Profile { std::bitset<35> unlocked; std::map<std::string, CampaignProgress> campaigns;
  std::map<std::string, std::string> checkpoints; };
using Unlocks = std::vector<std::size_t>;
struct Progress { std::uint64_t current = 0, total = 0; };
const std::array<Trophy, 35>& catalog();
std::optional<Snapshot> decode_save(const std::uint8_t* bytes, std::size_t size, Region region);
Unlocks evaluate(Profile& profile, const std::string& campaign, const Snapshot& snapshot,
                 std::uint64_t credited_bolts = 0);
std::optional<Progress> progress(const Profile&, const std::string& campaign,
                                 const Snapshot&, std::size_t catalog_index);
bool load_profile(const std::filesystem::path&, Profile&, std::string& error);
bool save_profile(const std::filesystem::path&, const Profile&, std::string& error);
} // namespace achievements
