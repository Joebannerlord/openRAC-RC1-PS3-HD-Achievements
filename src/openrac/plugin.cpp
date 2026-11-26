#include "runtime.hpp"
#include "launcher_summary.hpp"
#include "trophy_toast.hpp"
#include "../core/atomic_file.hpp"

#include <openrac/games/rac1.hpp>
#include <openrac/mod.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace achievements;

// Both retail regions serialize the same header and 21 packets (SDK rac1_save.h).
constexpr std::size_t global_bytes = 0x1530;
constexpr std::size_t level_bytes = 0xaa4;
constexpr unsigned level_packets = 20;
constexpr unsigned playing_levels = 19;
constexpr std::size_t save_bytes = 8 + global_bytes + level_packets * level_bytes;
constexpr std::uint32_t guest_ram_bytes = 32u << 20;
using SaveBytes = std::array<std::uint8_t, save_bytes>;
using Fingerprint = std::uint64_t;

Fingerprint hash(const std::uint8_t* bytes, std::size_t size) {
    Fingerprint value = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        value *= 1099511628211ull;
    }
    return value;
}

bool equal_progress(const CampaignProgress& a, const CampaignProgress& b) {
    return a.bolts == b.bolts && a.weapons == b.weapons && a.pickup_lower_bound == b.pickup_lower_bound;
}

std::filesystem::path toast_icon_directory(const openrac::Mod& mod) {
    if (!mod.has_slot14(offsetof(openrac_api, plugins_folder)) || !mod.api().plugins_folder) return {};
    const auto plugins = mod.plugins_folder();
    if (plugins.empty()) return {};
    return std::filesystem::u8path(plugins) / "rac1-hd-achievements" / "ui" / "icons";
}

struct Restore {
    std::array<Fingerprint, 1 + level_packets> packets{};
    std::bitset<1 + level_packets> received;
    std::uint32_t loads;
    bool internal;

    bool matches(const SaveBytes& bytes) const {
        if (!received.all() || packets[0] != hash(bytes.data() + 8, global_bytes)) return false;
        for (unsigned i = 0; i < level_packets; ++i) {
            const auto* part = bytes.data() + 8 + global_bytes + i * level_bytes;
            if (packets[i + 1] != hash(part, level_bytes)) return false;
        }
        return true;
    }
};

struct PendingWrite {
    int slot;
    Fingerprint expected;
    unsigned attempts = 0;
};

class Adapter {
public:
    explicit Adapter(openrac::Mod& mod)
        : mod_(mod), api_(mod.api()), disc_(mod.version()),
          region_(disc_ == "pal" ? Region::pal : Region::ntsc),
          toast_(toast_icon_directory(mod)) {
        const auto dir = mod.data_dir();
        if (dir.empty()) {
            mod_.warn("Achievement data folder unavailable. Progress applies to this session.");
            return;
        }
        path_ = std::filesystem::u8path(dir) / "profile.txt";
        std::string error;
        if (!load_profile(path_, profile_, error)) {
            mod_.warn("Achievement profile could not be read. Progress applies to this session. " + error);
            path_.clear();
            return;
        }
        summary_path_ = launcher_summary_path(path_.parent_path());
        // An upgrade can have a profile but no viewer data yet. Keep any existing
        // viewer data until a campaign supplies fresh counters.
        std::error_code ec;
        if (!summary_path_.empty() && profile_.unlocked.any()
            && !std::filesystem::exists(summary_path_, ec) && !ec) {
            summary_dirty_ = true;
            publish_summary();
        }
    }

    void hooks();
    void flush() {
        if (profile_dirty_) persist();
        if (summary_dirty_) publish_summary();
    }

private:
    openrac_play_status play() const {
        openrac_play_status status{};
        status.size = sizeof(status);
        if (api_.play_status(&status) != OPENRAC_HOOK_OK) status.phase = OPENRAC_PLAY_NONE;
        return status;
    }

    static bool level_active(const openrac_play_status& status) {
        return status.level >= 0 && status.level < static_cast<int>(playing_levels)
            && (status.phase == OPENRAC_PLAY_PLAYING || status.phase == OPENRAC_PLAY_MENU
                || status.phase == OPENRAC_PLAY_SCENE);
    }

