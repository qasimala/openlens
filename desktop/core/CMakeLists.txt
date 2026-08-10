find_package(PkgConfig REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)

add_library(openlens_core
  src/media.cpp
  src/pairing.cpp
  src/protocol.cpp
  src/session.cpp
  src/sinks.cpp
  src/wifi_discovery.cpp
  src/wifi_transport.cpp
)
target_include_directories(openlens_core PUBLIC include)
target_compile_features(openlens_core PUBLIC cxx_std_20)
target_link_libraries(openlens_core PUBLIC PkgConfig::FFMPEG PkgConfig::AVAHI OpenSSL::Crypto OpenSSL::SSL)
set_target_properties(openlens_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
