#include "achievements/core.hpp"
#include "atomic_file.hpp"
#include <algorithm>
#include <charconv>
#include <utility>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#undef max
#undef min
#endif

namespace achievements { namespace {
constexpr std::array<int,20> skill_flags={0,2,3,5,6,7,8,9,10,13,14,16,17,19,20,22,23,26,28,29};
constexpr std::array<int,15> gadget_ids={9,10,11,13,14,15,16,17,18,19,20,21,23,24,25};
constexpr std::array<int,10> gold_ids={9,10,11,15,16,17,19,20,21,25};
constexpr std::array<unsigned,19> gold_per_level={0,3,4,3,1,2,2,2,2,2,2,1,2,1,4,2,2,2,3};
constexpr std::array<Trophy,35> trophies={{
 {"take-aim","Take Aim","Shoot down a Novalis fighter or bomber",Grade::bronze}, {"transported","Transported","Destroy three airborne transports on Aridia",Grade::bronze},
 {"strike-a-pose","Strike a Pose!","Stand between the legs of Al's statue",Grade::bronze}, {"qwarktastic","Qwarktastic","Destroy the training Qwarkbot",Grade::bronze},
 {"vehicle-bombardier","Vehicle Bombardier","Destroy ten Eudora vehicles",Grade::bronze}, {"twisted-hoverboard","Twisted Hoverboard","Land a Twisty McMarx",Grade::silver},
 {"cluck-cluck","Cluck, Cluck","Morph a tank into a chicken",Grade::bronze}, {"speedy","Speedy","Earn the Rilgar race skill point",Grade::gold},
 {"girl-trouble","Girl Trouble","Beat the Alien Queen with the wrench",Grade::silver}, {"eat-lead","Eat Lead","Destroy Batalia tanks with the Sand Mouse",Grade::silver},
 {"destroyed","Destroyed","Destroy the Blarg destroyers",Grade::bronze}, {"sniper","Sniper","Shoot a Screamer through the sniper hole",Grade::bronze},
 {"hey-over-here","Hey, Over Here!","Lure an enemy into an Orxon force field",Grade::bronze}, {"buried-treasure","Buried Treasure","Destroy underwater crates",Grade::bronze},
 {"pest-control","Pest Control","Clear Hoven's forest Anklebiters",Grade::bronze}, {"sitting-ducks","Sitting Ducks","Destroy Gemlik cargo ships",Grade::bronze},
 {"vandal","Vandal","Break Oltanis streetlights",Grade::silver}, {"magician","Magician","Score 4,500 on Kalebo III",Grade::bronze},
 {"careful-cruise","Careful Cruise","Clear Drek's Fleet swim without damage",Grade::bronze}, {"going-commando","Going Commando","Wrench ten elite guards",Grade::bronze},
 {"unlock-sketchbook","Unlock Sketchbook","Earn 15 skill points",Grade::silver}, {"skillful","Skillful","Earn all skill points",Grade::gold},
 {"bolt-commando","Bolt Commando","Collect all gold bolts",Grade::gold}, {"ratchet-king-midas","Ratchet, King Midas","Buy all gold weapons",Grade::gold},
 {"drek-this","Drek This!","Defeat Chairman Drek",Grade::gold}, {"ultimate-animal","Ultimate Animal","Acquire the RYNO",Grade::silver},
 {"gadgeteer","Gadgeteer","Acquire all weapons",Grade::silver}, {"nano-this","Nano This!","Acquire Ultra Nanotech",Grade::silver},
 {"bolt-collector","Bolt Collector","Collect 1,000,000 bolts",Grade::silver}, {"all-qwarkd-up","All Qwark'd Up","Defeat Qwark",Grade::silver},
 {"lombax-magnetic","Lombax Magnetic","Acquire Magneboots",Grade::bronze}, {"hit-me-more","Hit Me More!","Acquire Premium Nanotech",Grade::bronze},
 {"not-the-droids","These are not the droids you're looking for...","Acquire the Persuader",Grade::bronze}, {"very-sucky","Very Sucky","Acquire the Suck Cannon",Grade::bronze},
 {"gadget-master","Gadget Master","Unlock every other trophy",Grade::platinum}
}};
bool any_item(const Snapshot&s,int id){return id>=0&&id<64&&(s.owned[id]||s.obtained[id]);}
std::uint16_t crc(const std::uint8_t* p,size_t n){if(n>0x1800)return 0;std::uint32_t c=0xEDB88320;for(size_t j=0;j<n;++j){c^=std::uint32_t(p[j])<<8;for(int i=0;i<8;++i)c=(c&0x8000)?(c<<1)^0x1F45:c<<1;}return c&0xffff;}
std::uint32_t le(const std::uint8_t*p){return std::uint32_t(p[0])|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;}
struct C {int id; const std::uint8_t* p; size_t n;};
bool section(const std::uint8_t* b,size_t n,std::vector<C>&o){if(n<8)return false;auto z=le(b);if(z+8!=n||!le(b+4)||crc(b+8,z)!=le(b+4))return false;size_t a=8;while(a+8<=n){int id=(int)le(b+a);size_t k=le(b+a+4);if(id==-1)return k==0&&a+8==n;size_t q=(k+3)&~size_t(3);if(a+8+q>n)return false;o.push_back({id,b+a+8,k});a+=8+q;}return false;}
const C* find(const std::vector<C>&v,int id){for(auto&x:v)if(x.id==id)return &x;return nullptr;}
bool layout(const std::vector<C>&v,const std::vector<std::pair<int,size_t>>&x){if(v.size()!=x.size())return false;for(size_t i=0;i<x.size();++i)if(v[i].id!=x[i].first||v[i].n!=x[i].second)return false;return true;}
}
const std::array<Trophy,35>& catalog(){return trophies;}
std::optional<Snapshot> decode_save(const std::uint8_t* bytes, std::size_t size, Region) {
    // PAL and NTSC-U share the packet layout (OpenRAC launcher save_edit.rs).
    constexpr std::size_t global_size = 0x1530;
    constexpr std::size_t level_size = 0xaa4;
    constexpr std::size_t level_sections = 20;
    if (!bytes || size != 8 + global_size + level_sections * level_size
        || le(bytes) != global_size || le(bytes + 4) != level_size) return {};

    static const std::vector<std::pair<int, std::size_t>> global_layout = {{0,4},{1,4},{2,4},{3,4},{4,8},{5,128},{7,12},{8,32},{9,148},{10,37},{11,37},{12,12},{13,32},{19,4},{14,20},{20,80},{15,1936},{16,1184},{17,288},{18,296},{21,4},{22,4},{23,4},{24,4},{25,4},{26,4},{27,4},{28,1},{29,1},{30,40},{31,4},{32,28},{33,1},{34,4},{35,4},{36,4},{37,12},{1000,148},{1001,148},{1002,148},{1003,4},{1004,4},{1005,4},{1008,4},{1009,4},{1010,150},{1011,4}};
    static const std::vector<std::pair<int, std::size_t>> level_layout = {{3001,1},{3002,2048},{3003,4},{3004,16},{3005,256},{3006,256},{3007,8},{3008,16},{4000,4},{4001,4},{4002,4}};
    std::vector<C> global;
    if (!section(bytes + 8, global_size, global) || !layout(global, global_layout)) return {};
    const auto planet = le(find(global, 0)->p);
    const bool blank = planet == UINT32_MAX;
    const auto wallet = le(find(global, 1)->p);
    const auto completed = le(find(global, 2)->p);
    if (!blank && (planet >= gold_per_level.size() || wallet > 9'999'999 || completed > INT32_MAX)) return {};

    Snapshot snapshot;
    const auto* flags = find(global, 5)->p;
    const auto* skills = find(global, 8)->p;
    const auto* owned = find(global, 10)->p;
    const auto* acquired = find(global, 11)->p;
    const auto* gold = find(global, 30)->p;
    for (std::size_t i = 0; i < snapshot.skills.size(); ++i) snapshot.skills[i] = skills[i] != 0;
    for (std::size_t i = 0; i < 37; ++i) {
        snapshot.owned[i] = owned[i] != 0;
        snapshot.obtained[i] = acquired[i] != 0;
    }
    for (std::size_t i = 0; i < 40; ++i) snapshot.gold_weapons[i] = gold[i] != 0;
    snapshot.wallet = wallet;
    snapshot.drek_beaten = completed != 0 || le(find(global, 31)->p) != 0;
    snapshot.premium = flags[4] != 0;
    snapshot.ultra = flags[5] != 0;
    snapshot.qwark_defeated = flags[0x66] != 0;

    std::vector<C> level;
    for (std::size_t i = 0; i < level_sections; ++i) {
        level.clear();
        const auto* packet = bytes + 8 + global_size + i * level_size;
        if (!section(packet, level_size, level) || !layout(level, level_layout)) return {};
        if (i == gold_per_level.size() || blank) continue;
        const auto pickup_count = le(find(level, 4000)->p);
        if (pickup_count > INT32_MAX) return {};
        snapshot.pickups[i] = pickup_count;
        const auto* collected = find(level, 3003)->p;
        for (unsigned j = 0; j < gold_per_level[i]; ++j) snapshot.gold_bolts += collected[j] != 0;
    }
    return blank ? Snapshot{} : snapshot;
}

namespace {
constexpr std::size_t platinum_index = 34;
constexpr std::uint64_t bolt_goal = 1'000'000;

std::uint64_t historical_bolts(const Snapshot& snapshot) {
    std::uint64_t pickups = 0;
    for (auto count : snapshot.pickups) pickups += count;
    return std::max<std::uint64_t>(snapshot.wallet, pickups);
}

std::uint64_t weapon_mask(const Snapshot& snapshot) {
    return (snapshot.owned | snapshot.obtained).to_ullong();
}

std::uint64_t gadget_count(std::uint64_t mask) {
    return std::count_if(gadget_ids.begin(), gadget_ids.end(),
        [mask](int id) { return (mask & (std::uint64_t{1} << id)) != 0; });
}

std::uint64_t gold_weapon_count(const Snapshot& snapshot) {
    return std::count_if(gold_ids.begin(), gold_ids.end(),
        [&snapshot](int id) { return snapshot.gold_weapons[id]; });
}
} // namespace

Unlocks evaluate(Profile& profile, const std::string& key, const Snapshot& snapshot,
                 std::uint64_t credit) {
    auto& campaign = profile.campaigns[key];
    campaign.weapons |= weapon_mask(snapshot);
    campaign.pickup_lower_bound = std::max(campaign.pickup_lower_bound, historical_bolts(snapshot));
    const auto credited = campaign.bolts + std::min(credit, UINT64_MAX - campaign.bolts);
    campaign.bolts = std::max(credited, campaign.pickup_lower_bound);

    std::array<bool, platinum_index> earned{};
    for (std::size_t i = 0; i < skill_flags.size(); ++i) earned[i] = snapshot.skills[skill_flags[i]];
    earned[20] = snapshot.skills.count() >= 15;
    earned[21] = snapshot.skills.all();
    earned[22] = snapshot.gold_bolts == 40;
    earned[23] = gold_weapon_count(snapshot) == gold_ids.size();
    earned[24] = snapshot.drek_beaten;
    earned[25] = any_item(snapshot, 23);
    earned[26] = gadget_count(campaign.weapons) == gadget_ids.size();
    earned[27] = snapshot.ultra;
    earned[28] = campaign.bolts >= bolt_goal;
    earned[29] = snapshot.qwark_defeated;
    earned[30] = any_item(snapshot, 28);
    earned[31] = snapshot.premium || snapshot.ultra;
    earned[32] = any_item(snapshot, 35);
    earned[33] = any_item(snapshot, 9);

    Unlocks unlocked;
    for (std::size_t i = 0; i < earned.size(); ++i) {
        if (earned[i] && !profile.unlocked[i]) {
            profile.unlocked.set(i);
            unlocked.push_back(i);
        }
    }
    if (!profile.unlocked[platinum_index] && profile.unlocked.count() == platinum_index) {
        profile.unlocked.set(platinum_index);
        unlocked.push_back(platinum_index);
    }
    return unlocked;
}

std::optional<Progress> progress(const Profile& profile, const std::string& key,
                                 const Snapshot& snapshot, std::size_t index) {
    const auto entry = profile.campaigns.find(key);
    const auto campaign = entry == profile.campaigns.end() ? CampaignProgress{} : entry->second;
    switch (index) {
        case 20: return Progress{snapshot.skills.count(), 15};
        case 21: return Progress{snapshot.skills.count(), snapshot.skills.size()};
        case 22: return Progress{snapshot.gold_bolts, 40};
        case 23: return Progress{gold_weapon_count(snapshot), gold_ids.size()};
        case 26: return Progress{gadget_count(campaign.weapons | weapon_mask(snapshot)), gadget_ids.size()};
        case 28: return Progress{std::max(campaign.bolts, historical_bolts(snapshot)), bolt_goal};
        case platinum_index:
            return Progress{profile.unlocked.count() - profile.unlocked[platinum_index], platinum_index};
        default: return {};
    }
}

namespace {
constexpr auto profile_magic = "rac1-achievements 2";
constexpr std::size_t max_profile_bytes = 1024 * 1024;
constexpr std::size_t max_profile_rows = 4096;

bool valid_key(const std::string& key) {
    return !key.empty() && key.size() <= 256
        && std::all_of(key.begin(), key.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}

bool integer(std::istream& row, std::uint64_t& value) {
    std::string word;
    if (!(row >> word)) return false;
    const auto result = std::from_chars(word.data(), word.data() + word.size(), value);
    return result.ec == std::errc{} && result.ptr == word.data() + word.size();
}

bool read_row(std::istringstream& row, Profile& profile) {
    std::string kind, key;
    if (!(row >> kind >> key) || !valid_key(key)) return false;
    if (kind == "U") {
        const auto found = std::find_if(trophies.begin(), trophies.end(),
            [&key](const auto& trophy) { return key == trophy.id; });
        if (found == trophies.end()) return false;
        const auto index = static_cast<std::size_t>(found - trophies.begin());
        if (profile.unlocked[index]) return false;
        profile.unlocked.set(index);
    } else if (kind == "C") {
        CampaignProgress campaign;
        if (!integer(row, campaign.weapons) || !integer(row, campaign.bolts)
            || !integer(row, campaign.pickup_lower_bound) || campaign.pickup_lower_bound > campaign.bolts
            || !profile.campaigns.emplace(key, campaign).second) return false;
    } else if (kind == "K") {
        std::string campaign;
        if (!(row >> campaign) || !valid_key(campaign)
            || !profile.checkpoints.emplace(key, campaign).second) return false;
    } else {
        return false;
    }
    row >> std::ws;
    return row.eof();
}
} // namespace

bool load_profile(const std::filesystem::path& path, Profile& output, std::string& error) {
    error.clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "Cannot inspect the achievement profile.";
        return false;
    }
    if (!exists) return true;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > max_profile_bytes) {
        error = "Achievement profile is too large or unreadable.";
        return false;
    }
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != profile_magic) {
        error = "Invalid achievement profile header.";
        return false;
    }
    Profile profile;
    std::size_t rows = 0;
    while (std::getline(input, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::istringstream row(line);
        if (++rows > max_profile_rows || !read_row(row, profile)) {
            error = "Invalid achievement profile record.";
            return false;
        }
    }
    if (!input.eof()) {
        error = "Cannot read the complete achievement profile.";
        return false;
    }
    output = std::move(profile);
    return true;
}