    const std::uint8_t* guest(openrac_gaddr address, std::size_t size) const {
        if (!address || size > guest_ram_bytes || address > guest_ram_bytes - size) return nullptr;
        return api_.memory + address;
    }

    std::optional<unsigned> packet_index(int index, openrac_gaddr table) const {
        // D_001A05C0 / D_001A08C0 on PAL, D_001A04C0 / D_001A07C0 on US.
        // The table is a guest address supplied by memcard_RestoreData.
        const auto global = region_ == Region::pal ? 0x001a05c0u : 0x001a04c0u;
        const auto level = region_ == Region::pal ? 0x001a08c0u : 0x001a07c0u;
        if (index == 0 && (table == global || table == api_.relocate_data(global))) return 0;
        if (index >= 0 && index < static_cast<int>(level_packets)
            && (table == level || table == api_.relocate_data(level))) return 1u + index;
        return std::nullopt;
    }

    std::optional<Snapshot> capture() const {
        SaveBytes bytes;
        if (api_.save_capture(bytes.data(), bytes.size()) != bytes.size()) return std::nullopt;
        return decode_save(bytes.data(), bytes.size(), region_);
    }

    bool read_slot(int slot, SaveBytes& bytes) const {
        if (slot < 0 || slot > 4) return false;
        openrac_save_info info{};
        info.size = sizeof(info);
        return api_.save_slot(slot, &info) == 1
            && api_.save_read(slot, bytes.data(), bytes.size()) == bytes.size();
    }

    std::string key(int slot, Fingerprint digest) const {
        return disc_ + ":" + std::to_string(slot) + ":" + std::to_string(digest);
    }

    void publish_summary() {
        summary_dirty_ = false;
        if (summary_path_.empty()) return;
        const auto summary = trophy_summary(profile_, displayed_snapshot_ ? &*displayed_snapshot_ : nullptr, displayed_campaign_);
        if (summary == published_summary_) return;
        std::string error;
        if (write_file_atomically(summary_path_, summary, error)) {
            published_summary_ = summary;
            return;
        }
        mod_.warn("Launcher trophy collection could not be updated. " + error);
        summary_path_.clear();
    }

    void persist() {
        profile_dirty_ = false;
        if (path_.empty()) return;
        std::string error;
        if (!save_profile(path_, profile_, error)) {
            mod_.warn("Achievement profile could not be written. Further progress applies to this session. " + error);
            path_.clear();
            summary_path_.clear();
        }
    }

    void evaluate_snapshot(const Snapshot& snapshot, std::uint64_t credit, bool imported,
                           bool checkpoint_added = false) {
        if (campaign_.empty()) return;
        const CampaignProgress before = profile_.campaigns[campaign_];
        const auto unlocked = evaluate(profile_, campaign_, snapshot, credit);
        profile_dirty_ |= !equal_progress(before, profile_.campaigns.at(campaign_));
        if (!summary_path_.empty()) {
            displayed_snapshot_ = snapshot;
            displayed_campaign_ = campaign_;
            summary_dirty_ = true;
        }
        if (imported || checkpoint_added || !unlocked.empty()) flush();
        if (unlocked.empty()) return;
        if (toast_enabled_ && !imported) {
            for (auto index : unlocked) toast_.enqueue(index);
        }
        if (imported || unlocked.size() > 3) {
            mod_.info(std::string(imported ? "Achievements imported from save: " : "Achievements unlocked: ")
                      + std::to_string(unlocked.size()) + " new, "
                      + std::to_string(profile_.unlocked.count()) + "/35 earned.");
            return;
        }
        for (auto index : unlocked) mod_.info(std::string("Achievement unlocked: ") + catalog()[index].name);
    }

