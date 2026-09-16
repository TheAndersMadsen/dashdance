# Native Melee port: recompiled guest code + host runtime.
cmake_minimum_required(VERSION 3.15)
find_package(Threads REQUIRED)
add_library(port_build_options INTERFACE)
target_compile_features(port_build_options INTERFACE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(port_build_options INTERFACE -fno-fast-math -ffp-model=strict -ffp-contract=off)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(port_build_options INTERFACE -fno-fast-math -frounding-math -ffp-contract=off)
endif()
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  target_compile_options(port_build_options INTERFACE -mfma)
endif()
add_executable(port_frame_queue_test tests/frame_queue_test.cpp)
target_include_directories(port_frame_queue_test PRIVATE runtime/gx)
target_compile_features(port_frame_queue_test PRIVATE cxx_std_17)
add_test(NAME port_frame_queue COMMAND port_frame_queue_test)
set_tests_properties(port_frame_queue PROPERTIES TIMEOUT 10)
add_executable(port_memory_range_test tests/memory_range_test.cpp)
target_include_directories(port_memory_range_test PRIVATE runtime/host)
target_compile_features(port_memory_range_test PRIVATE cxx_std_17)
add_test(NAME port_memory_ranges COMMAND port_memory_range_test)
add_test(NAME port_dol_validation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/dol_validation_test.py")
add_executable(port_texture_test tests/texture_snapshot_test.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_texture_test PRIVATE runtime/gx)
target_compile_features(port_texture_test PRIVATE cxx_std_17)
add_test(NAME port_texture_snapshots COMMAND port_texture_test)
add_executable(port_ax_ucode_test tests/ax_ucode_test.cpp runtime/hle/ax_ucode.cpp)
target_include_directories(port_ax_ucode_test PRIVATE runtime/hle)
target_compile_features(port_ax_ucode_test PRIVATE cxx_std_17)
add_test(NAME port_ax_ucode COMMAND port_ax_ucode_test)
add_executable(port_vcdiff_test tests/vcdiff_test.cpp runtime/host/vcdiff.cpp)
target_include_directories(port_vcdiff_test PRIVATE runtime/host)
target_compile_features(port_vcdiff_test PRIVATE cxx_std_17)
add_test(NAME port_vcdiff COMMAND port_vcdiff_test)
add_executable(port_subframe_test tests/subframe_test.cpp runtime/gx/subframe.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_shader.cpp runtime/gx/authored_pose.cpp)
target_include_directories(port_subframe_test PRIVATE runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_link_libraries(port_subframe_test PRIVATE native_animation)
target_compile_features(port_subframe_test PRIVATE cxx_std_17)
target_compile_definitions(port_subframe_test PRIVATE NOMINMAX)
add_test(NAME port_subframe COMMAND port_subframe_test)
set(PORT_LOAD_TEST "${CMAKE_CURRENT_BINARY_DIR}/generated/load_test.cpp")
add_custom_command(OUTPUT "${PORT_LOAD_TEST}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_load_test.py" "${PORT_LOAD_TEST}"
  DEPENDS tests/generate_load_test.py recomp/emit.py recomp/gekko.py)
add_executable(port_load_test "${PORT_LOAD_TEST}")
target_include_directories(port_load_test PRIVATE runtime/ppc)
target_compile_features(port_load_test PRIVATE cxx_std_17)
add_test(NAME port_signed_halfword_loads COMMAND port_load_test)

add_executable(port_render_observer_test tests/render_observer_test.cpp runtime/gx/render_observer.cpp runtime/gx/gx_texture.cpp runtime/gx/authored_pose.cpp runtime/gx/subframe.cpp runtime/gx/gx_shader.cpp)
target_include_directories(port_render_observer_test PRIVATE runtime/ppc runtime/gx "${PROJECT_SOURCE_DIR}/native")
target_compile_definitions(port_render_observer_test PRIVATE NOMINMAX)
target_link_libraries(port_render_observer_test PRIVATE native_animation)
add_test(NAME port_render_observer COMMAND port_render_observer_test)

add_executable(port_slippi_offline_test tests/slippi_offline_test.cpp
  runtime/hle/slippi_online.cpp runtime/hle/slippi_offline.cpp)
target_include_directories(port_slippi_offline_test PRIVATE runtime/hle)
target_compile_definitions(port_slippi_offline_test PRIVATE MELEE_PORT_OFFLINE=1)
add_test(NAME port_slippi_offline COMMAND port_slippi_offline_test)

add_executable(port_slippi_text_test tests/slippi_text_test.cpp runtime/hle/slippi_text.cpp)
target_include_directories(port_slippi_text_test PRIVATE runtime/hle)
if(NOT WIN32)
  find_package(Iconv REQUIRED)
  target_link_libraries(port_slippi_text_test PRIVATE Iconv::Iconv)
endif()
add_test(NAME port_slippi_text COMMAND port_slippi_text_test)

add_executable(port_portable_file_test tests/portable_file_test.cpp)
target_include_directories(port_portable_file_test PRIVATE runtime/host)
add_test(NAME port_portable_files COMMAND port_portable_file_test)

add_executable(port_input_script_test tests/input_script_test.cpp runtime/host/input_script.cpp)
target_include_directories(port_input_script_test PRIVATE runtime/host)
add_test(NAME port_input_script COMMAND port_input_script_test)

add_executable(port_ppc_numeric_test tests/ppc_numeric_test.cpp runtime/ppc/numeric.cpp)
target_include_directories(port_ppc_numeric_test PRIVATE runtime/ppc)
add_test(NAME port_ppc_numeric COMMAND port_ppc_numeric_test)
add_executable(port_ppc_dispatch_test tests/ppc_dispatch_test.cpp
  runtime/ppc/ppc_runtime.cpp runtime/ppc/interp.cpp runtime/ppc/numeric.cpp)
target_include_directories(port_ppc_dispatch_test PRIVATE tests/fixtures runtime/ppc runtime/host)
add_test(NAME port_ppc_dispatch COMMAND port_ppc_dispatch_test)
if(NOT MSVC)
  set(PORT_GENERATOR_TEST_ARGS --cxx "${CMAKE_CXX_COMPILER}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND PORT_GENERATOR_TEST_ARGS --sanitize)
  endif()
  add_test(NAME port_generator_numeric COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_generator_numeric.py" ${PORT_GENERATOR_TEST_ARGS})
  set_tests_properties(port_generator_numeric PROPERTIES TIMEOUT 60)
endif()

add_executable(port_pad_rumble_test tests/pad_rumble_test.cpp runtime/hle/pad_rumble.cpp)
target_include_directories(port_pad_rumble_test PRIVATE runtime/hle)
add_test(NAME port_pad_rumble COMMAND port_pad_rumble_test)
add_test(NAME port_gecko_absorbed_hooks COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/gecko_absorbed_hooks_test.py")
add_test(NAME port_return_adjust COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/return_adjust_test.py")

add_executable(port_slippi_game_file_test tests/slippi_game_file_test.cpp)
target_include_directories(port_slippi_game_file_test PRIVATE runtime/hle)
add_test(NAME port_slippi_game_files COMMAND port_slippi_game_file_test)
add_executable(port_slippi_record_events_test tests/slippi_record_events_test.cpp)
target_include_directories(port_slippi_record_events_test PRIVATE runtime/hle)
add_test(NAME port_slippi_record_events COMMAND port_slippi_record_events_test)
add_executable(port_slippi_matchmaking_protocol_test tests/slippi_matchmaking_protocol_test.cpp
  runtime/hle/slippi_matchmaking_protocol.cpp)
target_include_directories(port_slippi_matchmaking_protocol_test PRIVATE runtime/hle third_party)
add_test(NAME port_slippi_matchmaking_protocol COMMAND port_slippi_matchmaking_protocol_test)

add_executable(port_input_script_equivalence_test tests/input_script_equivalence_test.cpp runtime/host/input_script.cpp)
target_include_directories(port_input_script_equivalence_test PRIVATE runtime/host)
add_test(NAME port_input_script_equivalence COMMAND port_input_script_equivalence_test
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/vs_match.txt")
add_test(NAME port_offline_input_script_equivalence COMMAND port_input_script_equivalence_test
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/offline_vs_acceptance.txt")
add_test(NAME port_offline_gameplay_acceptance COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/offline_gameplay_acceptance_test.py")
add_test(NAME port_input_verification COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tools/test_port_input_verification.py")
if(CMAKE_GENERATOR STREQUAL "Ninja" AND NOT WIN32)
  add_test(NAME port_cmake_default_generated_dir COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/test_cmake_default_generated_dir.py"
    --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}")
  set_tests_properties(port_cmake_default_generated_dir PROPERTIES TIMEOUT 30)
endif()
add_executable(port_scene_trace_test tests/scene_trace_test.cpp)
target_include_directories(port_scene_trace_test PRIVATE runtime/host)
add_test(NAME port_scene_trace COMMAND port_scene_trace_test)
add_executable(port_loaded_module_digest_test tests/loaded_module_digest_test.cpp)
target_include_directories(port_loaded_module_digest_test PRIVATE runtime/host)
add_test(NAME port_loaded_module_digest COMMAND port_loaded_module_digest_test)

add_executable(port_gx_aurora_bridge_test tests/gx_aurora_bridge_test.cpp
  runtime/gx/gx_aurora_bridge.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_gx_aurora_bridge_test PRIVATE runtime/gx)
target_compile_features(port_gx_aurora_bridge_test PRIVATE cxx_std_20)
add_test(NAME port_gx_aurora_bridge COMMAND port_gx_aurora_bridge_test)
add_executable(port_metal_frontend_test tests/metal_frontend_test.cpp
  runtime/host/metal_frontend.cpp runtime/gx/gx_aurora_bridge.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_metal_frontend_test PRIVATE runtime/gx runtime/host)
target_compile_features(port_metal_frontend_test PRIVATE cxx_std_20)
add_test(NAME port_metal_frontend COMMAND port_metal_frontend_test)

if(APPLE)
  add_executable(port_file_identity_test tests/file_identity_test.cpp runtime/host/file_identity.cpp)
  target_include_directories(port_file_identity_test PRIVATE runtime/host)
  add_test(NAME port_file_identity COMMAND port_file_identity_test)
endif()

add_executable(port_dvd_read_worker_test tests/dvd_read_worker_test.cpp runtime/hle/dvd_read_worker.cpp)
target_include_directories(port_dvd_read_worker_test PRIVATE runtime/hle)
add_test(NAME port_dvd_read_worker COMMAND port_dvd_read_worker_test)
set_tests_properties(port_dvd_read_worker PROPERTIES TIMEOUT 10)

if(NOT WIN32)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fatal_log_test.cpp")
    add_executable(port_fatal_log_test tests/fatal_log_test.cpp runtime/host/host_log.cpp)
    target_include_directories(port_fatal_log_test PRIVATE runtime/host runtime/ppc)
    add_test(NAME port_fatal_log COMMAND port_fatal_log_test)
    set_tests_properties(port_fatal_log PROPERTIES TIMEOUT 15)
  endif()
  add_executable(port_headless_options_test tests/headless_options_test.cpp runtime/host/headless_options.cpp)
  target_include_directories(port_headless_options_test PRIVATE runtime/host runtime/ppc)
  add_test(NAME port_headless_options COMMAND port_headless_options_test)
  add_executable(port_audio_headless_test tests/audio_headless_test.cpp runtime/host/audio_headless.cpp)
  target_include_directories(port_audio_headless_test PRIVATE runtime/host)
  target_link_libraries(port_audio_headless_test PRIVATE port_build_options)
  add_test(NAME port_audio_headless COMMAND port_audio_headless_test)
endif()

if(MELEE_BUILD_EXPERIMENTAL_PORT OR MELEE_BUILD_PORT_TRANSPORT_TESTS OR MELEE_BUILD_PORT_METAL)
  enable_language(C)
  set(ENET_SOURCES third_party/enet/callbacks.c third_party/enet/compress.c
    third_party/enet/host.c third_party/enet/list.c third_party/enet/packet.c
    third_party/enet/peer.c third_party/enet/protocol.c)
  if(WIN32)
    list(APPEND ENET_SOURCES third_party/enet/win32.c)
  else()
    list(APPEND ENET_SOURCES third_party/enet/unix.c)
  endif()
  add_library(port_enet STATIC ${ENET_SOURCES})
  target_include_directories(port_enet PUBLIC third_party/enet/include)
  if(WIN32)
    target_compile_definitions(port_enet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    target_link_libraries(port_enet PUBLIC ws2_32 winmm)
  else()
    # Match ENet 1.3.13's feature probes without directory-wide definitions.
    include(CheckFunctionExists)
    include(CheckStructHasMember)
    include(CheckTypeSize)
    foreach(enet_function fcntl poll gethostbyname_r gethostbyaddr_r inet_pton inet_ntop)
      string(TOUPPER "${enet_function}" enet_feature)
      check_function_exists("${enet_function}" "PORT_ENET_HAS_${enet_feature}")
      if(PORT_ENET_HAS_${enet_feature})
        target_compile_definitions(port_enet PRIVATE "HAS_${enet_feature}=1")
      endif()
    endforeach()
    check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" PORT_ENET_HAS_MSGHDR_FLAGS)
    if(PORT_ENET_HAS_MSGHDR_FLAGS)
      target_compile_definitions(port_enet PRIVATE HAS_MSGHDR_FLAGS=1)
    endif()
    set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
    check_type_size("socklen_t" PORT_ENET_SOCKLEN_T BUILTIN_TYPES_ONLY)
    unset(CMAKE_EXTRA_INCLUDE_FILES)
    if(PORT_ENET_SOCKLEN_T)
      target_compile_definitions(port_enet PRIVATE HAS_SOCKLEN_T=1)
    endif()
  endif()
endif()

if(MELEE_BUILD_PORT_TRANSPORT_TESTS)
  add_executable(port_slippi_loopback_test tests/slippi_loopback_test.cpp
    runtime/hle/slippi_net.cpp runtime/hle/slippi_text.cpp)
  target_include_directories(port_slippi_loopback_test PRIVATE runtime/hle runtime/host runtime/ppc third_party)
  target_link_libraries(port_slippi_loopback_test PRIVATE port_enet port_build_options Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(port_slippi_loopback_test PRIVATE Iconv::Iconv)
  endif()
  add_test(NAME port_slippi_loopback COMMAND port_slippi_loopback_test)
  set_tests_properties(port_slippi_loopback PROPERTIES TIMEOUT 30 LABELS "loopback")
endif()

foreach(port_test port_frame_queue_test port_memory_range_test port_texture_test port_ax_ucode_test
                  port_vcdiff_test port_subframe_test port_load_test port_render_observer_test
                  port_slippi_offline_test port_slippi_text_test port_portable_file_test port_input_script_test
                  port_headless_options_test port_ppc_numeric_test port_dvd_read_worker_test
                  port_ppc_dispatch_test port_pad_rumble_test port_slippi_game_file_test
                  port_slippi_record_events_test port_slippi_matchmaking_protocol_test
                  port_input_script_equivalence_test port_gx_aurora_bridge_test port_metal_frontend_test port_file_identity_test
                  port_scene_trace_test port_loaded_module_digest_test port_fatal_log_test)
  if(NOT TARGET ${port_test})
    continue()
  endif()
  target_link_libraries(${port_test} PRIVATE port_build_options Threads::Threads)
  # These assertion-based diagnostics must execute their checks in Release too.
  if(MSVC)
    target_compile_options(${port_test} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${port_test} PRIVATE -UNDEBUG)
  endif()
endforeach()

if(NOT MELEE_BUILD_EXPERIMENTAL_PORT AND NOT MELEE_BUILD_PORT_HEADLESS AND NOT MELEE_BUILD_PORT_METAL)
  return()
endif()

if(MELEE_BUILD_PORT_HEADLESS OR MELEE_BUILD_PORT_METAL)
  set(PORT_GEN_DEFAULT "${PROJECT_BINARY_DIR}/generated/guest")
else()
  set(PORT_GEN_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/generated")
endif()
set(MELEE_PORT_GENERATED_DIR "${PORT_GEN_DEFAULT}" CACHE PATH "Local, untracked generated game C++ directory")
set(PORT_GEN "${MELEE_PORT_GENERATED_DIR}")

if(MELEE_BUILD_PORT_HEADLESS OR MELEE_BUILD_PORT_METAL)
  set(MELEE_PORT_INPUT_MANIFEST "${PROJECT_BINARY_DIR}/port-inputs.json" CACHE FILEPATH
    "Bootstrap provenance manifest for the generated game")
  if(NOT EXISTS "${MELEE_PORT_INPUT_MANIFEST}")
    message(FATAL_ERROR "Missing headless input manifest; generate with tools/bootstrap_port.py")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MELEE_PORT_INPUT_MANIFEST}")
  file(SHA256 "${MELEE_PORT_INPUT_MANIFEST}" MELEE_PORT_INPUT_MANIFEST_SHA256)
  add_custom_target(port_verify_generated
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/verify_port_inputs.py"
            --manifest "${MELEE_PORT_INPUT_MANIFEST}" --source-root "${PROJECT_SOURCE_DIR}"
            --generated-dir "${PORT_GEN}" --decomp-root "${MELEE_DECOMP_ROOT}"
            --fobj "${HOST_FOBJ}"
    COMMENT "Verifying pinned source and generated-game inputs"
    VERBATIM)
endif()
if(NOT EXISTS "${PORT_GEN}/guest_sources.cmake")
  message(FATAL_ERROR "port: generated code missing at ${PORT_GEN}; see tools/bootstrap_port.py and docs/PORTABLE_BUILD.md")
endif()
include("${PORT_GEN}/guest_sources.cmake")

# Guest library: large generated translation units.
add_library(guest STATIC ${GUEST_SOURCES} "${PORT_GEN}/function_names.cpp" "${PORT_GEN}/gecko_data.cpp")
target_include_directories(guest PUBLIC "${PORT_GEN}" runtime/ppc)
target_include_directories(guest PRIVATE runtime/hle)   # gecko_data.h: run-time optional code flags
target_compile_features(guest PUBLIC cxx_std_17)
target_link_libraries(guest PUBLIC port_build_options)
if(MSVC)
  target_compile_options(guest PRIVATE /bigobj /MP /EHsc /arch:AVX2 /fp:precise /wd4102 /wd4101 /wd4189 /W1)
else()
  target_compile_options(guest PRIVATE -Wno-unused-label -Wno-unused-variable)
endif()
target_compile_definitions(guest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MELEE_BUILD_PORT_HEADLESS OR MELEE_BUILD_PORT_METAL)
  add_dependencies(guest port_verify_generated)
  add_dependencies(native_animation port_verify_generated)
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")

# Portable runtime sources shared by the headless and Metal executables. This
# list is intentional: never pull Win32, D3D12, WASAPI, or online transports into
# a portable executable through a source glob.
set(PORT_PORTABLE_RUNTIME_SOURCES
  runtime/ppc/ppc_runtime.cpp runtime/ppc/interp.cpp runtime/ppc/numeric.cpp
  runtime/hle/ax_ucode.cpp runtime/hle/exi_slippi.cpp
  runtime/hle/hle_card.cpp runtime/hle/hle_dvd.cpp runtime/hle/dvd_read_worker.cpp runtime/hle/hle_os.cpp
  runtime/hle/hle_pad.cpp runtime/hle/pad_rumble.cpp runtime/hle/hle_stubs.cpp runtime/hle/jukebox.cpp
  runtime/hle/slippi_net.cpp runtime/hle/slippi_online.cpp runtime/hle/slippi_offline.cpp
  runtime/hle/slippi_playback.cpp runtime/hle/slippi_report.cpp
  runtime/host/host.cpp runtime/host/host_log.cpp runtime/host/vcdiff.cpp runtime/host/headless_options.cpp runtime/host/input_script.cpp
  runtime/host/file_identity.cpp runtime/host/headless_manifest.cpp
  runtime/gx/gx_core.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_shader.cpp
  runtime/gx/authored_pose.cpp runtime/gx/render_observer.cpp runtime/gx/subframe.cpp
  third_party/slippilib/SlippiGame.cpp)

# Artifact identity: invalidate before link, verify after. Shared by every
# generated-game executable.
option(MELEE_ARTIFACT_VERIFY "Verify the linked artifact against the input manifest (off for Xcode-generator device builds, whose per-config paths the step cannot resolve)" ON)
function(port_add_artifact_verification target)
  if(NOT MELEE_ARTIFACT_VERIFY)
    return()
  endif()
  add_custom_command(TARGET ${target} PRE_LINK
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/verify_port_inputs.py"
            --manifest "${MELEE_PORT_INPUT_MANIFEST}" --source-root "${PROJECT_SOURCE_DIR}"
            --generated-dir "${PORT_GEN}" --decomp-root "${MELEE_DECOMP_ROOT}"
            --fobj "${HOST_FOBJ}"
            --binary "$<TARGET_FILE:${target}>" --invalidate
    COMMENT "Invalidating the prior artifact identity before link"
    VERBATIM)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/verify_port_inputs.py"
            --manifest "${MELEE_PORT_INPUT_MANIFEST}" --source-root "${PROJECT_SOURCE_DIR}"
            --generated-dir "${PORT_GEN}" --decomp-root "${MELEE_DECOMP_ROOT}"
            --fobj "${HOST_FOBJ}"
            --binary "$<TARGET_FILE:${target}>"
            --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
    COMMENT "Verifying final artifact against source and generated-game inputs"
    VERBATIM)
endfunction()

if(MELEE_BUILD_PORT_HEADLESS)
  add_library(runtime_headless STATIC ${PORT_PORTABLE_RUNTIME_SOURCES}
    runtime/host/input_headless.cpp runtime/host/window_headless.cpp runtime/host/audio_headless.cpp
    runtime/hle/discord_rpc.cpp runtime/hle/slippi_login.cpp runtime/hle/slippi_http_apple.mm)
  target_include_directories(runtime_headless PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx
    "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" third_party)
  target_compile_definitions(runtime_headless PUBLIC MELEE_PORT_OFFLINE=1)
  target_compile_definitions(runtime_headless PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
  target_compile_definitions(runtime_headless PUBLIC
    MELEE_PORT_INPUT_MANIFEST_PATH="${MELEE_PORT_INPUT_MANIFEST}"
    MELEE_PORT_INPUT_MANIFEST_SHA256="${MELEE_PORT_INPUT_MANIFEST_SHA256}")
  target_link_libraries(runtime_headless PUBLIC native_animation port_build_options Threads::Threads)
  if(APPLE)
    target_link_libraries(runtime_headless PRIVATE "-framework Foundation")
  endif()
  add_dependencies(runtime_headless port_verify_generated)
  add_executable(melee_port_headless app/main_headless.cpp)
  # Repeat the runtime archive for ELF linkers: generated guest code calls HLE
  # implementations, and the host dispatch table also refers to guest code.
  target_link_libraries(melee_port_headless PRIVATE runtime_headless guest)
  if(UNIX AND NOT APPLE)
    target_link_libraries(melee_port_headless PRIVATE runtime_headless)
  endif()
  target_compile_definitions(melee_port_headless PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
  port_add_artifact_verification(melee_port_headless)
  message(STATUS "melee_port_headless is an offline diagnostic target; online and graphics acceptance remain separate")
endif()

if(MELEE_BUILD_PORT_METAL)
  set(MELEE_IOS_BUNDLE_ID "app.dashdance.ios" CACHE STRING "Bundle identifier for the iOS/visionOS app")
  set(MELEE_APPLE_TEAM "" CACHE STRING "Apple developer team id for automatic signing of device builds (Xcode generator)")
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "visionOS" OR CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(MELEE_DEVICE_TARGET ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
      set(MELEE_DEVICE_FAMILY "<integer>7</integer>")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
      set(MELEE_DEVICE_FAMILY "<integer>3</integer>")
    else()
      set(MELEE_DEVICE_FAMILY "<integer>1</integer><integer>2</integer>")
    endif()
    # Icon Composer bundles compile per platform; iOS gets the Liquid Glass icon, the others keep the PNG set.
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(MELEE_APPLE_ACTOOL_PLATFORM iphonesimulator)
      else()
        set(MELEE_APPLE_ACTOOL_PLATFORM iphoneos)
      endif()
    endif()
  else()
    set(MELEE_DEVICE_TARGET OFF)
  endif()
  # Pinned Aurora (encounter/aurora) provides the SDL3 Cocoa window, the Dawn/WebGPU
  # Metal device, and the GX translation that consumes the bridge's normalized commands.
  set(MELEE_AURORA_DIR "${PROJECT_SOURCE_DIR}/build/deps/aurora" CACHE PATH "Pinned Aurora source checkout")
  set(MELEE_AURORA_REVISION "749d6ee7a22bdfab78c8ece9047bca5d79aa72ca")
  if(NOT EXISTS "${MELEE_AURORA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Aurora checkout missing at ${MELEE_AURORA_DIR}; clone encounter/aurora at ${MELEE_AURORA_REVISION}")
  endif()
  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MELEE_AURORA_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE MELEE_AURORA_ACTUAL OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  if(NOT MELEE_AURORA_ACTUAL STREQUAL MELEE_AURORA_REVISION)
    message(FATAL_ERROR "Aurora checkout is ${MELEE_AURORA_ACTUAL}; expected ${MELEE_AURORA_REVISION}")
  endif()
  set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
  set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
  set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
  set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
  if(MELEE_DEVICE_TARGET)
    set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)   # no Homebrew zstd on device targets
    set(AURORA_ENABLE_GX OFF CACHE BOOL "" FORCE)        # only SDL3 is needed; the app renders with Metal directly
  else()
    set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory("${MELEE_AURORA_DIR}" "${PROJECT_BINARY_DIR}/aurora" EXCLUDE_FROM_ALL)
  # Aurora's tests/CMakeLists.txt registers its gtest suites with ctest, but the
  # checkout is added EXCLUDE_FROM_ALL, so the executables never build and ctest
  # lists dead "<suite>_NOT_BUILT" Not Run entries. They unit-test the Aurora
  # io/os/GX layers this port links and all pass on the host platform, so build
  # them with everything else.
  foreach(aurora_test io_tests time_tests os_time_tests gx_fifo_tests
      gx_texture_cache_tests texture_replacement_streaming_tests
      render_worker_tests gfx_recording_tests os_alloc_tests)
    if(TARGET ${aurora_test})
      set_target_properties(${aurora_test} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    endif()
  endforeach()
  foreach(aurora_target aurora_core aurora_gx)
    if(TARGET ${aurora_target})
      target_compile_options(${aurora_target} PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
  endforeach()

  if(NOT MELEE_DEVICE_TARGET)
  # Fallbacks for the Apple platform hooks (host.cpp carries the same ones for
  # non-Apple platforms): the offline diagnostic does not link the dashboard's
  # apple_power.mm, whose controller/network reporting needs window_sdl.cpp --
  # an alternative implementation of the window API window_metal.cpp already
  # provides here.
  set(PORT_METAL_APPLE_FALLBACK "${CMAKE_CURRENT_BINARY_DIR}/apple_platform_fallback_metal.cpp")
  file(WRITE "${PORT_METAL_APPLE_FALLBACK}"
    "// Generated by port/CMakeLists.txt. Fallbacks for the Apple platform hooks\n"
    "// (see host.cpp's non-Apple block): the offline Aurora/Metal diagnostic does\n"
    "// not link the dashboard's apple_power.mm.\n"
    "#include \"host.h\"\n"
    "namespace host {\n"
    "void notify_local(const std::string&, const std::string&) {}\n"
    "void power_play_begin() {}\n"
    "void power_play_end() {}\n"
    "const char* thermal_state_name() { return \"unknown\"; }\n"
    "const char* latency_warning() { return \"\"; }\n"
    "void audio_session_report() {}\n"
    "std::vector<ReadinessItem> competitive_readiness(int, bool, int) { return {}; }\n"
    "}\n")
  add_library(runtime_metal STATIC ${PORT_PORTABLE_RUNTIME_SOURCES}
    runtime/host/input_sdl.cpp runtime/host/audio_sdl.cpp runtime/host/gc_adapter_none.cpp
    runtime/host/window_metal.cpp runtime/host/metal_frontend.cpp runtime/host/metal_frontend_aurora.cpp
    runtime/gx/gx_aurora.cpp runtime/gx/gx_aurora_bridge.cpp
    runtime/hle/discord_rpc.cpp runtime/hle/slippi_login.cpp runtime/hle/slippi_http_apple.mm
    "${PORT_METAL_APPLE_FALLBACK}")
  target_include_directories(runtime_metal PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx
    "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" third_party)
  # gx_aurora.cpp writes the pinned Aurora FIFO directly (lib/gx/fifo.hpp).
  target_include_directories(runtime_metal PRIVATE "${MELEE_AURORA_DIR}/lib")
  target_compile_features(runtime_metal PUBLIC cxx_std_17)
  target_compile_definitions(runtime_metal PUBLIC MELEE_PORT_OFFLINE=1)
  target_compile_definitions(runtime_metal PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
  target_compile_definitions(runtime_metal PUBLIC
    MELEE_PORT_INPUT_MANIFEST_PATH="${MELEE_PORT_INPUT_MANIFEST}"
    MELEE_PORT_INPUT_MANIFEST_SHA256="${MELEE_PORT_INPUT_MANIFEST_SHA256}")
  target_link_libraries(runtime_metal PUBLIC native_animation port_build_options Threads::Threads
    aurora::core aurora::gx aurora::vi aurora::os aurora::mtx aurora::pad absl::flat_hash_map absl::btree)
  # Discord Rich Presence over the local IPC socket: exi_slippi's replay events
  # feed it, the login tables name characters and stages, and the HTTP transport
  # is the same Apple one the headless and macOS application binaries link.
  set_source_files_properties(runtime/hle/slippi_http_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(runtime_metal PUBLIC "-framework Foundation")
  add_dependencies(runtime_metal port_verify_generated)

  # Only the Aurora-facing translation units need C++20; the shared runtime stays
  # on the same standard as the headless and Windows builds.
  set_source_files_properties(runtime/host/metal_frontend_aurora.cpp runtime/gx/gx_aurora.cpp
    runtime/gx/gx_aurora_bridge.cpp runtime/host/metal_frontend.cpp app/main_metal.cpp app/main_mac.cpp
    tests/metal_native_smoke.cpp PROPERTIES COMPILE_OPTIONS "-std=gnu++20")
  add_executable(melee_port_metal app/main_metal.cpp)
  target_link_libraries(melee_port_metal PRIVATE runtime_metal guest)
  target_compile_definitions(melee_port_metal PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
  port_add_artifact_verification(melee_port_metal)

  add_executable(port_metal_native_smoke tests/metal_native_smoke.cpp
    runtime/host/metal_frontend.cpp runtime/host/metal_frontend_aurora.cpp
    runtime/gx/gx_aurora.cpp runtime/gx/gx_aurora_bridge.cpp runtime/gx/gx_texture.cpp)
  target_include_directories(port_metal_native_smoke PRIVATE runtime/gx runtime/host "${MELEE_AURORA_DIR}/lib")
  target_compile_features(port_metal_native_smoke PRIVATE cxx_std_17)
  target_link_libraries(port_metal_native_smoke PRIVATE port_build_options Threads::Threads
    aurora::core aurora::gx aurora::vi aurora::os aurora::mtx absl::flat_hash_map absl::btree)
  message(STATUS "melee_port_metal is an offline Cocoa/Metal/Aurora target; input, audio device and online remain separate")
  endif()
endif()

  # Online-capable macOS application runtime: the same portable sources plus the
  # Slippi text/transport/reporting implementations (ENet, iconv, libcurl).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
  endif()
if(MELEE_BUILD_PORT_METAL)
  if(APPLE AND NOT MELEE_DEVICE_TARGET)
    set(PORT_GC_ADAPTER_SOURCE runtime/host/gc_adapter_iokit.cpp)   # asks the USB host for 1 ms polling; no libusb needed
    message(STATUS "GameCube adapter: IOKit (1000 Hz polling request)")
  elseif(LIBUSB_FOUND AND NOT MELEE_DEVICE_TARGET)
    set(PORT_GC_ADAPTER_SOURCE runtime/host/gc_adapter_libusb.cpp)
    message(STATUS "GameCube adapter: libusb ${LIBUSB_VERSION}")
  else()
    set(PORT_GC_ADAPTER_SOURCE runtime/host/gc_adapter_none.cpp)
    message(STATUS "GameCube adapter: libusb-1.0 not found; WUP-028 support disabled")
  endif()
  if(MELEE_DEVICE_TARGET)
    set(PORT_APPLE_LAUNCHER_SOURCE runtime/host/ios_launcher.mm)
    set(PORT_APPLE_UTI_FRAMEWORK "-framework UniformTypeIdentifiers" "-framework SceneKit" "-framework AVFoundation")
    set(PORT_APPLE_UI_FRAMEWORK "-framework UIKit")
  else()
    set(PORT_APPLE_LAUNCHER_SOURCE runtime/host/mac_launcher.mm)
    set(PORT_APPLE_UTI_FRAMEWORK "-framework SceneKit" "-framework IOKit")
    set(PORT_APPLE_UI_FRAMEWORK "-framework AppKit")
  endif()
  # The application renders with the native Metal backend and talks to SDL3 directly;
  # Aurora is only built here for its vendored SDL3.
  add_library(runtime_mac STATIC ${PORT_PORTABLE_RUNTIME_SOURCES} runtime/hle/slippi_text.cpp
    runtime/hle/slippi_http_apple.mm runtime/hle/slippi_login.cpp runtime/hle/slippi_history.cpp
    runtime/host/window_sdl.cpp runtime/host/input_config.cpp runtime/host/dashboard.cpp runtime/host/audio_sdl.cpp runtime/host/haptics_apple.mm runtime/host/apple_power.mm runtime/host/controller_rate.mm runtime/host/game_menu.cpp runtime/host/notify_apple.mm runtime/hle/discord_rpc.cpp runtime/host/gc_diagram.cpp
    ${PORT_GC_ADAPTER_SOURCE} ${PORT_APPLE_LAUNCHER_SOURCE}
    runtime/gx/gx_metal.mm runtime/gx/gx_msl.cpp)
  set_source_files_properties(runtime/gx/gx_metal.mm runtime/hle/slippi_http_apple.mm runtime/host/haptics_apple.mm runtime/host/apple_power.mm runtime/host/controller_rate.mm runtime/host/notify_apple.mm
    ${PORT_APPLE_LAUNCHER_SOURCE} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_include_directories(runtime_mac PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx
    "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" third_party)
  target_compile_features(runtime_mac PUBLIC cxx_std_17)
  target_compile_definitions(runtime_mac PRIVATE MELEE_SOURCE_ART_DIR="${CMAKE_CURRENT_SOURCE_DIR}/app/art/controller")   # controller artwork for the bare development binary
  target_compile_definitions(runtime_mac PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
  target_compile_definitions(runtime_mac PUBLIC
    MELEE_PORT_INPUT_MANIFEST_PATH="${MELEE_PORT_INPUT_MANIFEST}"
    MELEE_PORT_INPUT_MANIFEST_SHA256="${MELEE_PORT_INPUT_MANIFEST_SHA256}")
  target_link_libraries(runtime_mac PUBLIC native_animation port_build_options Threads::Threads port_enet
    Iconv::Iconv ${PORT_APPLE_UI_FRAMEWORK} "-framework Foundation" "-framework Metal" "-framework QuartzCore" "-framework CoreText" "-framework CoreGraphics" "-framework ImageIO" "-framework Network" "-framework GameController" "-framework UserNotifications" ${PORT_APPLE_UTI_FRAMEWORK}
    ${AURORA_SDL3_TARGET})
  if(LIBUSB_FOUND AND NOT MELEE_DEVICE_TARGET)
    target_link_libraries(runtime_mac PUBLIC PkgConfig::LIBUSB)
  endif()
  add_dependencies(runtime_mac port_verify_generated)

  add_executable(melee_port_mac app/main_mac.cpp)
  if(MELEE_DEVICE_TARGET)
    set_target_properties(melee_port_mac PROPERTIES
      MACOSX_BUNDLE TRUE OUTPUT_NAME "Dashdance"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/ios/Info.plist.in"
      MACOSX_BUNDLE_GUI_IDENTIFIER "${MELEE_IOS_BUNDLE_ID}"
      MACOSX_BUNDLE_BUNDLE_NAME "Dashdance"
      MACOSX_BUNDLE_BUNDLE_VERSION "${MELEE_PORT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${MELEE_PORT_VERSION}"
      XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2,7"
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MELEE_IOS_BUNDLE_ID}")
    if(MELEE_APPLE_TEAM)
      # Device builds through the Xcode generator: automatic signing with the developer's team
      # (a free personal team works; the bundle id is made unique per team by setup.sh).
      set_target_properties(melee_port_mac PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MELEE_APPLE_TEAM}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
        XCODE_ATTRIBUTE_PROVISIONING_PROFILE_SPECIFIER "")
    endif()
    # The Slippi Sys folder ships inside the bundle so the app is self-contained.
    add_custom_command(TARGET melee_port_mac POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/slippi_sys" "$<TARGET_BUNDLE_CONTENT_DIR:melee_port_mac>/slippi_sys"
      COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/AppIcon.icon/Assets/glyph.png" "$<TARGET_BUNDLE_CONTENT_DIR:melee_port_mac>/AppMark.png"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/app/art/controller" "$<TARGET_BUNDLE_CONTENT_DIR:melee_port_mac>/controller"
      VERBATIM)
    if(MELEE_APPLE_ACTOOL_PLATFORM)
      # Liquid Glass app icon: the Icon Composer bundle compiled by actool into Assets.car (+ fallback PNGs).
      add_custom_command(TARGET melee_port_mac POST_BUILD
        COMMAND xcrun actool "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/AppIcon.icon" --compile "$<TARGET_BUNDLE_CONTENT_DIR:melee_port_mac>"
          --output-format human-readable-text --warnings --errors --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/AppIcon-partial.plist"
          --app-icon AppIcon --include-all-app-icons --enable-on-demand-resources NO --development-region en
          --target-device ipad --target-device iphone --minimum-deployment-target 26.0 --platform ${MELEE_APPLE_ACTOOL_PLATFORM}
        VERBATIM)
    else()
      add_custom_command(TARGET melee_port_mac POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/AppIcon60x60@2x.png" "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/AppIcon60x60@3x.png"
          "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/AppIcon76x76@2x.png" "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/AppIcon83.5x83.5@2x.png"
          "$<TARGET_BUNDLE_CONTENT_DIR:melee_port_mac>/"
        VERBATIM)
    endif()
  endif()
  target_link_libraries(melee_port_mac PRIVATE runtime_mac guest)
  target_compile_definitions(melee_port_mac PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
  port_add_artifact_verification(melee_port_mac)
  message(STATUS "melee_port_mac is the online-capable macOS Metal/Aurora application")
endif()

if(MELEE_BUILD_EXPERIMENTAL_PORT)
# Host runtime: CPU helpers, memory, HLE, host services.
set(RUNTIME_SOURCES
  runtime/ppc/ppc_runtime.cpp runtime/ppc/interp.cpp runtime/ppc/numeric.cpp
  runtime/hle/ax_ucode.cpp runtime/hle/exi_slippi.cpp
  runtime/hle/hle_card.cpp runtime/hle/hle_dvd.cpp runtime/hle/dvd_read_worker.cpp runtime/hle/hle_os.cpp runtime/hle/hle_pad.cpp
  runtime/hle/hle_stubs.cpp runtime/hle/pad_rumble.cpp runtime/hle/jukebox.cpp runtime/hle/slippi_net.cpp
  runtime/hle/slippi_online.cpp runtime/hle/slippi_offline.cpp runtime/hle/slippi_playback.cpp runtime/hle/slippi_report.cpp
  runtime/hle/slippi_text.cpp
  runtime/host/audio.cpp runtime/host/gc_adapter.cpp runtime/host/host.cpp runtime/host/host_log.cpp
  runtime/host/updater.cpp runtime/host/vcdiff.cpp runtime/host/window.cpp
  runtime/gx/authored_pose.cpp runtime/gx/gx_core.cpp runtime/gx/gx_d3d12.cpp
  runtime/gx/gx_shader.cpp runtime/gx/gx_streamline.cpp runtime/gx/gx_texture.cpp
  runtime/gx/pc_settings.cpp runtime/gx/render_observer.cpp runtime/gx/subframe.cpp runtime/gx/threaded_backend.cpp)
add_library(runtime STATIC ${RUNTIME_SOURCES} third_party/slippilib/SlippiGame.cpp)
target_compile_definitions(runtime PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
target_include_directories(runtime PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" third_party)
target_compile_features(runtime PUBLIC cxx_std_17)
target_compile_options(runtime PRIVATE /EHsc /arch:AVX2 /fp:precise /W3)
target_compile_definitions(runtime PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
target_link_libraries(runtime PUBLIC native_animation port_build_options port_enet bcrypt d3d12 dxgi d3dcompiler xinput9_1_0 winmm user32 gdi32 setupapi winusb)

add_executable(melee_port app/main.cpp)
add_executable(port_mm_probe tests/mm_probe.cpp)
target_link_libraries(port_mm_probe PRIVATE runtime guest)
target_compile_options(port_mm_probe PRIVATE /EHsc)
target_compile_definitions(port_mm_probe PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(melee_port PRIVATE runtime guest)
target_compile_options(melee_port PRIVATE /EHsc)
target_compile_definitions(melee_port PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")

# Melee Unlocked client: Win32 launcher (Play page, Build tab, self-update). No runtime dependency.
add_executable(melee_unlocked WIN32 app/launcher.cpp runtime/host/updater.cpp)
set_target_properties(melee_unlocked PROPERTIES OUTPUT_NAME MeleeUnlockedLauncher)
target_include_directories(melee_unlocked PRIVATE runtime/host runtime/ppc runtime/hle runtime/gx "${PORT_GEN}" third_party)
target_compile_features(melee_unlocked PRIVATE cxx_std_17)
target_compile_options(melee_unlocked PRIVATE /EHsc)
target_compile_definitions(melee_unlocked PRIVATE NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
target_link_libraries(melee_unlocked PRIVATE comctl32 winhttp user32 gdi32 shell32 comdlg32)

# Replay playback build: the same runtime with the game translated against the Slippi Playback
# code set (python port/recomp/recomp.py --sys-dir port/slippi_sys_playback --out port/generated_playback).
set(PORT_GEN_PLAYBACK "${CMAKE_CURRENT_SOURCE_DIR}/generated_playback")
if(EXISTS "${PORT_GEN_PLAYBACK}/guest_sources.cmake")
  include("${PORT_GEN_PLAYBACK}/guest_sources.cmake")
  add_library(guest_playback STATIC ${GUEST_SOURCES} "${PORT_GEN_PLAYBACK}/function_names.cpp" "${PORT_GEN_PLAYBACK}/gecko_data.cpp")
  target_include_directories(guest_playback BEFORE PUBLIC "${PORT_GEN_PLAYBACK}")
  target_include_directories(guest_playback PUBLIC runtime/ppc)
  target_include_directories(guest_playback PRIVATE runtime/hle)
  target_compile_features(guest_playback PUBLIC cxx_std_17)
  target_compile_options(guest_playback PRIVATE /bigobj /MP /EHsc /arch:AVX2 /fp:precise /wd4102 /wd4101 /wd4189 /W1)
  target_compile_definitions(guest_playback PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  add_executable(melee_port_playback app/main.cpp)
  target_link_libraries(melee_port_playback PRIVATE runtime guest_playback)
  target_compile_options(melee_port_playback PRIVATE /EHsc)
  target_compile_definitions(melee_port_playback PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(EXISTS "${MELEE_DOL_PATH}")
  add_test(NAME port_image_identity COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/image_identity_test.py" "$<TARGET_FILE:melee_port>"
    "${MELEE_DOL_PATH}")
endif()

add_executable(port_gpu_resource_test tests/gpu_resource_test.cpp runtime/gx/gx_d3d12.cpp runtime/gx/gx_shader.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_streamline.cpp)
target_include_directories(port_gpu_resource_test PRIVATE runtime/gx runtime/host runtime/ppc runtime/hle third_party)
target_compile_features(port_gpu_resource_test PRIVATE cxx_std_17)
target_compile_definitions(port_gpu_resource_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS GX_SRV_HEAP_SIZE=32 GX_SAMPLER_HEAP_SIZE=16 GX_CONSTANT_PAGE_SIZE=4096)
target_link_libraries(port_gpu_resource_test PRIVATE d3d12 dxgi d3dcompiler user32)
add_test(NAME port_gpu_resources COMMAND port_gpu_resource_test)
set_tests_properties(port_gpu_resources PROPERTIES TIMEOUT 60)

# Cache identity follows shader generation, constant layout and pipeline setup.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  runtime/gx/gx_shader.cpp runtime/gx/gx_shader.h runtime/gx/gx_d3d12.cpp)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.cpp" GX_SHADER_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_shader.h" GX_LAYOUT_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_d3d12.cpp" GX_PIPELINE_HASH)
string(SHA256 GX_CACHE_HASH "${GX_SHADER_HASH}${GX_LAYOUT_HASH}${GX_PIPELINE_HASH}")
foreach(cache_target runtime port_gpu_resource_test)
  target_compile_definitions(${cache_target} PRIVATE GX_SHADER_CACHE_VERSION="${GX_CACHE_HASH}")
endforeach()

add_library(port_imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx12.cpp third_party/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(port_imgui PUBLIC third_party/imgui)
target_compile_features(port_imgui PUBLIC cxx_std_17)
target_link_libraries(port_imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)
target_compile_definitions(runtime PRIVATE GX_PC_SETTINGS)
target_link_libraries(runtime PUBLIC port_imgui)

# NVIDIA Streamline (DLSS): optional, enabled when the SDK subset is present under third_party/streamline.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline/include/sl.h")
  target_compile_definitions(runtime PRIVATE GX_STREAMLINE)
  target_include_directories(runtime PRIVATE third_party/streamline/include)
  target_link_libraries(runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline/lib/x64/sl.interposer.lib" wintrust crypt32 delayimp)
  target_link_options(melee_port PRIVATE /DELAYLOAD:sl.interposer.dll)
  foreach(sl_dll sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
    add_custom_command(TARGET melee_port POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline/bin/x64/${sl_dll}" "$<TARGET_FILE_DIR:melee_port>/${sl_dll}")
  endforeach()
endif()
endif() # MELEE_BUILD_EXPERIMENTAL_PORT
