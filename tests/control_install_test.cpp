#include "launcher/control_install.h"
#include <doctest/doctest.h>

using namespace venture;
namespace {
struct ControlDirectory {
  fs::path path = fs::temp_directory_path() / ("venture-control-test-" + random_account_key());
  ControlDirectory() { fs::create_directories(path); }
  ~ControlDirectory() { try { remove_tree(path.parent_path(), path); } catch (...) {} }
};
}

TEST_CASE("control transaction publishes both application and saves atomically on failure") {
  ControlDirectory d;
  paths::atomic_write(d.path / "app/game.apk", "old apk");
  paths::atomic_write(d.path / "user/external-data/files/save", "old save");
  {
    launcher::control::Transaction tx;
    auto app = tx.stage(d.path / "app");
    auto user = tx.stage(d.path / "user");
    paths::atomic_write(app / "game.apk", "new apk");
    paths::atomic_write(user / "external-data/files/save", "new save");
    fs::remove_all(user); // fail after the first destination has been published
    CHECK_THROWS(tx.commit({}));
  }
  CHECK(paths::read(d.path / "app/game.apk") == "old apk");
  CHECK(paths::read(d.path / "user/external-data/files/save") == "old save");
  {
    launcher::control::Transaction tx;
    auto app = tx.stage(d.path / "app");
    paths::atomic_write(app / "game.apk", "new apk");
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS(tx.commit(stop.get_token()));
  }
  CHECK(paths::read(d.path / "app/game.apk") == "old apk");
}

TEST_CASE("control patches map OBB and application data without replacing another package") {
  ControlDirectory d;
  auto source = d.path / "source";
  paths::atomic_write(source / "sdcard/Android/obb/com.example.game/main.1.com.example.game.obb", "modded obb");
  paths::atomic_write(source / "sdcard/Android/data/com.example.game/files/save", "save");
  paths::atomic_write(source / "sdcard/Android/data/com.other/files/save", "other");
  paths::atomic_write(source / "sdcard/Movies/clip", "video");
  meta::Plan plan;
  launcher::control::Transaction tx;
  auto app = tx.stage(d.path / "app"), user = tx.stage(d.path / "user");
  launcher::control::copy_data(source, app, user, "com.example.game", plan, {}, [](auto, auto) {});
  tx.commit({});
  CHECK(paths::read(d.path / "app/main.1.com.example.game.obb") == "modded obb");
  CHECK(paths::read(d.path / "user/external-data/files/save") == "save");
  CHECK(paths::read(d.path / "user/external/Movies/clip") == "video");
  CHECK_FALSE(fs::exists(d.path / "user/external/Android/data/com.other"));
  REQUIRE(plan.files.size() == 1);
  CHECK(plan.files.front().kind == "obb");
}

TEST_CASE("control staging skips generated OBB junctions and refuses other links") {
  ControlDirectory d;
  fs::create_directories(d.path / "outside");
  paths::atomic_write(d.path / "outside/keep", "untouched");
  fs::create_directories(d.path / "user/external/Android/obb");
  link_directory(d.path / "user/external/Android/obb/com.example.game", d.path / "outside");
  {
    launcher::control::Transaction tx;
    auto user = tx.stage(d.path / "user");
    CHECK_FALSE(fs::exists(user / "external/Android/obb/com.example.game"));
    tx.commit({});
  }
  CHECK(paths::read(d.path / "outside/keep") == "untouched");
  link_directory(d.path / "user/unsafe", d.path / "outside");
  { launcher::control::Transaction tx; CHECK_THROWS(tx.stage(d.path / "user")); }
  CHECK(paths::read(d.path / "outside/keep") == "untouched");
}

TEST_CASE("modified installation size is accepted while missing files still fail") {
  ControlDirectory d;
  meta::Plan plan;
  plan.files = {{{}, "game.apk", {}, "apk", 100}, {{}, "main.obb", {}, "obb", 200}};
  paths::atomic_write(d.path / "game.apk", "modified apk");
  paths::atomic_write(d.path / "main.obb", "modified obb");
  CHECK(library::install_problems(d.path, plan).empty());
  fs::remove(d.path / "main.obb");
  CHECK(library::install_problems(d.path, plan) == std::vector<std::string>{"main.obb is missing"});
}

TEST_CASE("control recovery restores interrupted multi-directory publication") {
  ControlDirectory d;
  const auto app = d.path / "app", user = d.path / "user";
  const auto app_stage = d.path / "app.venture-control-crash", user_stage = d.path / "user.venture-control-crash";
  const auto app_backup = d.path / "app.venture-control-crash.backup";
  const auto user_backup = d.path / "user.venture-control-crash.backup";
  paths::atomic_write(app_backup / "game.apk", "original apk");
  paths::atomic_write(app / "game.apk", "partially published apk");
  paths::atomic_write(user_backup / "save", "original save");
  paths::atomic_write(user_stage / "save", "new save");
  auto journal = d.path / "journal.json";
  auto entries = Json::array();
  for (const auto &v : {std::array{app, app_stage, app_backup}, std::array{user, user_stage, user_backup}})
    entries.push_back({{"target", paths::utf8(v[0])}, {"staged", paths::utf8(v[1])},
                       {"backup", paths::utf8(v[2])}, {"existed", true}});
  paths::atomic_write(journal, Json{{"committed", false}, {"entries", entries}}.dump());
  launcher::control::Transaction::recover(journal);
  CHECK(paths::read(app / "game.apk") == "original apk");
  CHECK(paths::read(user / "save") == "original save");
  CHECK_FALSE(fs::exists(user_stage));
  CHECK_FALSE(fs::exists(journal));
  // A committed journal keeps the new files and only removes backups.
  paths::atomic_write(app_backup / "game.apk", "old apk");
  paths::atomic_write(app / "game.apk", "committed apk");
  paths::atomic_write(journal, Json{{"committed", true}, {"entries", entries}}.dump());
  launcher::control::Transaction::recover(journal);
  CHECK(paths::read(app / "game.apk") == "committed apk");
  CHECK_FALSE(fs::exists(app_backup));
}