    void restore_packet(openrac_gaddr src, int index, openrac_gaddr table, int errors) {
        // Native memcard_RestoreData returns its error count, including for the
        // file-loading path that calls it without Save_Restore around it.
        const auto packet = packet_index(index, table);
        if (!packet || resetting_) return;
        if (*packet == 0) {
            const auto status = play();
            const bool internal = in_whole_restore_ && !campaign_.empty()
                && status.phase != OPENRAC_PLAY_FRONT_END;
            restore_ = Restore{{}, {}, status.loads, internal};
            invalid_restore_ = false;
            pending_write_.reset();
        }
        if (!restore_) return;
        const auto size = *packet == 0 ? global_bytes : level_bytes;
        const auto* bytes = guest(src, size);
        if (errors != 0 || !bytes) {
            invalid_restore_ = true;
            restore_.reset();
            return;
        }
        restore_->packets[*packet] = hash(bytes, size);
        restore_->received.set(*packet);
    }

    bool settle_restore(const openrac_play_status& status) {
        if (!restore_) return invalid_restore_;
        if (!restore_->received.all() || !level_active(status)) return true;
        if (restore_->internal) {
            const auto snapshot = capture();
            if (!snapshot) return true;
            tracker_.rebase(*snapshot, true);
            evaluate_snapshot(*snapshot, 0, false);
            last_loads_ = status.loads;
            last_level_ = status.level;
            restore_.reset();
            return true;
        }
        if (status.phase != OPENRAC_PLAY_PLAYING || status.loads == restore_->loads) return true;
        const int slot = api_.save_slot_current();
        SaveBytes bytes;
        if (!read_slot(slot, bytes) || !restore_->matches(bytes)) return true;
        const auto snapshot = decode_save(bytes.data(), bytes.size(), region_);
        if (!snapshot) return true;
        const auto checkpoint = key(slot, hash(bytes.data(), bytes.size()));
        const auto map = profile_.checkpoints.emplace(checkpoint, checkpoint);
        campaign_ = map.first->second;
        current_slot_ = slot;
        tracker_ = runtime::BoltTracker{};
        tracker_.rebase(*snapshot, false);
        evaluate_snapshot(*snapshot, 0, true, map.second);
        last_loads_ = status.loads;
        last_level_ = status.level;
        restore_.reset();
        start_.reset();
        return true;
    }

    void begin_fresh(bool plus, int slot) {
        const auto status = play();
        if (plus && campaign_.empty()) {
            SaveBytes bytes;
            if (read_slot(slot, bytes) && decode_save(bytes.data(), bytes.size(), region_)) {
                const auto checkpoint = key(slot, hash(bytes.data(), bytes.size()));
                const auto found = profile_.checkpoints.find(checkpoint);
                campaign_ = found == profile_.checkpoints.end() ? checkpoint : found->second;
                current_slot_ = slot;
            }
        }
        if (!plus || campaign_.empty()) {
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            campaign_ = disc_ + ":new:" + std::to_string(now) + ":" + std::to_string(++new_count_);
            current_slot_ = -1;
        }
        tracker_ = runtime::BoltTracker{};
        start_ = status.loads;
        restore_.reset();
        invalid_restore_ = false;
        pending_write_.reset();
    }

    void prepare_write(openrac_gaddr address, int slot) {
        if (campaign_.empty() || slot < 0 || slot > 4) return;
        const auto* data = guest(address, save_bytes);
        if (!data || !decode_save(data, save_bytes, region_)) return;
        pending_write_ = PendingWrite{slot, hash(data, save_bytes)};
    }

    void prepare_autosave(int slot, const openrac_play_status& status) {
        if (slot != current_slot_ || !level_active(status) || campaign_.empty() || start_) return;
        SaveBytes bytes;
        if (!read_slot(slot, bytes)) return;
        const auto previous = profile_.checkpoints.find(key(slot, hash(bytes.data(), bytes.size())));
        if (previous == profile_.checkpoints.end() || previous->second != campaign_) return;

        // memcard_Save prepares the global packet and the current level packet.
        // The remaining sections come from the known checkpoint already on disk.
        const auto global = region_ == Region::pal ? 0x0014efd0u : 0x0014eed0u;
        const auto level = region_ == Region::pal ? 0x001507d0u : 0x001506d0u;
        const auto* global_packet = guest(api_.relocate_data(global), global_bytes);
        const auto* level_packet = guest(api_.relocate_data(level), level_bytes);
        if (!global_packet || !level_packet) return;
        std::memcpy(bytes.data() + 8, global_packet, global_bytes);
        std::memcpy(bytes.data() + 8 + global_bytes + status.level * level_bytes, level_packet, level_bytes);
        if (decode_save(bytes.data(), bytes.size(), region_)) {
            pending_write_ = PendingWrite{slot, hash(bytes.data(), bytes.size())};
        }
    }

