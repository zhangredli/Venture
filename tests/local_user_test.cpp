#include <doctest/doctest.h>
#include "common/environment.h"
#include "common/file_ops.h"
#include "hle/ovrplatform/broker.h"
#include "hle/ovrplatform/local_user.h"
#include "hle/ovrplatform/request_args.h"
#include "runtime/ovrplatform_support.h"
#include <future>

using namespace venture::ovrplatform;
namespace {
struct ProfileRoot {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("venture-local-user-" + venture::random_account_key());
  ~ProfileRoot() { venture::remove_tree(root.parent_path(), root); }
};
struct DataEnvironment {
  std::string previous;
  bool present = false;
  explicit DataEnvironment(const std::filesystem::path& root) {
    if (auto value = std::getenv("VENTURE_DATA_DIR")) { previous = value; present = true; }
    set(root.string());
  }
  static void set(const std::string& value) {
#ifdef _WIN32
    _putenv_s("VENTURE_DATA_DIR", value.c_str());
#else
    setenv("VENTURE_DATA_DIR", value.c_str(), 1);
#endif
    venture::environment::refresh();
  }
  ~DataEnvironment() {
    set(previous);
#ifndef _WIN32
    if (!present) { unsetenv("VENTURE_DATA_DIR"); venture::environment::refresh(); }
#endif
  }
};
struct ChannelWorker {
  std::array<std::shared_ptr<venture::PipeChannel>, 2> channels = venture::PipeChannel::pair();
  std::jthread worker;
  explicit ChannelWorker(venture::PipeChannel::Handler handler)
      : worker([this, handler](std::stop_token stop) { channels[0]->serve(handler, stop); }) {}
  ~ChannelWorker() { channels[1]->close(); worker.request_stop(); worker.join(); }
};
} // namespace

TEST_CASE("local user is generated once under concurrent creation and persists") {
  ProfileRoot profile;
  auto load = [&] {
    auto provider = local_user_provider(profile.root);
    if (provider->prepare()) throw std::runtime_error("local profile failed");
    return provider->logged_in_user({}).user;
  };
  auto first = std::async(std::launch::async, load);
  auto second = std::async(std::launch::async, load);
  const auto user = first.get(), other = second.get();
  CHECK(user.id != 0);
  CHECK(user.id < (UINT64_C(1) << 53));
  CHECK_FALSE(user.alias.empty());
  CHECK_FALSE(user.display_name.empty());
  CHECK(other.id == user.id);
  CHECK(other.alias == user.alias);
  CHECK(other.display_name == user.display_name);
  const auto reloaded = load();
  CHECK(reloaded.id == user.id);
  CHECK(reloaded.alias == user.alias);
  CHECK(reloaded.display_name == user.display_name);
  ProfileRoot independent;
  auto separate = local_user_provider(independent.root);
  REQUIRE_FALSE(separate->prepare());
  CHECK(separate->logged_in_user({}).user.id != user.id);
  const auto saved = venture::parse_json(venture::paths::read(profile.root / "local-user/profile.json"));
  CHECK(venture::number(saved, "id") == user.id);
  CHECK(venture::str(saved, "source") == "local");
}

TEST_CASE("local profile corruption is preserved rather than changing save identity") {
  ProfileRoot profile;
  std::string original = "{broken";
  SUBCASE("invalid JSON") {}
  SUBCASE("unsupported version") { original = R"({"version":2,"source":"local","id":42,"alias":"a","displayName":"A"})"; }
  SUBCASE("invalid identity") { original = R"({"version":1,"source":"local","id":0,"alias":"a","displayName":"A"})"; }
  const auto file = profile.root / "local-user/profile.json";
  venture::paths::atomic_write(file, original);
  auto provider = local_user_provider(profile.root);
  CHECK(provider->prepare().code == ErrorCode::unavailable);
  CHECK(provider->logged_in_user({}).error.code == ErrorCode::uninitialized);
  CHECK(provider->entitlement({}).error.code == ErrorCode::uninitialized);
  CHECK(venture::paths::read(file) == original);
}

