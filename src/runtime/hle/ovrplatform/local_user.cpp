#include "hle/ovrplatform/local_user.h"
#include "hle/ovrplatform/request_args.h"
#include "common/file_ops.h"
#include "common/json.h"
#include <mutex>

namespace venture::ovrplatform {
namespace {
bool valid(const User& user) {
  return user.id && user.id <= INT64_MAX && !user.alias.empty() &&
         user.alias.size() <= 128 && user.alias.find('\0') == user.alias.npos &&
         !user.display_name.empty() && user.display_name.size() <= 256 &&
         user.display_name.find('\0') == user.display_name.npos;
}
User load_user(const std::filesystem::path& data) {
  const auto directory = data / "local-user";
  FileLock lock(directory / ".lock");
  const auto file = directory / "profile.json";
  if (is_link(file))
    throw std::runtime_error("Venture local user profile must be a regular file.");
  if (std::filesystem::exists(file)) {
    const auto json = parse_json(paths::read(file));
    User user{number(json, "id"), str(json, "alias"), str(json, "displayName")};
    if (number(json, "version") != 1 || str(json, "source") != "local" || !valid(user))
      throw std::runtime_error("Invalid Venture local user profile; existing identity was preserved.");
    return user;
  }
  const auto key = random_account_key();
  // Nonzero positive 49-bit ID, exactly representable by JSON/JavaScript too.
  User user{(UINT64_C(1) << 48) | std::stoull(key, nullptr, 16),
            "Local_" + key, "Local Player " + key.substr(0, 6)};
  paths::atomic_write(file, Json{{"version", 1}, {"source", "local"},
      {"id", user.id}, {"alias", user.alias}, {"displayName", user.display_name}}.dump(2));
  return user;
}
class LocalUser final : public Provider {
  std::filesystem::path data_;
  User user_;
  bool ready_ = false;
  std::mutex mutex_;
  Result read(std::stop_token stop) {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    if (!ready_) return {failure(ErrorCode::uninitialized)};
    return {{}, user_, {}, user_model(user_.id, user_.alias, user_.display_name)};
  }
public:
  explicit LocalUser(std::filesystem::path data) : data_(std::move(data)) {}
  explicit LocalUser(User user) : user_(std::move(user)) {}
  Error prepare() override {
    std::lock_guard lock(mutex_);
    try {
      if (!ready_ && !data_.empty()) user_ = load_user(data_);
      ready_ = valid(user_);
      return ready_ ? Error{} : failure(ErrorCode::invalid_response);
    } catch (...) {
      return failure(ErrorCode::unavailable);
    }
  }
  Result logged_in_user(std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    return read(stop);
  }
  Result entitlement(std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    auto result = read(stop);
    return {result.error}; // Local simulation: no online ownership claim.
  }
  Result access_token(std::stop_token stop) override {
    return {failure(stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::unauthenticated)};
  }
  Result user_proof(std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    if (auto result = read(stop); result.error) return result;
    try {
      // Offline SDK compatibility only; never a proof accepted by Meta.
      return {{}, {}, "venture-local-" + random_account_key() + random_account_key()};
    } catch (...) {
      return {failure(ErrorCode::unavailable)};
    }
  }
  Result service(std::string_view name, const RequestArgs& args, std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    auto result = read(stop);
    if (result.error) return result;
    if (args.error) return {args.error};
    if (name == "ovr_User_Get") {
      if (args.values.size() != 1 || !std::holds_alternative<std::uint64_t>(args.values[0]))
        return {failure(ErrorCode::invalid_argument)};
      if (std::get<std::uint64_t>(args.values[0]) == user_.id) return result;
    }
    return {failure(ErrorCode::unauthenticated)};
  }
};
} // namespace
std::shared_ptr<Provider> local_user_provider(const std::filesystem::path& data) {
  return std::make_shared<LocalUser>(data);
}
std::shared_ptr<Provider> local_user_provider(User user) {
  return std::make_shared<LocalUser>(std::move(user));
}
} // namespace venture::ovrplatform
