// lsfg-vk's shader extraction reads Config::activeConf.dll for the path of Lossless.dll. Its
// configuration module (TOML files, environment) isn't used here: VulkanFrameGeneration sets the
// path directly.
#include "config/config.hpp"

namespace Config {
Configuration activeConf{};
}  // namespace Config
