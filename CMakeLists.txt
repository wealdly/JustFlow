cmake_minimum_required(VERSION 3.25)
project(justflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/W3 /EHsc /permissive- /utf-8 /Zc:__cplusplus)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")   # nvsdk_ngx_d.lib is built against the dynamic CRT

# ---- Windows SDK paths (fxc + cppwinrt projection headers ship with the SDK) --------------------
set(WINSDK_VER "10.0.26100.0" CACHE STRING "Windows SDK version")
set(WINSDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
set(FXC "${WINSDK_ROOT}/bin/${WINSDK_VER}/x64/fxc.exe")
set(CPPWINRT_INC "${WINSDK_ROOT}/Include/${WINSDK_VER}/cppwinrt")

# ---- shaders: fxc -> gen/<name>.h with g_cs_<name> byte array ----------------------------------
set(SHADERS swizzle gray downscale expand compose residual compose_residual)
set(SHADER_HEADERS)
foreach(s ${SHADERS})
    set(src ${CMAKE_SOURCE_DIR}/shaders/${s}.hlsl)
    set(out ${CMAKE_BINARY_DIR}/gen/cs_${s}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
        COMMAND "${FXC}" /nologo /T cs_5_0 /E CSMain /O3 /Fh ${out} /Vn g_cs_${s} ${src}
        DEPENDS ${src}
        COMMENT "fxc ${s}.hlsl")
    list(APPEND SHADER_HEADERS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_HEADERS})

# ---- forwarder DLL: the file name MUST contain "nvngx.dll" (runtime caller gate) ---------------
add_library(nvngx_dll_justflow SHARED forwarder/forwarder.cpp)
set_target_properties(nvngx_dll_justflow PROPERTIES OUTPUT_NAME "nvngx.dll_justflow" PREFIX "" SUFFIX ".dll")
target_include_directories(nvngx_dll_justflow PRIVATE third_party/ngx/include)
target_link_libraries(nvngx_dll_justflow PRIVATE d3d12)

# ---- Spike 0: runtime probe (device + forwarder + NR only) --------------------------------------
add_executable(justflow_spike src/spike.cpp src/d3d.cpp src/ngx_nr.cpp)
add_dependencies(justflow_spike nvngx_dll_justflow)
target_include_directories(justflow_spike PRIVATE src third_party/ngx/include)
target_link_directories(justflow_spike PRIVATE third_party/ngx/lib)
target_link_libraries(justflow_spike PRIVATE d3d12 dxgi nvsdk_ngx_d)
add_custom_command(TARGET justflow_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:nvngx_dll_justflow> $<TARGET_FILE_DIR:justflow_spike>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/runtime/nvngx_dlssnr.dll $<TARGET_FILE_DIR:justflow_spike>)

# ---- the app (only once every module exists) ---------------------------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp AND EXISTS ${CMAKE_SOURCE_DIR}/src/compose.cpp)
add_executable(justflow
    src/main.cpp src/d3d.cpp src/capture.cpp src/present.cpp src/ngx_nr.cpp
    src/nvofa.cpp src/compose.cpp src/bench.cpp src/config.cpp src/fg.cpp src/tray.cpp)
add_dependencies(justflow shaders nvngx_dll_justflow)
target_include_directories(justflow PRIVATE src ${CMAKE_BINARY_DIR}/gen third_party/ngx/include third_party/nvofa "${CPPWINRT_INC}")
target_link_directories(justflow PRIVATE third_party/ngx/lib)
target_link_libraries(justflow PRIVATE d3d11 d3d12 dxgi dwmapi user32 gdi32 shell32 comctl32 windowscodecs WindowsApp nvsdk_ngx_d)
target_compile_options(justflow PRIVATE /await:strict)   # C++/WinRT coroutine keywords are not used, but the headers want it on 14.44
set_target_properties(justflow PROPERTIES WIN32_EXECUTABLE ON)    # tray app: no console window; attaches to a parent console when launched from one

add_custom_command(TARGET justflow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:nvngx_dll_justflow> $<TARGET_FILE_DIR:justflow>
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/profiles $<TARGET_FILE_DIR:justflow>/profiles
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/runtime/nvngx_dlssnr.dll ${CMAKE_SOURCE_DIR}/runtime/nvngx_dlssg.dll $<TARGET_FILE_DIR:justflow>)
endif()
