// Child of the actual launcher's play command. No game runtime or headset.
#include "common/environment.h"
#include "common/paths.h"
#include "common/ipc.h"
#include <chrono>
#include <thread>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) != "--game") continue;
    const auto value = [](const char* name) {
      const auto* v = venture::environment::getenv(name);
      return std::string(v ? v : "missing");
    };
    venture::paths::atomic_write(venture::paths::from_utf8(argv[i + 1]) / "launch-options.json",
        venture::Json{{"passthrough", value("VENTURE_PASSTHROUGH")},
                      {"pcAudio", value("VENTURE_PC_AUDIO")},
                      {"resolution", value("VENTURE_RESOLUTION")},
                      {"frameRate", value("VENTURE_FRAME_RATE")},
                      {"mirrorEye", value("VENTURE_MIRROR_EYE")},
                      {"controls", value("VENTURE_CONTROLS")},
                      {"gpuBounds", value("VENTURE_VULKAN_ROBUST_ACCESS")},
                      {"densityMaps", value("VENTURE_VULKAN_QUEST_DENSITY_MAPS")},
                      {"textureCompression", value("VENTURE_TEXTURE_COMPRESSION")},
                      {"textureCompressionThreshold", value("VENTURE_TEXTURE_COMPRESSION_THRESHOLD")}}.dump());
    const auto game = venture::paths::from_utf8(argv[i + 1]);
    if (std::filesystem::exists(game / "probe-readiness")) {
      std::uintptr_t read = 0, write = 0;
      for (int a = 1; a + 1 < argc; ++a) {
        if (std::string_view(argv[a]) == "--credential-read") read = std::stoull(argv[a + 1]);
        if (std::string_view(argv[a]) == "--credential-write") write = std::stoull(argv[a + 1]);
      }
      auto channel = venture::PipeChannel::inherited(read, write);
      channel->call({{"op", "runtime.ready"}, {"ready", false}});
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      bool ready = false;
      while (!std::filesystem::exists(game / "probe-exit") && std::chrono::steady_clock::now() < until) {
        if (!ready && std::filesystem::exists(game / "probe-render")) {
          channel->call({{"op", "runtime.ready"}, {"ready", true}});
          ready = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    return 0;
  }
  return 2;
}