bool save_profile(const std::filesystem::path& path, const Profile& profile, std::string& error) {
    error.clear();
    if (profile.unlocked.count() + profile.campaigns.size() + profile.checkpoints.size() > max_profile_rows) {
        error = "Too many achievement profile records.";
        return false;
    }
    std::ostringstream rows;
    rows << profile_magic << '\n';
    for (std::size_t i = 0; i < trophies.size(); ++i) {
        if (profile.unlocked[i]) rows << "U " << trophies[i].id << '\n';
    }
    for (const auto& [key, campaign] : profile.campaigns) {
        if (!valid_key(key)) {
            error = "Invalid campaign key.";
            return false;
        }
        rows << "C " << key << ' ' << campaign.weapons << ' ' << campaign.bolts
             << ' ' << campaign.pickup_lower_bound << '\n';
    }
    for (const auto& [key, campaign] : profile.checkpoints) {
        if (!valid_key(key) || !valid_key(campaign)) {
            error = "Invalid checkpoint key.";
            return false;
        }
        rows << "K " << key << ' ' << campaign << '\n';
    }
    const auto contents = rows.str();
    if (contents.size() > max_profile_bytes) {
        error = "Achievement profile is too large.";
        return false;
    }
    return write_file_atomically(path, contents, error);
}

bool write_file_atomically(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    error.clear();
    auto temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
        error = "Cannot write the temporary file.";
        return false;
    }
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
    std::filesystem::rename(temporary, path, ec);
    if (!ec) return true;
#endif
    error = "Cannot replace the saved file.";
    std::filesystem::remove(temporary, ec);
    return false;
}
} // namespace achievements
