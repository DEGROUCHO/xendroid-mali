# xenia-core: the loose .h/.cc files directly under src/xenia/ (emulator,
# kernel_state, etc.) that aren't part of any specific submodule.
add_library(xenia-core STATIC)
file(GLOB _core_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)
target_sources(xenia-core PRIVATE ${_core_sources})
target_link_libraries(xenia-core PUBLIC fmt xenia-base zstd)
xe_target_defaults(xenia-core)

add_subdirectory(base)
add_subdirectory(cpu)
if(XE_TARGET_X86_64)
  add_subdirectory(cpu/backend/x64)
endif()
if(XE_TARGET_AARCH64)
  add_subdirectory(cpu/backend/a64)
endif()
add_subdirectory(apu)
add_subdirectory(apu/nop)
# SDL-based audio is not built on Android (uses AAudio/OpenSLES instead).
if(NOT ANDROID)
  add_subdirectory(apu/sdl)
endif()
add_subdirectory(gpu)
add_subdirectory(gpu/null)
add_subdirectory(gpu/vulkan)
if(APPLE)
  add_subdirectory(gpu/metal)
endif()
# SDL-based helper/HID are not built on Android (uses Android input instead).
if(NOT ANDROID)
  add_subdirectory(helper/sdl)
endif()
add_subdirectory(hid)
add_subdirectory(hid/nop)
if(NOT ANDROID)
  add_subdirectory(hid/sdl)
endif()
add_subdirectory(hid/portal)
add_subdirectory(kernel)
add_subdirectory(patcher)
add_subdirectory(ui)
add_subdirectory(ui/vulkan)
if(APPLE)
  add_subdirectory(ui/metal)
endif()
add_subdirectory(vfs)
add_subdirectory(debug/ui)
add_subdirectory(app/discord)
add_subdirectory(app)

if(WIN32)
  add_subdirectory(apu/xaudio2)
  add_subdirectory(debug/gdb)
  add_subdirectory(gpu/d3d12)
  add_subdirectory(ui/d3d12)
endif()
add_subdirectory(hid/keyboard)

# Save files bind to this exact source/flags build, including changed runtime
# headers. Configure depends forces a fresh fingerprint after source edits.
file(GLOB_RECURSE _snapshot_build_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/../*.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../*.inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../*.inl"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../CMakeLists.txt")
# Resolve redundant /../ segments and symlinks before adding dependency paths.
# CMake/Ninja already tracks loaded CMake files; an alternate spelling of the
# same implicit dependency would create a duplicate generated-file rule.
set(_snapshot_build_canonical_sources)
foreach(_source IN LISTS _snapshot_build_sources)
  file(REAL_PATH "${_source}" _source_real)
  list(APPEND _snapshot_build_canonical_sources "${_source_real}")
endforeach()
list(REMOVE_DUPLICATES _snapshot_build_canonical_sources)
list(SORT _snapshot_build_canonical_sources)
set(_snapshot_build_material "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_BUILD_TYPE};${ANDROID_ABI};${XE_TARGET_AARCH64}")
foreach(_source IN LISTS _snapshot_build_canonical_sources)
  get_filename_component(_source_name "${_source}" NAME)
  get_filename_component(_source_extension "${_source}" LAST_EXT)
  if(NOT _source_name STREQUAL "CMakeLists.txt" AND
     NOT _source_extension STREQUAL ".cmake")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
  endif()
  file(SHA256 "${_source}" _source_hash)
  file(RELATIVE_PATH _source_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
  string(APPEND _snapshot_build_material ";${_source_relative}:${_source_hash}")
endforeach()
string(SHA256 _snapshot_build_id "${_snapshot_build_material}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/snapshot")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/snapshot/build_identity.h"
  "#define XENIA_SNAPSHOT_BUILD_ID \"${_snapshot_build_id}\"\n")
target_include_directories(xenia-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
