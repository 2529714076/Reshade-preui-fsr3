cmake_minimum_required(VERSION 3.20)
project(reshade_preui_fsr3 VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if (MSVC)
    # 静态 CRT，避免目标机缺少 VC 运行库
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W3 /EHsc /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# 依赖（ReShade 头文件 / MinHook）由 scripts/fetch-deps.ps1 下载到 third_party/。
# 如果本地已经有 third_party/，构建会直接使用，不联网。
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include/MinHook.h")
    message(FATAL_ERROR
        "third_party/minhook is missing. Run:\n"
        "  pwsh -File scripts/fetch-deps.ps1\n"
        "(build.bat 会自动执行这一步)")
endif()
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/include/reshade.hpp")
    message(FATAL_ERROR
        "third_party/reshade/include is missing. Run:\n"
        "  pwsh -File scripts/fetch-deps.ps1\n"
        "(build.bat 会自动执行这一步)")
endif()

add_library(reshade_preui_fsr3 SHARED
    src/main.cpp
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hde/hde64.c
)

target_include_directories(reshade_preui_fsr3 PRIVATE
    third_party/reshade/include
    third_party/minhook/include
    third_party/minhook/src
)

target_compile_definitions(reshade_preui_fsr3 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    # 版本号唯一来源：改上面的 project(... VERSION)，日志和 Release 产物会跟着变
    PREUI_VERSION="${PROJECT_VERSION}"
)

set_target_properties(reshade_preui_fsr3 PROPERTIES
    PREFIX ""
    SUFFIX ".addon64"
    OUTPUT_NAME "ReShade_PreUI_FSR"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
