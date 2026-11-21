// Private stdio RPC endpoint used by the Windows Flutter application. It owns
// Core on this thread; the reader never calls Core and disk tasks return through
// Worker completions. stdout contains protocol messages only.
#include "launcher/core.h"
#include "launcher/control_install.h"
#include "common/android/manifest.h"
#include "common/log.h"
#include "common/environment.h"
#include "common/saves_migration.h"
#include <iostream>
#include <thread>
#include <deque>
#include <algorithm>

namespace venture::launcher {
namespace {
struct Inbox {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> messages;
  bool eof = false;
};
Options control_options(bool direct = false) { Options o; o.cli = true; o.control = !direct; o.launch_requested = direct; o.offline = true; return o; }
library::DisplaySettings checked_display(const Json &value, bool per_game) {
  if (!value.is_object()) throw std::runtime_error("Display settings must be an object.");
  auto parsed = library::display_from_json(value, per_game);
  auto normalized = library::display_to_json(parsed);
  for (const auto &[key, v] : value.items()) {
    if (per_game && (v == "" || (key == "textureCompressionThreshold" && v == 0))) continue;
    if (key == "advanced" && v.is_object()) {
      for (const auto &[id, choice] : v.items())
        if (choice != "" && field(field(normalized, "advanced"), id) != choice)
          throw std::runtime_error("Invalid advanced setting: " + id);
      continue;
    }
    if (!normalized.contains(key) || normalized[key] != v)
      throw std::runtime_error("Invalid display setting: " + key);
  }
  return parsed;
}
void check_global_setting(const std::string &key, const Json &v) {
  auto invalid = [&] { throw std::runtime_error("Invalid setting: " + key); };
  if (key == "gamesDirectory" || key == "runtimeExecutable" || key == "adbExecutable") {
    if (!v.is_string()) invalid();
    auto p = paths::from_utf8(v.get<std::string>());
    if (p.empty() && key != "gamesDirectory") return;
    if (!p.is_absolute()) invalid();
    if (key != "gamesDirectory" && !fs::is_regular_file(p)) invalid();
  } else if (key == "libraryFolders") {
    if (!v.is_array()) invalid();
    for (const auto &p : v) if (!p.is_string() || !paths::from_utf8(p.get<std::string>()).is_absolute()) invalid();
  } else if (key == "textureCompression" || key == "textureCompressionThreshold" || key == "parallelDownloads" || key == "logLevel") {
    if (!v.is_number_integer()) invalid();
    const auto n = v.get<int>();
    if (key == "textureCompression" ? n < 0 || n > 2 : key == "textureCompressionThreshold" ? n < 10 || n > 95 : key == "parallelDownloads" ? n < 1 || n > 4 : n < 0 || n > 4) invalid();
  } else if (key == "gameLanguage") {
    if (!v.is_string()) invalid();
    const auto text = v.get<std::string>();
    if (text.size() > 16 || !std::ranges::all_of(text, [](unsigned char c) { return std::isalnum(c) || c == '-'; })) invalid();
  } else if (key == "passthroughKeyColor") {
    if (!v.is_string() || !environment::parse_color_key(v.get<std::string>())) invalid();
  } else if (key != "controls" && key != "display" && key != "vulkanLayers" && !v.is_boolean()) invalid();
}
class Control final : public Core {
  struct Task {
    std::string id, state = "running", error;
    uint64_t done = 0, total = 0;
    Json result = Json::object();
    std::stop_source stop;
    std::mutex mutex;
  };
  std::map<std::string, std::shared_ptr<Task>> tasks_;
  Worker disk_{[] {}};
  uint64_t sequence_ = 0;
  std::shared_ptr<Inbox> inbox_;
  std::string last_state_;
  void wake() override { if (inbox_) inbox_->cv.notify_all(); }
  void writable() {
    if (!owns_download_queue()) throw std::runtime_error("Another Venture instance is using this library. Close it and reconnect.");
  }
  library::Install installation(const Json &p) {
    const auto id = str(p, "installId");
    for (const auto &i : installs_) if (paths::utf8(i.directory) == id) return i;
    throw std::runtime_error("Installation not found. Refresh the application list.");
  }
  std::shared_ptr<FileLock> lock_game(const library::Install &i) {
    return std::make_shared<FileLock>(data_ / "locks" / ("game-" + i.plan.package + ".lock"), false);
  }
  Json launch_state(const library::Install &i) {
    const auto r = running_.find(i.id());
    const bool active = r != running_.end() && !r->second.exit;
    return {{"running", r != running_.end() && !r->second.exit},
            {"starting", launch_pending_.contains(i.id())},
            {"loading", launch_pending_.contains(i.id()) || (active && r->second.rendered &&
                !r->second.rendered->load(std::memory_order_relaxed))},
            {"failure", r == running_.end() ? "" : r->second.failure}};
  }
  Json library_json() {
    Json out = Json::array();
    for (const auto &i : installs_) {
      const auto *g = game(i.id());
      auto item = launch_state(i);
      if (auto active = preferred_installs_.find(i.id()); active != preferred_installs_.end() && active->second != i.directory) {
        item["running"] = false; item["starting"] = false; item["loading"] = false; item["failure"] = "";
      }
      item.update(Json{{"installId", paths::utf8(i.directory)}, {"id", i.id()},
        {"package", i.plan.package}, {"name", g ? g->name : i.label.empty() ? i.plan.package : i.label},
        {"version", i.plan.version}, {"versionCode", i.plan.version_code}, {"complete", i.complete},
        {"bytes", i.bytes}, {"directory", paths::utf8(i.directory)}, {"pinned", i.plan.pinned}});
      auto art = data_ / "cache/control-art" / paths::from_utf8(i.plan.package + ".img");
      if (!fs::exists(art)) art = data_ / "cache/art" / paths::from_utf8(i.plan.app_id + ".image");
      item["cover"] = fs::exists(art) ? paths::utf8(art) : "";
      out.push_back(std::move(item));
    }
    return out;
  }
  Json settings_json(const Json &p) {
    auto fresh = library::Settings::load(data_);
    if (!p.contains("installId")) return fresh.json();
    const auto i = installation(p);
    const auto display = fresh.game_display.find(i.id());
    const auto controls = fresh.game_controls.find(i.id());
    return {{"display", library::display_to_json(display == fresh.game_display.end() ? library::game_display_defaults() : display->second)},
      {"controls", controls == fresh.game_controls.end() ? Json() : Json::parse(controls::encode(controls->second))},
      {"global", fresh.json()}};
  }
  Json schema() {
    Json profiles = Json::array(), inputs = Json::array(), switches = Json::array();
    for (const auto &q : controls::kQuestInputs) inputs.push_back({{"id", controls::quest_key(q.id)}, {"left", q.left}, {"right", q.right}, {"axes", q.id == controls::Quest::stick}});
    for (const auto &profile : controls::kProfiles) {
      Json host = Json::array();
      for (const auto &input : profile.inputs) host.push_back({{"path", input.path}, {"label", input.label}, {"hands", input.hands}, {"reserved", input.reserved}, {"axes", controls::is_axes(input.path)}});
      profiles.push_back({{"id", profile.path}, {"name", profile.name}, {"inputs", host}});
    }
    for (const auto &a : library::advanced_switches) switches.push_back(a.id);
    return {{"profiles", profiles}, {"questInputs", inputs}, {"advanced", switches},
      {"frameRates", library::frame_rates}, {"resolutionPercents", library::resolution_percents},
      {"controllerDefaults", {{"holdMs", controls::kHoldMs}, {"chordHoldMs", controls::kChordHoldMs}}},
      {"limits", {{"minSide", library::min_custom_side}, {"maxSide", library::max_custom_side}, {"minRate", library::min_custom_rate}, {"maxRate", library::max_custom_rate},
                  {"minCompressionThreshold", library::min_compression_threshold}, {"maxCompressionThreshold", library::max_compression_threshold}}}};
  }
  Json task_json(const std::shared_ptr<Task> &t) {
    std::lock_guard lock(t->mutex);
    return {{"taskId", t->id}, {"state", t->state}, {"done", t->done}, {"total", t->total}, {"error", t->error}, {"result", t->result}};
  }
  using Work = std::function<Json(std::stop_token, const control::Progress &)>;
  Json submit(Work work) {
    for (const auto &[id, t] : tasks_) if (task_json(t)["state"] == "running")
      throw std::runtime_error("Another emulator operation is in progress. Wait or cancel it first.");
    // Retain recent results for task.get without growing every state event forever.
    if (tasks_.size() >= 256) {
      auto oldest = std::ranges::min_element(tasks_, {}, [](const auto &entry) { return std::stoull(entry.first); });
      tasks_.erase(oldest);
    }
    auto task = std::make_shared<Task>(); task->id = std::to_string(++sequence_);
    tasks_[task->id] = task;
    disk_.submit([this, task, work = std::move(work)](std::stop_token shutdown) -> Worker::Completion {
      std::stop_callback cancel(shutdown, [&] { task->stop.request_stop(); });
      try {
        auto result = work(task->stop.get_token(), [task](uint64_t done, uint64_t total) {
          std::lock_guard lock(task->mutex); task->done = done; task->total = total;
        });
        std::lock_guard lock(task->mutex); task->result = std::move(result); task->state = "complete";
      } catch (const std::exception &e) {
        std::lock_guard lock(task->mutex); task->error = e.what(); task->state = task->stop.stop_requested() ? "cancelled" : "failed";
      }
      return [this] { rescan(); };
    });
    return {{"taskId", task->id}};
  }
  Json install(const Json &p, bool patch) {
    writable();
    if (str(p, "path").empty()) throw std::runtime_error("An installation source is required.");
    const auto source = fs::absolute(paths::from_utf8(str(p, "path"))).lexically_normal();
    const auto p_source_root = paths::from_utf8(str(p, "sourceRoot"));
    auto package = str(p, "package");
    std::optional<library::Install> current;
    if (p.contains("installId")) { current = installation(p); package = current->plan.package; }
    if (package.empty() || !paths::safe_package(package)) throw std::runtime_error("Choose an APK and its package before installing.");
    if (!current) {
      for (const auto &i : installs_) if (i.plan.package == package && !i.plan.pinned) {
        if (current) throw std::runtime_error("More than one installation matches. Choose a target installation.");
        current = i;
      }
    }
    if (patch && !current) throw std::runtime_error("Install the base application before applying this patch.");
    const auto app = current ? current->plan.app_id : package;
    const auto root = current ? root_of(*current) : settings_.games;
    auto identity = current ? launch_choice(current->id(), play_key(current->id())).account_key : account_key_;
    if (identity.empty()) identity = "local";
    auto game_lock = std::make_shared<FileLock>(data_ / "locks" / ("game-" + package + ".lock"), false);
    return submit([=, this, game_lock = std::move(game_lock)](std::stop_token stop, const control::Progress &progress) {
      control::Transaction tx(data_ / "control-transactions" / paths::from_utf8(package + ".json"));
      meta::Plan plan;
      fs::path destination;
      std::unique_ptr<import::Source> selected;
      if (!patch) {
        for (auto &s : import::open_local(source, stop)) if (s->manifest.package == package) {
          if (selected) throw std::runtime_error("Multiple APKs match this package. Select a single APK.");
          selected = std::move(s);
        }
        if (!selected) throw std::runtime_error("No APK for the selected package was found.");
        plan = import::local_plan(*selected, app).plan;
        if (current && current->plan.pinned) throw std::runtime_error("Install updates into the application's main installation, not a pinned version.");
        destination = paths::game_dir(root, package, plan.folder());
      } else { plan = current->plan; destination = current->directory; }
      if (!paths::within(root, destination)) throw std::runtime_error("Invalid installation directory.");
      fs::create_directories(destination.parent_path());
      const auto staged = tx.stage(destination, stop, progress);
      if (selected) {
        auto imported = import::local_plan(*selected, app);
        uint64_t done = 0;
        for (const auto &step : imported.steps) {
          control::check_stop(stop);
          if (!paths::safe_name(step.file.name)) throw std::runtime_error("Invalid import filename.");
          std::ofstream output(staged / paths::from_utf8(step.file.name), std::ios::binary | std::ios::trunc);
          if (!output) throw std::runtime_error("Could not create installation file.");
          selected->read(*step.from, [&](std::span<const uint8_t> bytes) {
            control::check_stop(stop);
            output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            if (!output) throw std::runtime_error("Could not write installation file. Check free disk space.");
            done += bytes.size(); progress(done, selected->bytes());
          }, stop);
          output.close(); if (!output) throw std::runtime_error("Could not finish installation file.");
        }
        try {
        const auto presentation = android::read_presentation(staged / paths::from_utf8(plan.files.front().name));
        const auto &bytes = presentation.banner.empty() ? presentation.icon : presentation.banner;
        if (!bytes.empty()) {
          const auto art = data_ / "cache/control-art" / paths::from_utf8(package + ".img");
          paths::atomic_write(art, std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
        }
        } catch (const std::exception &e) { log::warn("control", "Could not extract artwork: {}", e.what()); }
      }
      const auto user = account_data_dir(data_, identity, package, current && current->plan.pinned ? plan.folder() : "");
      const auto staged_user = tx.stage(user, stop, progress);
      auto data_source = p_source_root.empty() ? source : p_source_root;
      if (fs::is_regular_file(data_source)) data_source = data_source.parent_path();
      auto copied = control::copy_data(data_source, staged, staged_user, package, plan, stop, progress);
      if (!selected && !copied) throw std::runtime_error("No patch data for this package was found.");
      paths::atomic_write(staged / "plan.json", plan.json().dump(2));
      tx.commit(stop);
      return Json{{"installId", paths::utf8(destination)}, {"package", package}};
    });
  }
public:
  explicit Control(std::shared_ptr<Inbox> inbox, bool direct = false) : Core(control_options(direct)), inbox_(std::move(inbox)) {
    start(false);
  }
  ~Control() override { for (auto &[id, t] : tasks_) t->stop.request_stop(); disk_.stop(); shutdown(); }
  int direct_launch(const std::string &target) {
    while (scanning_) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    const auto found = std::ranges::find_if(installs_, [&](const auto &i) {
      return target == (i.plan.pinned ? i.plan.package + "@" + i.plan.folder() : i.plan.package);
    });
    if (found == installs_.end()) throw std::runtime_error("The requested application is not installed.");
    const auto id = found->id();
    launch_game(id, play_key(id), {});
    for (;;) {
      tick();
      if (!launch_pending_.contains(id)) {
        const auto running = running_.find(id);
        if (running == running_.end()) throw std::runtime_error(notice_);
        if (running->second.exit) return *running->second.exit;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  Json dispatch(const std::string &method, const Json &p) {
    if (method == "hello") return {{"protocol", 1}, {"version", VENTURE_VERSION}, {"dataDirectory", paths::utf8(data_)}, {"writable", owns_download_queue()}};
    if (method == "library") { rescan(); last_state_.clear(); return library_json(); }
    if (method == "schema") return schema();
    if (method == "settings.get") return settings_json(p);
    if (method == "settings.set" || method == "settings.reset") {
      writable();
      FileLock lock(data_ / "locks/settings.lock");
      auto next = library::Settings::load(data_);
      const bool reset = method == "settings.reset";
      if (p.contains("installId")) {
        const auto i = installation(p);
        auto changes = field(p, "changes");
        if (reset) { next.game_display.erase(i.id()); next.game_controls.erase(i.id()); }
        else {
          if (changes.contains("display")) {
            auto display = library::display_to_json(next.game_display.contains(i.id()) ? next.game_display.at(i.id()) : library::game_display_defaults());
            display.merge_patch(changes["display"]);
            next.game_display[i.id()] = checked_display(display, true);
          }
          if (changes.contains("controls")) {
            if (changes["controls"].is_null()) next.game_controls.erase(i.id());
            else {
              auto config = next.game_controls.contains(i.id()) ? Json::parse(controls::encode(next.game_controls.at(i.id()))) : Json::object();
              config.merge_patch(changes["controls"]);
              next.game_controls[i.id()] = controls::decode(config.dump());
            }
          }
        }
      } else {
        auto value = next.json();
        if (reset) {
          auto defaults = library::Settings::defaults(data_).json();
          for (const auto *key : {"display", "controls", "textureCompression", "textureCompressionThreshold", "keepTranslatedCode", "gameLanguage", "mixedReality", "passthroughColorKey", "passthroughKeyColor", "vulkanLayers", "launchWithConsole", "logLevel"}) {
            if (std::string_view(key) == "controls") value["controls"]["global"] = Json::object();
            else if (defaults.contains(key)) value[key] = defaults[key]; else value.erase(key);
          }
        } else {
          const auto changes = field(p, "changes");
          for (const auto &[key, v] : changes.items()) {
            check_global_setting(key, v);
            if (key == "controls") value["controls"]["global"].merge_patch(v);
            else if (key == "display") value["display"].merge_patch(v);
            else if (key == "display" || key == "gamesDirectory" || key == "runtimeExecutable" || key == "adbExecutable" || key == "libraryFolders" || key == "logLevel" || key == "launchWithConsole" || key == "textureCompression" || key == "textureCompressionThreshold" || key == "parallelDownloads" || key == "keepTranslatedCode" || key == "gameLanguage" || key == "mixedReality" || key == "passthroughColorKey" || key == "passthroughKeyColor" || key == "vulkanLayers" || key == "addGamesToSteam" || key == "steamAddNew" || key == "lookupImportsOnline") value[key] = v;
            else throw std::runtime_error("Unknown or unsupported setting: " + key);
          }
        }
        checked_display(value["display"], false);
        next = library::Settings::from_json(data_, value);
      }
      apply_settings(std::move(next), false);
      return settings_json(p);
    }
    if (method == "install" || method == "patch") return install(p, method == "patch");
    if (method == "scan") {
      const auto picked = paths::from_utf8(str(p, "path"));
      return submit([picked](std::stop_token stop, const control::Progress &) {
        Json out = Json::array();
        std::vector<fs::path> files;
        if (fs::is_directory(picked)) {
          size_t count = 0;
          for (auto it = fs::recursive_directory_iterator(picked); it != fs::recursive_directory_iterator(); ++it) {
            control::check_stop(stop);
            if (++count > 100000) throw std::runtime_error("Too many installation files.");
            if (is_link(it->path())) throw std::runtime_error("Linked installation source is not supported.");
            if (it.depth() > 16) { it.disable_recursion_pending(); continue; }
            auto ext = paths::utf8(it->path().extension());
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (it->is_regular_file() && ext == ".apk") files.push_back(it->path());
          }
        } else files.push_back(picked);
        for (const auto &file : files) {
          for (const auto &s : import::open_local(file, stop)) out.push_back({{"package", s->manifest.package}, {"name", s->manifest.label}, {"version", s->manifest.version_name}, {"path", s->describe()}});
        }
        return out;
      });
    }
    if (method == "task.get" || method == "task.cancel") {
      auto t = tasks_.find(str(p, "taskId"));
      if (t == tasks_.end()) throw std::runtime_error("Task not found.");
      if (method == "task.cancel") t->second->stop.request_stop();
      return task_json(t->second);
    }
    const auto i = installation(p);
    if (method == "directories") {
      auto key = launch_choice(i.id(), play_key(i.id())).account_key;
      return {{"application", paths::utf8(i.directory)}, {"mods", paths::utf8(mods_dir(data_, i.plan.package))},
        {"saves", paths::utf8(account_data_dir(data_, key.empty() ? "local" : key, i.plan.package, i.plan.pinned ? i.plan.folder() : ""))}};
    }
    writable();
    if (method == "play") {
      if (launch_state(i)["running"] == true || launch_state(i)["starting"] == true)
        throw std::runtime_error("This game is already running or starting.");
      preferred_installs_[i.id()] = i.directory;
      launch_game(i.id(), play_key(i.id()), {}); return launch_state(i);
    }
    if (method == "stop") {
      if (auto selected = preferred_installs_.find(i.id());
          selected != preferred_installs_.end() && selected->second != i.directory)
        throw std::runtime_error("This installation is not running. Select the running copy to stop it.");
      if (launch_pending_.contains(i.id())) launch_cancelled_.insert(i.id());
      auto r = running_.find(i.id());
      if (r != running_.end() && !r->second.exit) { r->second.process.request_stop(); r->second.quitting = true; }
      return launch_state(i);
    }
    if (method == "uninstall") {
      auto lock = lock_game(i);
      return submit([i, lock](std::stop_token stop, const control::Progress &) {
        control::check_stop(stop); library::uninstall(i.directory.parent_path().parent_path(), i); return Json{{"uninstalled", true}};
      });
    }
    if (method == "saves.import" || method == "saves.export") {
      if ((running_.contains(i.id()) && !running_.at(i.id()).exit) || launch_pending_.contains(i.id()))
        throw std::runtime_error("Stop the application before transferring saves.");
      preferred_installs_[i.id()] = i.directory;
      if (method == "saves.export") {
        auto work = export_saves(i.id(), str(p, "serial"), paths::from_utf8(str(p, "adbExecutable")));
        return submit([work](std::stop_token stop, const control::Progress &) {
          auto n = work(stop);
          return Json{{"files", n.files}, {"kept", n.kept}, {"removed", n.removed},
                      {"backup", paths::utf8(n.backup)}, {"hasPrivate", n.has_private}};
        });
      }
      auto work = import_saves(i.id(), paths::from_utf8(str(p, "path")), str(p, "serial"),
                               paths::from_utf8(str(p, "adbExecutable")));
      return submit([work](std::stop_token stop, const control::Progress &) {
        auto n = work(stop); return Json{{"files", n.files}, {"backup", paths::utf8(n.backup)}};
      });
    }
    throw std::runtime_error("Unknown control method: " + method);
  }
  void poll() {
    tick(); disk_.drain();
    Json tasks = Json::array(); for (const auto &[id, t] : tasks_) tasks.push_back(task_json(t));
    auto state = Json{{"event", "state"}, {"library", library_json()}, {"tasks", tasks}, {"scanning", scanning_}, {"notice", notice_}, {"warning", warning_}};
    const auto text = state.dump();
    if (text != last_state_) { std::cout << text << '\n' << std::flush; last_state_ = text; }
  }
};
} // namespace
} // namespace venture::launcher

int main(int argc, char **argv) {
  using namespace venture;
  try {
    auto inbox = std::make_shared<launcher::Inbox>();
    if (argc == 3 && std::string_view(argv[1]) == "--launch") {
      if (!paths::safe_launch_target(argv[2])) throw std::runtime_error("Invalid application launch target.");
      launcher::Control core(inbox, true);
      return core.direct_launch(argv[2]);
    }
    std::thread([inbox] {
      std::string line;
      while (std::getline(std::cin, line)) {
        { std::lock_guard lock(inbox->mutex); inbox->messages.push_back(std::move(line)); }
        inbox->cv.notify_all();
      }
      { std::lock_guard lock(inbox->mutex); inbox->eof = true; } inbox->cv.notify_all();
    }).detach();
    launcher::Control core(inbox);
    for (;;) {
      std::deque<std::string> messages;
      { std::unique_lock lock(inbox->mutex);
        inbox->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return inbox->eof || !inbox->messages.empty(); });
        messages.swap(inbox->messages); if (inbox->eof && messages.empty()) break;
      }
      core.poll();
      for (const auto &line : messages) {
        Json response{{"id", nullptr}};
        try {
          if (line.size() > 1024 * 1024) throw std::runtime_error("Request exceeds protocol limit.");
          const auto request = Json::parse(line); response["id"] = request.at("id");
          response["result"] = core.dispatch(str(request, "method"), request.value("params", Json::object()));
        } catch (const std::exception &e) { response["error"] = {{"code", "operation_failed"}, {"message", e.what()}}; }
        std::cout << response.dump() << '\n' << std::flush;
      }
    }
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
