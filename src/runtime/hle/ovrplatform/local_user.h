#pragma once
#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
// One persistent local identity per Venture data profile, shared by imports.
// It has no real Meta access token, proof, purchases or network credentials.
std::shared_ptr<Provider> local_user_provider(const std::filesystem::path& data);
// The launcher sends the same identity to its child through its private pipe.
std::shared_ptr<Provider> local_user_provider(User user);
} // namespace venture::ovrplatform
