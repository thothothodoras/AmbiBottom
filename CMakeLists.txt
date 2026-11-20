cmake_minimum_required(VERSION 3.10)
project(ambi_bridge_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()
# IMPORTANT: never let -DNDEBUG sneak in via a Release/RelWithDebInfo build
# type - every test in tests/test_core.cpp is a plain assert(), and NDEBUG
# silently turns every one of them into a no-op (confirmed by inspecting
# the actual compile command: RelWithDebInfo added -DNDEBUG and the tests
# still printed "[ok]"/passed with the assertions doing nothing). Strip it
# unconditionally regardless of build type so "tests passed" always means
# the assertions actually ran.
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL}")

# v0.33: the ceiling of the strip's current limit, fixed at build time (no
# setting can go above it). 900 mA = the largest USB port of the reference TV
# (its label, not measured). Lower it if your supply gives less.
set(AMBI_MAX_BUDGET_MA 900 CACHE STRING "highest current_budget_ma any setting may ask for (mA)")
add_compile_definitions(AMBI_MAX_BUDGET_MA=${AMBI_MAX_BUDGET_MA})

find_package(PkgConfig REQUIRED)
pkg_check_modules(MBEDTLS REQUIRED mbedtls)
pkg_check_modules(CJSON REQUIRED libcjson)

# This system's mbedtls.pc lists only -lmbedtls, but libmbedtls.so itself
# depends on libmbedx509 and libmbedcrypto; with --as-needed linking that
# omission surfaces as "undefined reference to mbedtls_entropy_func" et al.
# Link the full stack explicitly rather than relying on the .pc file.
set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)

find_package(Threads REQUIRED)

# ---- Pure logic core (parser, colour pipeline, settings) ------------------
# No network, no mbedTLS - this is the part that ports byte-for-byte into
# the ESP32/WLED usermod later.
# shared/ambi_core: the code that also runs on the ESP32 (firmware/wled-usermod).
# ambi_pipeline = colour pipeline + HueStream parser + TV-lock rules: C++17
# standard library only; nothing else may be included or linked here.
add_library(ambi_pipeline STATIC
  shared/ambi_core/huestream.cpp
  shared/ambi_core/color_pipeline.cpp
  shared/ambi_core/tv_lock.cpp
  shared/ambi_core/stream_renderer.cpp
)
target_include_directories(ambi_pipeline PUBLIC shared/ambi_core)
target_compile_options(ambi_pipeline PRIVATE -Wpedantic -Werror)

add_library(ambi_core STATIC
  shared/ambi_core/hue_core.cpp
  shared/ambi_core/settings.cpp
  src/pairing_store.cpp
  src/hue_rest.cpp
  src/mdns_ssdp.cpp
  src/monitor_server.cpp
  src/settings_page.cpp
  src/wled_ddp.cpp
  src/identity_store.cpp
  src/discovery.cpp
  shared/ambi_core/strip_stats.cpp  # v0.41: shared with the ESP32 (wear log on the device)
  src/login_store.cpp
  src/color_trace.cpp
)
# v0.28: login_store (monitor login) hashes with mbedcrypto (PBKDF2/SHA-256; also on the ESP32).
target_include_directories(ambi_core PUBLIC include shared/ambi_core ${CJSON_INCLUDE_DIRS} ${MBEDTLS_INCLUDE_DIRS})
target_link_libraries(ambi_core PUBLIC ambi_pipeline ${CJSON_LIBRARIES} mbedcrypto)
target_compile_options(ambi_core PUBLIC ${CJSON_CFLAGS_OTHER})

