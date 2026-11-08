cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64/libobs)
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64/deps/w32-pthreads)
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64/UI/obs-frontend-api)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(CURL REQUIRED)
find_package(qrcodegencpp CONFIG QUIET)
if(NOT TARGET qrcodegencpp::qrcodegencpp)
  # Linux distributions and the Flatpak SDK provide pkg-config metadata.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(QRCODEGENCPP REQUIRED IMPORTED_TARGET qrcodegencpp)
  add_library(qrcodegencpp::qrcodegencpp ALIAS PkgConfig::QRCODEGENCPP)
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE qrcodegencpp::qrcodegencpp)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  ./src/pch.h
  ./src/obs-multi-rtmp.h
  ./src/obs-multi-rtmp.cpp 
  ./src/edit-widget.h
  ./src/edit-widget.cpp
  ./src/connection-settings.h
  ./src/connection-settings.cpp
  src/rapidapi-quota-widget.cpp
  src/tiktok-safety.cpp
  src/connection-login.cpp
  src/streamlabs-desktop.cpp
  ./src/account-info.h
  ./src/account-info.cpp
  ./src/account-info-widget.h
  ./src/account-info-widget.cpp
  ./src/streamlabs-info.h
  ./src/streamlabs-info.cpp
  ./src/streamlabs-category-widget.h
  ./src/streamlabs-category-widget.cpp
  src/tiktok-game-info.cpp
  src/tiktok-topic-widget.cpp
  src/dual-layout-widget.cpp
  src/layout-scene.cpp
  src/dual-output-session.cpp
  src/primary-canvas.cpp
  src/primary-output.cpp
  src/primary-settings.cpp
  ./src/tiktok-live-session.h
  ./src/tiktok-live-session.cpp
  ./src/push-widget.h
  ./src/push-widget.cpp
  src/shared-scene-view.h
  ./src/json-util.hpp
  ./src/output-config.h
  ./src/output-config.cpp
  ./src/obs-properties-widget.h
  ./src/obs-properties-widget.cpp
  ./src/protocols.h
  ./src/protocols.cpp
  ./src/helpers.h
  ./src/helpers.cpp
  ./src/streamlabs-api.h
  ./src/streamlabs-api.cpp
)

set(TIKTOK_NATIVE_SOURCES
  src/tiktok-native/live_safety.cpp
  src/tiktok-native/rapidapi_quota.cpp
  src/tiktok-native/tiktok_studio_qr.cpp
  src/tiktok-native/domain_routing.cpp
  src/tiktok-native/tiktok_studio_topics.cpp
  src/tiktok-native/tiktok_studio_game_tags.cpp
  src/tiktok-native/live_client.cpp
  src/tiktok-native/mobile_profile.cpp
  src/tiktok-native/mobile_device.cpp
  src/tiktok-native/frame_signing.cpp
  src/tiktok-native/sei_metadata.cpp
  src/tiktok-native/tiktok_request_signer.cpp
  src/tiktok-native/tiktok_studio_device.cpp
  src/tiktok-native/tiktok_studio_session.cpp
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${TIKTOK_NATIVE_SOURCES} src/tiktok-native/localization.cpp)

if(WIN32)
  set(TIKTOK_CRYPTO_LIBRARY bcrypt)
elseif(NOT APPLE)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  set(TIKTOK_CRYPTO_LIBRARY OpenSSL::Crypto)
endif()
if(TIKTOK_CRYPTO_LIBRARY)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${TIKTOK_CRYPTO_LIBRARY})
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  ./dep/nlohmann-json
  .deps/obs-studio-32.2.1/libobs
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

