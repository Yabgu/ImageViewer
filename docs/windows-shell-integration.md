# Windows shell integration for `.iwi`

ImageViewer registers itself with the Windows shell **for the current user
only** (`HKEY_CURRENT_USER`, no elevation, no `regsvr32`). Two things are
registered: a file type that opens `.iwi` in `iview.exe`, and a thumbnail
handler that makes Explorer draw the actual image instead of the generic
file-type icon.

## What gets registered

Association, written by both install scripts:

| Key | Value |
| --- | --- |
| `HKCU\Software\Classes\.iwi` | `ImageViewer.IWI`, `Content Type = image/x-iwi` |
| `HKCU\Software\Classes\ImageViewer.IWI` | type name, `DefaultIcon`, `shell\open\command` |
| `HKCU\Software\Classes\Applications\iview.exe` | `FriendlyAppName`, `SupportedTypes\*iwi` |
| `HKCU\Software\Classes\SystemFileAssociations\.iwi\shell\OpenWithImageViewer` | right-click verb |

Thumbnail handler:

| Key | Value |
| --- | --- |
| `HKCU\Software\Classes\CLSID\{8A3D6F2C-5B47-4C1E-9F1A-6E2D4B8C0A31}` | `ImageViewer IWI Thumbnail Provider` |
| `…\InprocServer32` | `<install dir>\IwiThumbnailProvider.dll`, `ThreadingModel = Apartment` |
| `HKCU\Software\Classes\.iwi\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}` | provider CLSID |
| `HKCU\Software\Classes\ImageViewer.IWI\ShellEx\{E357FCCD-…}` | provider CLSID |

`{E357FCCD-A995-4576-B01F-234630154E96}` is the shell's thumbnail verb; it is
fixed by Windows. The provider CLSID is chosen by this project and appears in
three files that cannot include one another — `src/imageviewer/shell/ThumbnailProviderIds.ixx`,
`scripts/install-desktop-windows.ps1` and `scripts/install-desktop-windows.bat`.
`src/tests/test_iwi_thumbnail.cpp` fails if they disagree, because a mismatch
does not produce an error anywhere: Explorer simply finds no handler and draws
the icon.

Both `ShellEx` locations are written deliberately. The ProgID's entry is what
the shell uses after resolving `.iwi`; the extension's entry covers a direct
lookup of the extension key. Registering only one of them works on some builds
and not others.

## Building

The provider is a normal CMake target and needs no viewer toolchain:

```powershell
cmake -S . -B build -DIMAGEVIEWER_BUILD_THUMBNAIL_PROVIDER=ON
cmake --build build --config Release --target IwiThumbnailProvider
```

It is built **outside** the `IMAGEVIEWER_BUILD_VIEWER` gate, so a headless
build still produces and tests it. Turn it off with
`-DIMAGEVIEWER_BUILD_THUMBNAIL_PROVIDER=OFF`; `cmake --install` puts the DLL
next to `iview.exe` either way.

## Installing

```powershell
./scripts/install-desktop-windows.ps1 -Binary <dir>\iview.exe
./scripts/install-desktop-windows.ps1 -Uninstall
```

The script resolves `iview.exe` itself when `-Binary` is omitted, refuses to
register a build that has no `ImageLoaderIWI.dll` beside it (unless `-Force`),
and skips the thumbnail keys when `IwiThumbnailProvider.dll` is absent — the
association still works in that case, Explorer just shows the icon. Every value
is read back from the hive and compared before the script reports success.

`scripts/install-desktop-windows.bat` writes the same keys for a cmd-only
environment. Prefer the PowerShell script: it records the previous `.iwi`
owner so `-Uninstall` can restore it, and it deletes `.iwi` when there was no
previous owner.

## Why there is no `DllRegisterServer`

`regsvr32` is normally elevated, and a handler registered by an administrator
is registered for the wrong user — the association and the thumbnail would
disagree for the account that actually browses files. The DLL exports only
`DllGetClassObject` and `DllCanUnloadNow` (listed in
`src/imageviewer/shell/ThumbnailProviderIWI.def`, because `combaseapi.h`
already declares both and a module unit cannot redefine them).

## What the thumbnail does

- Reads the 74-byte header first, then only the source rows the output grid
  samples. A 4096×4096 RGBA `.iwi` is never fully allocated in `dllhost.exe`.
- Never upscales: a 4×4 `.iwi` stays 4×4.
- Averages in linear light and encodes back to sRGB, so fine patterns do not
  come out too dark.
- Output is 32bpp premultiplied BGRA; alpha type is `WTSAT_ARGB` for
  four-component files and `WTSAT_RGB` for three-component ones.
- Accepts exactly what the viewer accepts: interleaved RGB8/RGBA8. Both read
  the same header code (`IwiFormat.ixx`), and the ABI suite pins the viewer's
  refusals independently.

Deliberately refused, so the icon is shown instead of a wrong picture:

- PQ and HLG transfer functions. There is no tone mapping here, and guessing a
  curve would render a wrong image.
- Planar storage, sub-8-bit components, components off their `i * 8` slot.
- Widths or heights beyond 32768 px (a provider-side cap, not a format rule —
  such a file still opens in `iview.exe`).

## Troubleshooting

**The icon shows, no thumbnail.** Check the DLL path recorded by the shell:

```powershell
reg query "HKCU\Software\Classes\CLSID\{8A3D6F2C-5B47-4C1E-9F1A-6E2D4B8C0A31}\InprocServer32"
```

If it names a path that no longer exists (a moved or deleted build tree),
re-run the install script — the path is absolute and is not updated when the
build directory moves.

**A file keeps showing a stale or fallback image.** Explorer caches thumbnails
per file, including an earlier failure. Copy the file to a new name, or clear
the cache:

```powershell
ie4uinit.exe -show
```

**Check the handler without Explorer.** `ctest -R iview_shell_tests` covers the
decoder directly. To exercise the registered path, ask the shell for a
thumbnail with `IShellItemImageFactory::GetImage` and `SIIGBF_THUMBNAILONLY`,
which refuses the icon fallback and therefore fails if the handler is not
registered.

**`-Uninstall` did not restore the previous handler.** It restores the ProgID
that `.iwi` pointed at when the script last ran, not an arbitrary earlier one.
Windows' own `UserChoice` hash is never touched, so an explicit user choice
made in Explorer's "Open with" dialog always wins and is never overridden.