TEST_CASE("local simulation grants entitlement and exposes only its own identity") {
  ProfileRoot profile;
  auto provider = local_user_provider(profile.root);
  REQUIRE_FALSE(provider->prepare());
  const auto user = provider->logged_in_user({});
  REQUIRE_FALSE(user.error);
  CHECK_FALSE(provider->entitlement({}).error);
  CHECK_FALSE(provider->service("ovr_User_Get", {{user.user.id}}, {}).error);
  CHECK(provider->service("ovr_User_Get", {{user.user.id + 1}}, {}).error.code == ErrorCode::unauthenticated);
  CHECK(provider->service("ovr_User_Get", {}, {}).error.code == ErrorCode::invalid_argument);
  CHECK(provider->access_token({}).error.code == ErrorCode::unauthenticated);
  CHECK(provider->access_token({}).text.empty());
  const auto proof = provider->user_proof({});
  CHECK_FALSE(proof.error);
  CHECK(proof.text.starts_with("venture-local-"));
  CHECK(provider->user_proof({}).text != proof.text);
  CHECK(provider->service("ovr_IAP_GetViewerPurchases", {}, {}).error.code == ErrorCode::unauthenticated);
  std::stop_source cancel;
  cancel.request_stop();
  CHECK(provider->entitlement(cancel.get_token()).error.code == ErrorCode::cancelled);
  CHECK(provider->logged_in_user(cancel.get_token()).error.code == ErrorCode::cancelled);
  CHECK(provider->user_proof(cancel.get_token()).error.code == ErrorCode::cancelled);
}

TEST_CASE("launcher and runtime share the persistent local identity without network credentials") {
  ProfileRoot profile;
  DataEnvironment environment(profile.root);
  auto store = std::make_shared<venture::ovrplatform::test::FakeStore>();
  store->value = {};
  auto http = loopback_http_transport(1); // No server: accidental network calls fail.
  const auto app = venture::ovrplatform::test::app();
  auto broker = std::make_shared<CredentialBroker>(app, "local", store, http);
  REQUIRE_FALSE(broker->prepare());
  auto packet = broker->handle({{"op", "credentials"}});
  REQUIRE(packet.at("localIdentity") == true);
  CHECK(venture::str(packet, "appToken").empty());
  ChannelWorker channel([broker](const venture::Json& request) { return broker->handle(request); });
  auto child = brokered_provider(app, "local", channel.channels[1], http);
  REQUIRE_FALSE(child->prepare());
  auto user = child->logged_in_user({});
  REQUIRE_FALSE(user.error);
  CHECK(user.user.id == venture::number(packet, "appUserId"));
  CHECK(user.user.display_name == venture::str(packet, "displayName"));
  CHECK_FALSE(child->entitlement({}).error);
  CHECK_FALSE(child->service("ovr_User_Get", {{user.user.id}}, {}).error);
  CHECK(child->access_token({}).error.code == ErrorCode::unauthenticated);
  CHECK_FALSE(child->user_proof({}).error);
  CHECK(child->user_proof({}).text.starts_with("venture-local-"));
  CHECK(child->service("ovr_IAP_GetViewerPurchases", {}, {}).error.code == ErrorCode::unauthenticated);
  CHECK(broker->handle({{"op", "refresh"}}).at("appUserId") == user.user.id);
  // Changing an app's declared ID must not change this local user's saves.
  child->declare_app_id("123456789");
  REQUIRE_FALSE(child->prepare());
  CHECK(child->logged_in_user({}).user.id == user.user.id);
  store->replace();
  CHECK(child->entitlement({}).error.code == ErrorCode::identity_mismatch);
  CHECK(child->logged_in_user({}).error.code == ErrorCode::uninitialized);
}

TEST_CASE("local identity packet cannot impersonate a signed-in account") {
  const auto app = venture::ovrplatform::test::app();
  venture::Json packet{{"version", 1}, {"account", "123456abcdef"},
    {"appId", app.app_id}, {"package", app.package}, {"binaryId", app.binary_id},
    {"versionCode", app.version_code}, {"identityError", -20001}, {"appToken", ""},
    {"appUserId", 42}, {"alias", "local"}, {"displayName", "Local"}, {"localIdentity", true}};
  ChannelWorker channel([packet](const venture::Json&) { return packet; });
  auto child = brokered_provider(app, "123456abcdef", channel.channels[1], loopback_http_transport(1));
  CHECK(child->prepare().code == ErrorCode::identity_mismatch);
}
