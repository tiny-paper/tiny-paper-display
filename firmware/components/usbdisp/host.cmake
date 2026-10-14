target_include_directories(tiny_core PUBLIC "${dir}/include")
# The frame a PNG becomes, byte for byte (frame_exact, a ctest) and its timing (frame_bench, run by hand): both make
# real deflate streams with zlib and reach the private PNG side (src/usbdisp_priv.h).
find_package(ZLIB REQUIRED)
add_executable(usbdisp_frame_exact "${dir}/test/frame_exact.c")
add_executable(usbdisp_frame_bench "${dir}/test/frame_bench.c")
foreach(t usbdisp_frame_exact usbdisp_frame_bench)
  target_include_directories(${t} PRIVATE "${dir}/src")
  target_link_libraries(${t} PRIVATE tiny_core tiny_hal_sim unity ZLIB::ZLIB)
  target_compile_definitions(${t} PRIVATE TINY_ROOT="${TINY_ROOT}")
endforeach()
add_test(NAME usbdisp_frame_exact COMMAND usbdisp_frame_exact)
