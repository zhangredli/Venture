// Child of the actual launcher's play command. No game runtime or headset.
#include "common/environment.h"
#include "common/paths.h"
#include "common/ipc.h"
#include <chrono>
#include <thread>
#include <string>
#include <string_view>
#include <cstdio>
#include <sstream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
  // A binary-clean fake headset for the control endpoint's save-import test.
  if (const auto* root = venture::environment::getenv("VENTURE_TEST_HEADSET_ROOT");
      root && argc >= 5 && std::string_view(argv[1]) == "-s") {
    if (std::string_view(argv[2]) != "QUEST-USB") return 3;
    const auto folder = venture::paths::from_utf8(root);
    const std::string_view op = argv[3], command = argv[4];
    if (op == "shell" && command == "pm list packages -3 --show-versioncode") {
      if (!std::filesystem::exists(folder / "missing-app"))
        std::puts("package:com.example.control versionCode:1");
      std::puts("package:com.other.game versionCode:1");
      return 0;
    }
    if (op == "shell" && command.starts_with("echo @com.example.control;")) {
      const auto size = std::filesystem::file_size(folder / "save.dat");
      std::printf("@com.example.control\n1 /data/app/com.example.control/base.apk\n%llu /sdcard/Android/data/com.example.control/files/save\n",
                  static_cast<unsigned long long>(size));
      if (std::filesystem::exists(folder / "old.dat"))
        std::printf("%llu /sdcard/Android/data/com.example.control/files/old\n",
                    static_cast<unsigned long long>(std::filesystem::file_size(folder / "old.dat")));
      return 0;
    }
    if (op == "exec-out" && (command == "cat '/sdcard/Android/data/com.example.control/files/save'" ||
                            command == "cat '/sdcard/Android/data/com.example.control/files/old'")) {
#ifdef _WIN32
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      const auto data = venture::paths::read(folder / (command.ends_with("/old'") ? "old.dat" : "save.dat"));
      return std::fwrite(data.data(), 1, data.size(), stdout) == data.size() ? 0 : 4;
    }
    if (op == "push" && argc == 6) {
      if (std::filesystem::exists(folder / "fail-push")) return 6;
      const std::string_view remote = argv[5];
      if (remote == "/data/local/tmp/venture-saves/com.example.control") {
        std::filesystem::copy(venture::paths::from_utf8(argv[4]), folder / "staged",
                              std::filesystem::copy_options::recursive);
        return 0;
      }
      if (remote == "/data/local/tmp/venture-saves/com.example.control.remove") {
        std::filesystem::copy_file(venture::paths::from_utf8(argv[4]), folder / "removals");
        return 0;
      }
    }
    if (op == "shell" && command.starts_with("rm -rf '/data/local/tmp/venture-saves/com.example.control'")) {
      std::filesystem::remove_all(folder / "staged");
      std::filesystem::remove(folder / "removals");
      return 0;
    }
    if (op == "shell" && command.starts_with("find '/data/local/tmp/venture-saves/com.example.control'")) {
      for (const auto& file : std::filesystem::recursive_directory_iterator(folder / "staged"))
        if (file.is_regular_file())
          std::printf("%llu /data/local/tmp/venture-saves/com.example.control/%s\n",
                      static_cast<unsigned long long>(file.file_size()),
                      venture::paths::utf8(file.path().lexically_relative(folder / "staged")).c_str());
      return 0;
    }
    if (op == "shell" && command.starts_with("am force-stop com.example.control; d=")) {
      venture::paths::atomic_write(folder / "stopped", "true");
      if (std::filesystem::exists(folder / "removals")) {
        const auto removals = venture::paths::read(folder / "removals");
        if (removals.find("/sdcard/Android/data/com.example.control/files/old\n") != std::string::npos)
          std::filesystem::remove(folder / "old.dat");
      }
      std::filesystem::copy_file(folder / "staged/files/save", folder / "save.dat",
                                 std::filesystem::copy_options::overwrite_existing);
      std::filesystem::remove_all(folder / "staged");
      std::filesystem::remove(folder / "removals");
      std::puts("sent");
      return 0;
    }
    return 5;
  }
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