# ---- mbedTLS-dependent code (DTLS-PSK server + the JointSpace HTTPS/
# Digest client) ------------------------------------------------------------
add_library(ambi_dtls STATIC
  src/dtls_psk_server.cpp
  src/jointspace_client.cpp
  shared/ambi_core/jointspace_fetch.cpp  # v0.37: the JointSpace client, shared with the ESP32
)
target_include_directories(ambi_dtls PUBLIC include shared/ambi_core ${MBEDTLS_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
target_link_libraries(ambi_dtls PUBLIC ${MBEDTLS_LIBRARIES} ${CJSON_LIBRARIES} Threads::Threads)
target_compile_options(ambi_dtls PUBLIC ${MBEDTLS_CFLAGS_OTHER} ${CJSON_CFLAGS_OTHER})

# ---- Unit tests (pure logic only) ------------------------------------------
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE ambi_core)

# ---- Loopback DTLS self-test (client+server in one process) ---------------
add_executable(test_dtls_loopback tests/test_dtls_loopback.cpp)
target_link_libraries(test_dtls_loopback PRIVATE ambi_dtls ambi_core Threads::Threads)

# ---- Hue REST self-test (real HTTP client against the real server) --------
add_executable(test_hue_rest tests/test_hue_rest.cpp)
target_link_libraries(test_hue_rest PRIVATE ambi_core Threads::Threads)

# ---- SSDP self-test (real multicast M-SEARCH against the real responder) --
add_executable(test_ssdp tests/test_ssdp.cpp)
target_link_libraries(test_ssdp PRIVATE ambi_core Threads::Threads)

# ---- WLED DDP output (packet layout + UDP loopback) -------------------------
add_executable(test_wled_ddp tests/test_wled_ddp.cpp)
target_link_libraries(test_wled_ddp PRIVATE ambi_core)

# ---- main program -----------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/src/main.cpp")
  add_executable(ambi_bridge src/main.cpp src/terminal_dashboard.cpp)
  target_link_libraries(ambi_bridge PRIVATE ambi_core ambi_dtls Threads::Threads)
endif()

# Links ambi_pipeline ONLY: the part shared with the ESP32 has no dependencies.
add_executable(test_shared_pipeline tests/test_shared_pipeline.cpp)
target_link_libraries(test_shared_pipeline PRIVATE ambi_pipeline ambi_core ambi_dtls)  # v0.45 (+ v0.48: settings::sanitize): + the JointSpace pairing signature
add_executable(test_hue_core tests/test_hue_core.cpp)
target_link_libraries(test_hue_core PRIVATE ambi_core)
# The ESP32 firmware's DTLS server is portable C++: the same client test runs against it.
add_executable(test_esp_dtls tests/test_dtls_loopback.cpp firmware/wled-usermod/lib/AmbiBottomEsp/src/ambi_dtls.cpp)
target_compile_definitions(test_esp_dtls PRIVATE AMBI_TEST_ESP_DTLS)
target_include_directories(test_esp_dtls PRIVATE firmware/wled-usermod/lib/AmbiBottomEsp/src ${MBEDTLS_INCLUDE_DIRS})
target_link_libraries(test_esp_dtls PRIVATE ambi_pipeline ${MBEDTLS_LIBRARIES} Threads::Threads)
# The ESP32 usermod's application code as a Linux process (tools/esp_app_test.sh).
add_executable(esp_app_sim tests/esp_app_sim.cpp
  firmware/wled-usermod/lib/AmbiBottomEsp/src/ambi_bottom_app.cpp
  firmware/wled-usermod/lib/AmbiBottomEsp/src/ambi_dtls.cpp
  shared/ambi_core/jointspace_fetch.cpp)  # v0.37
target_include_directories(esp_app_sim PRIVATE firmware/wled-usermod/lib/AmbiBottomEsp/src ${MBEDTLS_INCLUDE_DIRS})
target_link_libraries(esp_app_sim PRIVATE ambi_core ${MBEDTLS_LIBRARIES} Threads::Threads)
add_executable(test_settings_validation tests/test_settings_validation.cpp)
target_link_libraries(test_settings_validation PRIVATE ambi_core)
enable_testing()
add_test(NAME settings_validation COMMAND test_settings_validation)
add_test(NAME esp_dtls COMMAND test_esp_dtls)
add_test(NAME hue_core COMMAND test_hue_core)
add_test(NAME shared_pipeline COMMAND test_shared_pipeline)
add_test(NAME core COMMAND test_core)
add_test(NAME dtls_loopback COMMAND test_dtls_loopback)
add_test(NAME hue_rest COMMAND test_hue_rest)
add_test(NAME ssdp COMMAND test_ssdp)
add_test(NAME wled_ddp COMMAND test_wled_ddp)
