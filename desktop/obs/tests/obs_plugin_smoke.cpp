// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs/obs.h>

#include <cstddef>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "plugin path is required\n";
    return 2;
  }
  if (!obs_startup("en-US", nullptr, nullptr)) {
    std::cerr << "libobs startup unavailable in this display/session; skipping "
                 "loader smoke\n";
    return 77;
  }
  obs_module_t* module = nullptr;
  const int opened = obs_open_module(&module, argv[1], ".");
  if (opened != MODULE_SUCCESS || module == nullptr || !obs_init_module(module)) {
    std::cerr << "OpenLens module did not load: " << opened << '\n';
    obs_shutdown();
    return 4;
  }
  bool found = false;
  const char* id = nullptr;
  for (std::size_t index = 0; obs_enum_input_types(index, &id); ++index) {
    if (id != nullptr && std::string_view(id) == "openlens_phone_camera")
      found = true;
  }
  obs_shutdown();
  if (!found) {
    std::cerr << "OpenLens source type was not registered\n";
    return 5;
  }
  std::cout << "OpenLens native OBS source registered successfully\n";
  return 0;
}