    void confirm_write(bool force = false) {
        if (!pending_write_ || (!force && ++pending_write_->attempts % 15 != 0)) return;
        SaveBytes bytes;
        if (read_slot(pending_write_->slot, bytes) && hash(bytes.data(), bytes.size()) == pending_write_->expected) {
            if (current_slot_ < 0 || current_slot_ == pending_write_->slot) {
                current_slot_ = pending_write_->slot;
                if (profile_.checkpoints.emplace(key(current_slot_, pending_write_->expected), campaign_).second) persist();
            }
            pending_write_.reset();
        } else if (pending_write_->attempts >= 600) {
            pending_write_.reset();
        }
    }

    void frame() {
        const auto status = play();
        if ((profile_dirty_ || summary_dirty_) && ++flush_ticks_ >= 60) {
            flush_ticks_ = 0;
            flush();
        }
        if (status.phase == OPENRAC_PLAY_FRONT_END && last_phase_ != OPENRAC_PLAY_FRONT_END) {
            toast_.clear();
            confirm_write(true);
            flush();
            campaign_.clear();
            current_slot_ = -1;
            pending_write_.reset();
            restore_.reset();
            invalid_restore_ = false;
            start_.reset();
        }
        last_phase_ = status.phase;
        if (toast_enabled_ && preview_bind_ != OPENRAC_BIND_NONE
            && status.phase == OPENRAC_PLAY_PLAYING && level_active(status)
            && mod_.has_input() && api_.input) {
            const auto input = mod_.input();
            if (input.focused() && !input.overlay() && input.pressed(preview_bind_)) {
                const auto& trophies = catalog();
                const auto demo = std::find_if(trophies.begin(), trophies.end(), [](const Trophy& trophy) {
                    return std::string_view(trophy.id) == "vandal";
                });
                if (demo != trophies.end()) {
                    toast_.enqueue(static_cast<std::size_t>(demo - trophies.begin()), true);
                }
            }
        }
        if (settle_restore(status) || !level_active(status) || campaign_.empty()) return;
        if (start_ && (status.phase != OPENRAC_PLAY_PLAYING || status.loads == *start_)) return;
        confirm_write();
        const bool rebase = start_.has_value() || status.loads != last_loads_;
        if (!rebase && !dirty_ && ++ticks_ < 8) return;
        const auto snapshot = capture();
        if (!snapshot) return;
        if (rebase) tracker_.rebase(*snapshot, !start_ && status.level == last_level_);
        evaluate_snapshot(*snapshot, rebase ? 0 : tracker_.observe(*snapshot), false);
        last_loads_ = status.loads;
        last_level_ = status.level;
        start_.reset();
        ticks_ = 0;
        dirty_ = false;
    }

    openrac::Mod& mod_;
    const openrac_api& api_;
    const std::string disc_;
    const Region region_;
    runtime::TrophyToast toast_;
    int preview_bind_ = OPENRAC_BIND_NONE;
    int toast_sound_ = OPENRAC_HOOK_UNKNOWN;
    bool toast_enabled_ = false;
    Profile profile_;
    std::filesystem::path path_;
    std::filesystem::path summary_path_;
    std::optional<Snapshot> displayed_snapshot_;
    std::string displayed_campaign_;
    std::string published_summary_;
    runtime::BoltTracker tracker_;
    std::string campaign_;
    std::optional<Restore> restore_;
    std::optional<PendingWrite> pending_write_;
    std::optional<std::uint32_t> start_;
    std::uint32_t last_loads_ = 0;
    int last_phase_ = OPENRAC_PLAY_NONE;
    int last_level_ = -1;
    int current_slot_ = -1;
    int plus_slot_ = -1;
    unsigned new_count_ = 0;
    unsigned ticks_ = 0;
    unsigned flush_ticks_ = 0;
    bool profile_dirty_ = false;
    bool summary_dirty_ = false;
    bool invalid_restore_ = false;
    bool in_whole_restore_ = false;
    bool resetting_ = false;
    bool dirty_ = true;
};

