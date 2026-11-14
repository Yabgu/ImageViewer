#pragma once

/*
 * repo_root.hpp — locate the checkout from a running test binary.
 *
 * The registration tests compare the CLSID compiled into the provider with the
 * one the install scripts write.  Those are text files outside the build tree,
 * so the test has to find the source tree from wherever the generator put the
 * executable: build-ninja/src/tests, build/src/tests/Release, and so on.
 */

#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace iwtest {

inline std::filesystem::path ExecutablePath()
{
#ifdef _WIN32
    wchar_t buffer[32768];
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(32768));
    if (length == 0 || length >= 32768)
        return {};
    return std::filesystem::path(buffer);
#else
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (length <= 0)
        return {};
    return std::filesystem::path(std::string(buffer, static_cast<std::size_t>(length)));
#endif
}

/* The nearest ancestor that looks like this repository. */
inline std::filesystem::path RepoRoot()
{
    std::error_code ec;
    auto dir = ExecutablePath().parent_path();
    for (;;) {
        if (!dir.empty() &&
            std::filesystem::exists(dir / "CMakeLists.txt", ec) &&
            std::filesystem::is_directory(dir / "scripts", ec))
            return dir;
        const auto parent = dir.parent_path();
        if (parent == dir || parent.empty())
            return {};
        dir = parent;
    }
}

} // namespace iwtest
