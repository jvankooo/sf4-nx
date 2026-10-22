// sf4-nx: the file lookups the native renderer from masseffect-nx expects (its me_packaged.h API), without
// masseffect-nx's installed-NSP mode: everything is next to the executable (sdmc:/switch/sf4-nx/ on the Switch).

#pragma once

#include <filesystem>
#include <string_view>

#include <rex/filesystem.h>

namespace me::packaged {

inline bool Active() { return false; }

inline const char* DataDir() { return ""; }

// A read-only data file shipped with the game (sf4_shaders.mesp, ...): next to the executable.
inline std::filesystem::path DataFile(std::string_view name) {
  return rex::filesystem::GetExecutableFolder() / std::filesystem::path(name);
}

// A file named by a setting: absolute names unchanged, relative ones next to the executable.
inline std::filesystem::path ShippedFile(const std::filesystem::path& name) {
  return name.is_absolute() ? name : rex::filesystem::GetExecutableFolder() / name;
}

}  // namespace me::packaged