option(ENABLE_CONNECTION_TESTS "Build offline connection settings tests" OFF)
if(ENABLE_CONNECTION_TESTS)
  enable_testing()
  add_executable(connection-settings-test
    src/rapidapi-quota-widget.cpp src/tiktok-native/rapidapi_quota.cpp
    tests/connection-settings-test.cpp
    src/connection-settings.cpp
    src/account-info-widget.cpp
    src/streamlabs-category-widget.cpp
    src/tiktok-topic-widget.cpp
    src/dual-layout-widget.cpp
    src/tiktok-native/tiktok_studio_topics.cpp
    src/output-config.cpp
  )
  target_include_directories(connection-settings-test PRIVATE src dep/nlohmann-json)
  target_link_libraries(connection-settings-test PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
  add_test(NAME connection-settings COMMAND connection-settings-test)
  set_tests_properties(connection-settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")
  add_executable(media-signing-test tests/media-signing-test.cpp
    src/tiktok-native/rapidapi_quota.cpp
    src/tiktok-native/frame_signing.cpp src/tiktok-native/sei_metadata.cpp)
  target_include_directories(media-signing-test PRIVATE src/tiktok-native)
  target_link_libraries(media-signing-test PRIVATE Qt6::Core CURL::libcurl)
  target_compile_definitions(media-signing-test PRIVATE NOMINMAX)
  add_test(NAME media-signing COMMAND media-signing-test)
  add_executable(native-tiktok-test tests/native-tiktok-test.cpp src/account-info.cpp src/streamlabs-info.cpp src/streamlabs-desktop.cpp src/tiktok-game-info.cpp ${TIKTOK_NATIVE_SOURCES})
  target_include_directories(native-tiktok-test PRIVATE src src/tiktok-native dep/nlohmann-json)
  target_compile_definitions(native-tiktok-test PRIVATE NOMINMAX TIKTOK_NATIVE_TESTING)
  target_link_libraries(native-tiktok-test PRIVATE Qt6::Gui qrcodegencpp::qrcodegencpp CURL::libcurl ${TIKTOK_CRYPTO_LIBRARY})
  add_test(NAME native-tiktok COMMAND native-tiktok-test)
  if(WIN32)
    add_executable(qr-login-test tests/qr-login-test.cpp src/streamlabs-desktop.cpp ${TIKTOK_NATIVE_SOURCES})
    target_include_directories(qr-login-test PRIVATE src src/tiktok-native dep/nlohmann-json)
    target_compile_definitions(qr-login-test PRIVATE NOMINMAX TIKTOK_NATIVE_TESTING)
    target_link_libraries(qr-login-test PRIVATE Qt6::Widgets qrcodegencpp::qrcodegencpp CURL::libcurl OBS::libobs ${TIKTOK_CRYPTO_LIBRARY})
    add_test(NAME qr-login COMMAND qr-login-test)
    set_tests_properties(qr-login PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal" TIMEOUT 30)
    add_executable(safety-test tests/safety-test.cpp src/tiktok-safety.cpp ${TIKTOK_NATIVE_SOURCES})
    target_include_directories(safety-test PRIVATE src src/tiktok-native dep/nlohmann-json)
    target_compile_definitions(safety-test PRIVATE NOMINMAX TIKTOK_NATIVE_TESTING)
    target_link_libraries(safety-test PRIVATE Qt6::Widgets qrcodegencpp::qrcodegencpp CURL::libcurl OBS::libobs OBS::obs-frontend-api ${TIKTOK_CRYPTO_LIBRARY})
    add_test(NAME live-safety COMMAND safety-test)
    set_tests_properties(live-safety PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal" TIMEOUT 30)
    # Loaded only by an isolated portable OBS smoke-test installation.
    add_library(primary-frontend-smoke MODULE tests/primary-frontend-smoke.cpp)
    target_link_libraries(primary-frontend-smoke PRIVATE Qt6::Widgets OBS::libobs OBS::obs-frontend-api)
    add_executable(dual-output-test tests/dual-output-test.cpp
      src/tiktok-safety.cpp
      src/rapidapi-quota-widget.cpp
      src/primary-canvas.cpp src/primary-output.cpp src/primary-settings.cpp
      src/dual-output-session.cpp src/dual-layout-widget.cpp src/layout-scene.cpp
      src/edit-widget.cpp src/connection-settings.cpp src/account-info-widget.cpp src/output-config.cpp
      src/streamlabs-category-widget.cpp src/tiktok-topic-widget.cpp src/obs-properties-widget.cpp src/protocols.cpp
      src/tiktok-live-session.cpp ${TIKTOK_NATIVE_SOURCES})
    target_include_directories(dual-output-test PRIVATE src dep/nlohmann-json)
    target_compile_features(dual-output-test PRIVATE cxx_std_20)
    set_target_properties(dual-output-test PROPERTIES AUTOMOC ON)
    target_compile_definitions(dual-output-test PRIVATE NOMINMAX TIKTOK_NATIVE_TESTING)
    target_link_libraries(dual-output-test PRIVATE Qt6::Widgets qrcodegencpp::qrcodegencpp CURL::libcurl OBS::libobs OBS::obs-frontend-api ${TIKTOK_CRYPTO_LIBRARY})
    add_test(NAME dual-output COMMAND dual-output-test "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/libobs/data")
    set_tests_properties(dual-output PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal" TIMEOUT 150)
  endif()
endif()
