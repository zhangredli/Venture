#pragma once
#include "common/library/library.h"
#include "common/library/import.h"
#include "common/file_ops.h"
#include <fstream>
#include <chrono>
#include <algorithm>
#include <array>

namespace venture::launcher::control {
using Progress = std::function<void(uint64_t, uint64_t)>;
inline void check_stop(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error("Operation cancelled.");
}
inline void copy_file(const fs::path &, const fs::path &, std::stop_token,
                      uint64_t &, uint64_t, const Progress &);
// Stage every destination on its own volume; publish only after all copies
// finish. Backups survive until the entire transaction has committed.
class Transaction {
  struct Entry { fs::path target, staged, backup; bool existed = false; };
  std::vector<Entry> entries_;
  bool committed_ = false;
  fs::path journal_;
  void record(bool committed) {
    if (journal_.empty()) return;
    Json entries = Json::array();
    for (const auto &e : entries_) entries.push_back({{"target", paths::utf8(e.target)},
      {"staged", paths::utf8(e.staged)}, {"backup", paths::utf8(e.backup)}, {"existed", e.existed}});
    paths::atomic_write(journal_, Json{{"committed", committed}, {"entries", entries}}.dump());
  }
  static void restore(const std::vector<Entry> &entries, bool committed) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if (is_link(it->target) || is_link(it->staged) || is_link(it->backup))
        throw std::runtime_error("Linked transaction directory cannot be recovered.");
      if (!committed) {
        if (fs::exists(it->backup)) {
          remove_tree(it->target.parent_path(), it->target);
          fs::rename(it->backup, it->target);
        } else if (!it->existed && !fs::exists(it->staged)) {
          remove_tree(it->target.parent_path(), it->target);
        } else if (it->existed && !fs::exists(it->target)) {
          throw std::runtime_error("Original installation is missing during transaction recovery.");
        }
      } else remove_tree(it->backup.parent_path(), it->backup);
      remove_tree(it->staged.parent_path(), it->staged);
    }
  }
