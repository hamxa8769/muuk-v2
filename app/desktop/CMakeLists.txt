add_executable(muuk-desktop
    src/main.cpp
    src/live_controller.cpp
    src/dashboard_window.cpp
    src/glass_ui.cpp
    src/config_discovery.cpp
    src/open_path.cpp
    src/app_prefs.cpp
    src/system_monitor.cpp
    src/windows_startup.cpp
    src/updater.cpp
)

# The app version comes from the single source of truth: the top-level
# project(VERSION ...). The title bar, the header label and the updater all
# read it through src/version.hpp; nothing else hard-codes a version.
target_compile_definitions(muuk-desktop PRIVATE MUUK_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(muuk-desktop PRIVATE muuk_ctrader_fix muuk_mt5_bridge imgui)
if(WIN32)
    # winhttp: the self-updater's GitHub check and zip download.
    target_link_libraries(muuk-desktop PRIVATE winhttp)
    # A Windows desktop app: no console window next to it, the Muuk icon and
    # version info. main() stays the entry point; started from a terminal it
    # still prints there (AttachConsole in main.cpp).
    target_sources(muuk-desktop PRIVATE res/muuk.rc)
    set_target_properties(muuk-desktop PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(muuk-desktop PRIVATE /ENTRY:mainCRTStartup)
    endif()
    # dwmapi: dark native title bar to match the app theme; shell32: open PDFs/folders.
    # ole32/uuid: the desktop / Start menu shortcuts (IShellLink).
    target_link_libraries(muuk-desktop PRIVATE dwmapi shell32 psapi wtsapi32 ole32 uuid)
endif()
target_compile_features(muuk-desktop PRIVATE cxx_std_20)

# Its own process and file name, apart from the production copy on this PC.
set_target_properties(muuk-desktop PROPERTIES OUTPUT_NAME muuk-upgrade-desktop)

# Self-updater tests: only the platform-independent logic (version
# compare, release JSON, the *.local.json keep rule) -- no ImGui, no
# Windows headers, so this builds on Linux too.
if(MUUK_BUILD_TESTS)
    add_executable(muuk_updater_tests
        tests/test_updater.cpp
        src/updater.cpp
    )
    target_include_directories(muuk_updater_tests PRIVATE src)
    target_link_libraries(muuk_updater_tests PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    if(WIN32)
        # On Windows the test binary also compiles updater.cpp's WinHTTP
        # code, so it needs winhttp too -- otherwise the link fails with
        # 12 unresolved WinHttp symbols (LNK2019). On Linux the WinHTTP
        # parts are compiled out, so no link is needed there.
        target_link_libraries(muuk_updater_tests PRIVATE winhttp)
    endif()
    target_compile_features(muuk_updater_tests PRIVATE cxx_std_20)
    add_test(NAME updater COMMAND muuk_updater_tests)
endif()
