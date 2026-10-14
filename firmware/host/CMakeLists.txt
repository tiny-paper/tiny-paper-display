# Host (Linux) build of the hardware-free firmware core and its unit tests.
# Used by `make -C firmware test` and by sim/. Conventions (firmware/README.md):
#   components/<name>/src/*.c     -> library tiny_core
#   components/<name>/host.cmake  -> optional, included (extra sources, deps, custom tests)
#   components/<name>/test/test_*.c -> one Unity test executable each (ctest)
#   hal/sim/src/*.c (+ hal/sim/host.cmake) -> library tiny_hal_sim
cmake_minimum_required(VERSION 3.20)
project(tiny_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
option(TINY_SANITIZE "ASan + UBSan for host builds" ON)
# OFF: no Bluetooth config mode in the core (TINY_BLE=0), as every ESP image (tiny/hal.h); ON keeps the BLE tests.
option(TINY_BLE "Bluetooth config mode in the core" ON)
# ON: 32-bit host build (`make -C firmware test32`, needs gcc-multilib): size_t is 32 bits as on the devices, so size
# arithmetic that wraps there wraps here too and the sanitizers see it (issue #56).
# -malign-double: 64-bit members aligned to 8 as on the devices' ABI (the struct size asserts hold).
# Packages: gcc-multilib lib32z1-dev.
# -msse2 -mfpmath=sse: IEEE doubles (no x87 excess precision), so the float vectors match bit for bit.
option(TINY_M32 "32-bit host build (-m32)" OFF)
if(TINY_M32)
  add_compile_options(-m32 -malign-double -msse2 -mfpmath=sse)
  # undefined behaviour ends the test or the fuzz run (the default prints it and goes on); alignment is only
  # printed: -malign-double makes the allocator of this build too coarse a model for it
  add_compile_options(-fno-sanitize-recover=undefined -fsanitize-recover=alignment)
  set(ZLIB_LIBRARY "/usr/lib32/libz.so" CACHE FILEPATH "32-bit zlib (lib32z1-dev)")
  add_link_options(-m32)
endif()
if(TINY_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Disk: the ~140 test executables link the core as shared libraries instead of each carrying a
# static copy, and keep line tables only (-g1, compressed): ASan and UBSan reports still name file and line.
# Size budget: firmware/tools/check_build_size.py. The simulator sets TINY_SHARED OFF (one runner, static).
option(TINY_SHARED "tiny_core and tiny_hal_sim as shared libraries" ON)
if(TINY_SHARED)
  set(TINY_LIB_TYPE SHARED)
  # targets that bring their own HAL (fake_hal.c, fuzzers) cannot link the shared core (it needs the sim HAL to
  # resolve): they take a static copy, which pulls only the objects they use
  set(TINY_CORE_STANDALONE tiny_core_static)
  set(TINY_HAL_STANDALONE tiny_hal_sim_static)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
  set(TINY_LIB_TYPE STATIC)
  set(TINY_CORE_STANDALONE tiny_core)
  set(TINY_HAL_STANDALONE tiny_hal_sim)
endif()
option(TINY_FULL_DEBUG "full debug info (-g), only to compare sizes" OFF)
if(NOT TINY_FULL_DEBUG)
  set(CMAKE_C_FLAGS_DEBUG "-g1 -gz")
endif()

get_filename_component(TINY_FW "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(TINY_ROOT "${TINY_FW}/.." ABSOLUTE)
enable_testing()

include(FetchContent)
FetchContent_Declare(unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG v2.6.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)

# mbedtls for components/btseal (the Bluetooth key: HKDF-SHA256, AES-CCM), the 3.6 line ESP-IDF 5.5 ships; the release
# tarball has the generated files. Only the crypto library is built, without -Werror (not our code).
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
  URL_HASH SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mbedtls)

# Warnings are errors for our code (issue #23); set after Unity so only targets declared below get it.
option(TINY_WERROR "-Werror for our own targets" ON)
if(TINY_WERROR)
  add_compile_options(-Werror)
endif()

add_library(tiny_core ${TINY_LIB_TYPE})
target_include_directories(tiny_core PUBLIC "${TINY_FW}/include")
target_compile_definitions(tiny_core PUBLIC TINY_HOST=1)
# the photo partition code (store.md "Photo partition"; ESP: the PicPak only), on the sim boards with `partitions`
target_compile_definitions(tiny_core PUBLIC TINY_PHOTO_PART=1)
if(TINY_BLE)
  target_compile_definitions(tiny_core PUBLIC TINY_BLE=1)
endif()
target_link_libraries(tiny_core PUBLIC m mbedcrypto)

add_library(tiny_hal_sim ${TINY_LIB_TYPE})
target_link_libraries(tiny_hal_sim PUBLIC tiny_core)

# generated tests of the default loop that swap HAL symbols in the executable (need the static copies)
set(TINY_STATIC_TESTS app_test_config app_test_wifi_wait_input dlog_test_dlog)

file(GLOB TINY_COMPONENTS LIST_DIRECTORIES true "${TINY_FW}/components/*")
foreach(dir ${TINY_COMPONENTS})
  if(NOT IS_DIRECTORY "${dir}")
    continue()
  endif()
  get_filename_component(comp "${dir}" NAME)
  file(GLOB srcs "${dir}/src/*.c")
  target_sources(tiny_core PRIVATE ${srcs})
  set(TINY_CUSTOM_TESTS OFF)
  if(EXISTS "${dir}/host.cmake")
    include("${dir}/host.cmake")
  endif()
  if(NOT TINY_CUSTOM_TESTS)
    file(GLOB tests "${dir}/test/test_*.c")
    foreach(t ${tests})
      get_filename_component(tname "${t}" NAME_WE)
      set(target "${comp}_${tname}")
      add_executable(${target} "${t}")
      if(target IN_LIST TINY_STATIC_TESTS)
        target_link_libraries(${target} PRIVATE ${TINY_CORE_STANDALONE} ${TINY_HAL_STANDALONE} unity)
      else()
        target_link_libraries(${target} PRIVATE tiny_core tiny_hal_sim unity)
      endif()
      target_compile_definitions(${target} PRIVATE TINY_ROOT="${TINY_ROOT}")
      add_test(NAME ${target} COMMAND ${target})
    endforeach()
  endif()
endforeach()

file(GLOB hal_sim_srcs "${TINY_FW}/hal/sim/src/*.c")
target_sources(tiny_hal_sim PRIVATE ${hal_sim_srcs} "${CMAKE_CURRENT_LIST_DIR}/empty.c")
if(EXISTS "${TINY_FW}/hal/sim/host.cmake")
  include("${TINY_FW}/hal/sim/host.cmake")
endif()

if(TINY_SHARED)
  # static copies of the two libraries for the tests that replace HAL symbols, --wrap calls or compile core sources
  # themselves: --wrap and symbol replacement do not reach across shared objects, and ASan flags a global defined
  # twice. Same sources and flags, so what they test is the same code.
  foreach(lib tiny_core tiny_hal_sim)
    add_library(${lib}_static STATIC)
    get_target_property(_srcs ${lib} SOURCES)
    target_sources(${lib}_static PRIVATE ${_srcs})
    foreach(prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS)
      get_target_property(_v ${lib} INTERFACE_${prop})
      if(_v)
        set_property(TARGET ${lib}_static PROPERTY INTERFACE_${prop} ${_v})
        set_property(TARGET ${lib}_static PROPERTY ${prop} ${_v})
      endif()
    endforeach()
  endforeach()
  target_link_libraries(tiny_core_static PUBLIC m mbedcrypto)
  get_target_property(_hl tiny_hal_sim INTERFACE_LINK_LIBRARIES)
  list(TRANSFORM _hl REPLACE "^tiny_core$" "tiny_core_static")
  target_link_libraries(tiny_hal_sim_static PUBLIC ${_hl})
endif()