public:
  explicit Transaction(fs::path journal = {}) : journal_(std::move(journal)) {}
  // The caller holds the profile queue lock and the journal's package lock.
  // A prepared transaction always rolls back; a committed one only cleans up.
  static void recover(const fs::path &journal) {
    const auto value = Json::parse(paths::read(journal));
    std::vector<Entry> entries;
    for (const auto &e : value.at("entries")) {
      Entry entry{paths::from_utf8(e.at("target").get<std::string>()),
                  paths::from_utf8(e.at("staged").get<std::string>()),
                  paths::from_utf8(e.at("backup").get<std::string>()), e.at("existed").get<bool>()};
      const auto prefix = paths::utf8(entry.target.filename()) + ".venture-control-";
      if (!entry.target.is_absolute() || entry.target == entry.target.root_path() ||
          entry.staged.parent_path() != entry.target.parent_path() ||
          !paths::utf8(entry.staged.filename()).starts_with(prefix) ||
          entry.backup != fs::path(entry.staged.native() + paths::from_utf8(".backup").native()))
        throw std::runtime_error("Invalid installation recovery journal.");
      entries.push_back(std::move(entry));
    }
    restore(entries, value.at("committed").get<bool>());
    fs::remove(journal);
  }
  fs::path stage(const fs::path &target, std::stop_token stop = {}, const Progress &progress = [](auto, auto) {}) {
    check_stop(stop);
    if (is_link(target)) throw std::runtime_error("Linked destination directory is not supported.");
    const auto suffix = ".venture-control-" + random_account_key();
    auto &e = entries_.emplace_back(target, fs::path(target.native() + paths::from_utf8(suffix).native()),
                                    fs::path(target.native() + paths::from_utf8(suffix + ".backup").native()), fs::exists(target));
    record(false);
    fs::create_directories(e.staged);
    if (fs::exists(target)) {
      uint64_t done = 0;
      for (auto it = fs::recursive_directory_iterator(target); it != fs::recursive_directory_iterator(); ++it) {
        check_stop(stop);
        const auto &item = *it;
        if (is_link(item.path())) {
          // The runtime reconstructs this cross-volume OBB view on launch.
          // Never traverse or copy its junction target into a save transaction.
          if (item.path().lexically_relative(target).parent_path().generic_string() == "external/Android/obb") {
            it.disable_recursion_pending(); continue;
          }
          throw std::runtime_error("Linked installation data cannot be overwritten.");
        }
        const auto dest = e.staged / item.path().lexically_relative(target);
        if (item.is_directory()) fs::create_directories(dest);
        else if (item.is_regular_file()) {
          fs::create_directories(dest.parent_path());
          control::copy_file(item.path(), dest, stop, done, 0, progress);
        }
      }
    }
    return e.staged;
  }
  void commit(std::stop_token stop) {
    check_stop(stop);
    for (auto &e : entries_) {
      if (fs::exists(e.target)) fs::rename(e.target, e.backup);
      fs::rename(e.staged, e.target);
    }
    record(true);
    committed_ = true;
  }
  ~Transaction() {
    try {
      restore(entries_, committed_);
      if (!journal_.empty()) fs::remove(journal_);
    } catch (...) { /* Keep the durable journal and backups for next startup. */ }
  }
};
inline void copy_file(const fs::path &from, const fs::path &to, std::stop_token stop,
                      uint64_t &done, uint64_t total, const Progress &progress) {
  if (is_link(from)) throw std::runtime_error("Linked source file is not supported.");
  fs::create_directories(to.parent_path());
  std::ifstream input(from, std::ios::binary);
  std::ofstream output(to, std::ios::binary | std::ios::trunc);
  if (!input || !output) throw std::runtime_error("Could not open installation data.");
  std::array<char, 64 << 10> buffer;
  while (input) {
    check_stop(stop);
    input.read(buffer.data(), buffer.size());
    auto n = input.gcount();
    output.write(buffer.data(), n);
    if (!output) throw std::runtime_error("Could not write installation data. Check free disk space.");
    done += uint64_t(n); progress(done, total);
  }
  if (!input.eof()) throw std::runtime_error("Could not read installation data.");
  output.close();
  if (!output) throw std::runtime_error("Could not finish writing installation data.");
}
// Map package-specific data before the generic /sdcard mapping, exactly as
// runtime/core/process_paths.h does. Never copy one package's data to another.
inline size_t copy_data(const fs::path &source, const fs::path &install,
                     const fs::path &user, const std::string &package, meta::Plan &plan,
                     std::stop_token stop, const Progress &progress) {
  uint64_t total = 0, done = 0;
  std::vector<std::pair<fs::path, fs::path>> copies;
  for (const auto &item : fs::recursive_directory_iterator(source)) {
    check_stop(stop);
    if (is_link(item.path())) throw std::runtime_error("Linked patch data is not supported.");
    if (!item.is_regular_file()) continue;
    auto rel = item.path().lexically_relative(source);
    auto text = paths::utf8(rel);
    std::ranges::replace(text, '\\', '/');
    if (auto at = text.find("/sdcard/"); at != std::string::npos) text.erase(0, at + 1);
    else if (auto at = text.find("/Android/"); at != std::string::npos) text.erase(0, at + 1);
    else if (auto at = text.find("/" + package + "/"); at != std::string::npos) text.erase(0, at + 1);
    const bool shared = text.starts_with("sdcard/");
    if (text.starts_with("sdcard/")) text.erase(0, 7);
    const auto obb = "Android/obb/" + package + "/";
    const auto data = "Android/data/" + package + "/";
    fs::path target;
    std::string name;
    if (text.starts_with(obb)) {
      name = text.substr(obb.size());
      if (!paths::safe_name(name)) throw std::runtime_error("Nested OBB data is not supported.");
      target = install / paths::from_utf8(name);
    } else if (text.starts_with(data)) target = user / "external-data" / paths::from_utf8(text.substr(data.size()));
    else if (text.starts_with(package + "/") && text.find('/', package.size() + 1) == std::string::npos) {
      name = text.substr(package.size() + 1); target = install / paths::from_utf8(name);
    } else if (shared && !text.starts_with("Android/")) {
      target = user / "external" / paths::from_utf8(text);
    } else if (text.find('/') == std::string::npos && meta::expansion_name(text, package)) {
      name = text; target = install / paths::from_utf8(name);
    } else continue;
    if (!paths::within(install, target) && !paths::within(user, target))
      throw std::runtime_error("Patch path escapes destination.");
    if (!name.empty()) {
      auto file = std::ranges::find(plan.files, name, &meta::File::name);
      if (file == plan.files.end()) plan.files.push_back({{}, name, {}, meta::expansion_name(name, package) ? "obb" : "asset", item.file_size()});
      else { file->size = item.file_size(); file->sha256.clear(); }
    }
    total += item.file_size(); copies.emplace_back(item.path(), target);
  }
  for (const auto &[from, to] : copies) copy_file(from, to, stop, done, total, progress);
  return copies.size();
}
} // namespace venture::launcher::control
