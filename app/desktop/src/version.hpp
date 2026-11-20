#pragma once

// The app version. Single source of truth: CMake's project(VERSION ...) in
// the top-level CMakeLists.txt, passed in as MUUK_VERSION_STRING by
// app/desktop/CMakeLists.txt. Nothing else in the tree hard-codes a version:
// the title bar, the header label and the updater all read kMuukVersion.
// A direct compile without CMake falls back to 0.0.0-dev.
#ifndef MUUK_VERSION_STRING
#define MUUK_VERSION_STRING "0.0.0-dev"
#endif

namespace muuk::desktop {

inline constexpr const char* kMuukVersion = MUUK_VERSION_STRING;

}  // namespace muuk::desktop