void Adapter::hooks() {
    if (mod_.has_overlay() && api_.on_overlay) {
        toast_enabled_ = mod_.on_overlay([](void* user, const openrac_overlay* overlay) {
            auto& self = *static_cast<Adapter*>(user);
            if (overlay && self.toast_.draw(*overlay) && self.toast_sound_ >= 0) {
                self.mod_.sound_play(self.toast_sound_);
            }
        }, this);
        if (toast_enabled_ && api_.setting_string) {
            preview_bind_ = openrac::bind_from_name(mod_.setting<std::string>("preview_toast"));
        }
        if (toast_enabled_ && mod_.has_sounds() && api_.sound_load && api_.sound_play
            && (!api_.setting_bool || mod_.setting<bool>("toast_sound"))) {
            toast_sound_ = mod_.sound_load("ui/audio/trophy-chime.wav");
        }
    }
    mod_.hook<openrac::rac1::Save_Restore>([this](auto next, openrac_gaddr buffer) {
        in_whole_restore_ = true;
        next(buffer);
        in_whole_restore_ = false;
    });
    mod_.hook<openrac::rac1::Save_RestoreData>([this](auto next, openrac_gaddr src, int index, openrac_gaddr table) {
        const int errors = next(src, index, table);
        restore_packet(src, index, table, errors);
        return errors;
    });
    mod_.hook<openrac::rac1::Save_NewGame>([this](auto next) {
        if (!resetting_) begin_fresh(false, -1);
        const bool outer_reset = resetting_;
        resetting_ = true;
        next();
        resetting_ = outer_reset;
    });
    mod_.hook<openrac::rac1::Save_NewGamePlus>([this](auto next, int slot) {
        if (!campaign_.empty() && level_active(play())) {
            if (const auto snapshot = capture()) evaluate_snapshot(*snapshot, tracker_.observe(*snapshot), false);
        }
        begin_fresh(true, slot);
        resetting_ = true;
        plus_slot_ = slot;
        next(slot);
        plus_slot_ = -1;
        resetting_ = false;
    });
    mod_.hook<openrac::rac1::Save_Prepare>([this](auto next, int buffer, int slot) {
        next(buffer, slot);
        prepare_write(static_cast<openrac_gaddr>(buffer), slot);
    });
    mod_.hook<openrac::rac1::Save_RequestAutosave>([this](auto next, int request, int flags) {
        const auto status = play();
        const int slot = api_.save_slot_current();
        const int result = next(request, flags);
        if (result) prepare_autosave(slot, status);
        return result;
    });
    mod_.hook<openrac::rac1::Save_Serialize>([this](auto next, openrac_gaddr buffer) {
        next(buffer);
        if (plus_slot_ >= 0) prepare_write(buffer, plus_slot_);
    });
    mod_.hook<openrac::rac1::Moby_CollectBolt>([this](auto next, openrac_gaddr bolt) {
        next(bolt);
        dirty_ = true;
    });
    mod_.on_frame([this] { frame(); });
}

std::unique_ptr<Adapter> adapter;

void load(openrac::Mod& mod) {
    if (mod.game() != "rac1" || (mod.version() != "pal" && mod.version() != "ntsc")) {
        throw openrac::Error("RAC1 HD Achievements supports RAC1 PAL and NTSC-U.");
    }
    const auto& api = mod.api();
    if (!mod.has_api_11() || !mod.has_play() || !api.memory || !api.relocate_data
        || !api.save_capture || !api.save_read || !api.save_slot || !api.save_slot_current) {
        throw openrac::Error("RAC1 HD Achievements requires the OpenRAC 1.11 play and save interfaces.");
    }
    adapter = std::make_unique<Adapter>(mod);
    adapter->hooks();
}
} // namespace

OPENRAC_MOD_CPP("rac1-hd-achievements", "0.5.3", load)

extern "C" OPENRAC_MOD_EXPORT void openrac_mod_unload() {
    if (adapter) adapter->flush();
    adapter.reset();
}
