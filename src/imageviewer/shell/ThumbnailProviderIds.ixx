module;

#ifdef _WIN32
#include <guiddef.h>
#endif

export module ThumbnailProviderIds;

/*
 * ThumbnailProviderIds — the identifiers registration and code must agree on.
 *
 * The CLSID appears in three places that cannot import each other: this module
 * (C++), scripts/install-desktop-windows.ps1 (PowerShell) and
 * scripts/install-desktop-windows.bat (cmd).  A mismatch does not fail loudly —
 * Explorer simply finds no handler and shows the file-type icon — so
 * src/tests/test_iwi_thumbnail.cpp asserts all three carry the same text.
 *
 * The ShellEx GUID {E357FCCD-A995-4576-B01F-234630154E96} is the shell's
 * thumbnail-handler verb.  It is fixed by Windows, not chosen here.
 */

export namespace iwthumb {

/* Keep in sync with the two install scripts; the test named above is the
 * mechanism that keeps them in sync. */
inline constexpr char kProviderClsidString[] =
    "{8A3D6F2C-5B47-4C1E-9F1A-6E2D4B8C0A31}";

inline constexpr char kThumbnailShellExGuidString[] =
    "{E357FCCD-A995-4576-B01F-234630154E96}";

/* The DLL the scripts must point InprocServer32 at. */
inline constexpr char kProviderDllName[] = "IwiThumbnailProvider.dll";

inline constexpr char kProviderFriendlyName[] =
    "ImageViewer IWI Thumbnail Provider";

#ifdef _WIN32
/* {8A3D6F2C-5B47-4C1E-9F1A-6E2D4B8C0A31} */
inline constexpr GUID CLSID_IwiThumbnailProvider =
    { 0x8a3d6f2c, 0x5b47, 0x4c1e,
      { 0x9f, 0x1a, 0x6e, 0x2d, 0x4b, 0x8c, 0x0a, 0x31 } };
#endif

} // export namespace iwthumb
