<#
.SYNOPSIS
    install-desktop-windows.ps1 - register ImageViewer with the Windows shell.

.DESCRIPTION
    Registers, for the current user only (HKEY_CURRENT_USER, no administrator
    and nothing outside the user's hive):

        .iwi                              -> the ImageViewer.IWI ProgID
        ImageViewer.IWI                   the type, its icon and its open command
        Applications\iview.exe            the app entry "Open with" is built from
        SystemFileAssociations\.iwi       a right-click "Open with ImageViewer"

    then notifies the shell so Explorer picks the change up without a sign-out.

    This is the Windows counterpart of scripts/install-desktop-linux.sh, and the
    two make the same three claims: .iwi gets a type, ImageViewer handles it,
    and every other image type keeps whatever default the user already chose.
    Only .iwi is claimed here; PNG/JPEG/... keep their existing handler, which
    is why the ProgID is scoped to the extension rather than to the app.

.PARAMETER Binary
    Path to iview.exe. By default the newest build in this repository that has
    the IWI loader beside it is used, then whatever `iview` resolves to on PATH.

.PARAMETER Force
    Install even when ImageLoaderIWI.dll is not beside the executable, and
    replace an .iwi ProgID that may have been set by something else. Without
    it, either condition stops the script with the reason.

.PARAMETER Uninstall
    Remove everything this script writes and restore the .iwi ProgID it found.

.EXAMPLE
    ./scripts/install-desktop-windows.ps1

.EXAMPLE
    ./scripts/install-desktop-windows.ps1 -Binary C:\work\ImageViewer\build-release\src\imageviewer\iview.exe

.EXAMPLE
    ./scripts/install-desktop-windows.ps1 -Uninstall

.NOTES
    Why the registry holds an absolute path and no copy of the binary:
    iview loads its format plugins from beside the executable.  The Linux
    script makes the same point about its symlink - a copy of the binary
    strands the loaders, and .iwi stops opening.  There is nothing to copy
    here, so the command value names the built executable directly.

    Why HKEY_CURRENT_USER and not HKEY_LOCAL_MACHINE: a machine-wide handler
    is an administrator decision that affects every account, and the Linux
    script deliberately stayed inside $HOME.  Per-user registration needs no
    elevation and is undone by -Uninstall.
#>

[CmdletBinding()]
param(
    [string]$Binary,
    [switch]$Force,
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The one type this registers, and the ProgID it maps to.  Constant on purpose:
# a ProgID that drifted from the key names below would register a type nothing
# claims.
$ProgId      = 'ImageViewer.IWI'
$Extension   = '.iwi'
$ContentType = 'image/x-iwi'
$AppExeName  = 'iview.exe'
$VerbKeyName = 'OpenWithImageViewer'
$VerbLabel   = 'Open with ImageViewer'

$ClassesRoot = 'HKCU:\Software\Classes'
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Split-Path -Parent $ScriptDir

# The .NET registry API takes a path *relative to the hive* ("Software\Classes\…"),
# while Test-Path and Remove-Item take a provider path ("HKCU:\Software\Classes\…").
# Callers below write the provider form throughout — it is the one a PowerShell
# reader can paste into a console — and it is converted in exactly one place.
# Handing the provider form straight to CreateSubKey does not fail: it quietly
# creates a key literally named `HKCU:` under the hive.
$HivePrefix = 'HKCU:\'

function Write-Step { param([string]$Message) Write-Host "`n==> $Message" }
function Write-Note { param([string]$Message) Write-Host "    $Message" }
function Stop-With   { param([string]$Message) throw $Message }

# ── Registry helpers ────────────────────────────────────────────────────────
#
# These talk to the .NET registry API rather than to Set-ItemProperty because
# the default value of a key has the empty name, and `-Name '(default)'` only
# works by way of PowerShell's path provider quirks.  An empty-name value is
# exactly what `.iwi` = ProgID is, so name it as such.

function ConvertTo-SubKeyPath {
    param([Parameter(Mandatory)][string]$Path)
    if (-not $Path.StartsWith($HivePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Stop-With "registry path must start with $HivePrefix : $Path"
    }
    return $Path.Substring($HivePrefix.Length)
}

function Set-RegValue {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Name,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Value
    )
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey((ConvertTo-SubKeyPath $Path))
    if ($null -eq $key) { Stop-With "could not create or open $Path" }
    try { $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String) }
    finally { $key.Dispose() }
}

function Get-RegValue {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Name
    )
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey((ConvertTo-SubKeyPath $Path))
    if ($null -eq $key) { return $null }
    try { return $key.GetValue($Name, $null) }
    finally { $key.Dispose() }
}

function Remove-RegTree {
    param([Parameter(Mandatory)][string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
        Write-Note "removed $Path"
    }
}

# Explorer caches associations in memory; SHCNE_ASSOCCHANGED is how a script
# tells it to re-read them.  Without this the registry is right and the
# double-click still goes to the old handler until the next sign-in.
function Update-ShellAssociations {
    if (-not ('Win32.ShellNotify' -as [type])) {
        Add-Type -Namespace Win32 -Name ShellNotify -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);
'@
    }
    # SHCNE_ASSOCCHANGED = 0x08000000, SHCNF_IDLIST = 0x0000
    [Win32.ShellNotify]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Note 'shell notified (SHCNE_ASSOCCHANGED)'
}

# ── Uninstall ───────────────────────────────────────────────────────────────

if ($Uninstall) {
    Write-Step 'Removing the ImageViewer registration'

    $previous = Get-RegValue -Path "$ClassesRoot\$ProgId" -Name 'PreviousIwiProgId'

    Remove-RegTree "$ClassesRoot\$ProgId"
    Remove-RegTree "$ClassesRoot\Applications\$AppExeName"
    Remove-RegTree "$ClassesRoot\SystemFileAssociations\$Extension\shell\$VerbKeyName"

    # Restore whatever owned .iwi before this script ran.  Anything else would
    # leave the extension pointing at a ProgID whose key no longer exists.
    if ($previous) {
        Set-RegValue -Path "$ClassesRoot\$Extension" -Name '' -Value $previous
        Write-Note "$Extension restored to $previous"
    }
    else {
        Remove-RegTree "$ClassesRoot\$Extension"
    }

    Update-ShellAssociations
    Write-Host "`nImageViewer is no longer registered for $Extension."
    return
}

# ── Locate the binary ───────────────────────────────────────────────────────
#
# A build that shipped without ImageLoaderIWI.dll produces a launcher that
# cannot open the one type this script exists for, so prefer a tree that has
# it and say so when none does.

function Test-IwiPluginBeside {
    param([Parameter(Mandatory)][string]$ExePath)
    $dir = Split-Path -Parent $ExePath
    return (Test-Path -LiteralPath (Join-Path $dir 'ImageLoaderIWI.dll'))
}

function Resolve-Binary {
    if ($Binary) {
        $full = [System.IO.Path]::GetFullPath($Binary)
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
            Stop-With "not a file: $full"
        }
        return $full
    }

    # Newest first, across the layouts this project's CMake generators produce:
    #   build*/src/imageviewer/{Release,Debug}/iview.exe   (Visual Studio)
    #   build*/local/src/imageviewer/iview.exe             (Ninja install tree)
    $candidates = Get-ChildItem -Path $RepoRoot -Filter $AppExeName -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '[\\/]build[^\\/]*[\\/]' } |
        Sort-Object LastWriteTime -Descending

    $withPlugin = $candidates | Where-Object { Test-IwiPluginBeside $_.FullName } | Select-Object -First 1
    if ($withPlugin) { return $withPlugin.FullName }

    if ($candidates) { return $candidates[0].FullName }

    $onPath = Get-Command $AppExeName -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    Stop-With @'
no iview.exe found.
Build one first:
    cmake -S . -B build-release -DIMAGEVIEWER_BUILD_VIEWER=ON -DIMAGEVIEWER_BUILD_PLUGINS=ON
    cmake --build build-release --config Release
or point at an existing build with -Binary PATH.
'@
}

Write-Step 'Finding iview.exe'
$exe = Resolve-Binary
Write-Note "binary: $exe"

$plugin = Join-Path (Split-Path -Parent $exe) 'ImageLoaderIWI.dll'
if (-not (Test-Path -LiteralPath $plugin)) {
    if ($Force) {
        Write-Note "warning: no ImageLoaderIWI.dll beside the binary; .iwi will not open"
    }
    else {
        Stop-With @"
no ImageLoaderIWI.dll beside $exe
Expected it in $(Split-Path -Parent $exe).
iview loads its format plugins from beside the executable, so registering
this build would produce a handler that cannot open .iwi.  Rebuild with
-DIMAGEVIEWER_BUILD_PLUGINS=ON, or pass -Force to register it anyway.
"@
    }
}
else {
    Write-Note 'plugins: ImageLoaderIWI.dll is beside it'
}

# ── Register the type ───────────────────────────────────────────────────────

Write-Step "Registering the $ContentType type for $Extension"

# Remember who owned the extension, so -Uninstall can put it back.  Never
# overwrite an earlier backup: on a second run the "previous" value is ours,
# and recording that would make the undo a no-op.
$currentOwner = Get-RegValue -Path "$ClassesRoot\$Extension" -Name ''
if ($currentOwner -and $currentOwner -ne $ProgId) {
    $existingBackup = Get-RegValue -Path "$ClassesRoot\$ProgId" -Name 'PreviousIwiProgId'
    if (-not $existingBackup) {
        Set-RegValue -Path "$ClassesRoot\$ProgId" -Name 'PreviousIwiProgId' -Value $currentOwner
        Write-Note "$Extension was $currentOwner (recorded for -Uninstall)"
    }
}

Set-RegValue -Path "$ClassesRoot\$Extension" -Name '' -Value $ProgId
Set-RegValue -Path "$ClassesRoot\$Extension" -Name 'Content Type' -Value $ContentType
Write-Note "HKCU:\Software\Classes\$Extension -> $ProgId"

Set-RegValue -Path "$ClassesRoot\$ProgId"                       -Name '' -Value 'IWI Image'
Set-RegValue -Path "$ClassesRoot\$ProgId\DefaultIcon"           -Name '' -Value "`"$exe`",0"
Set-RegValue -Path "$ClassesRoot\$ProgId\shell\open\command"    -Name '' -Value "`"$exe`" `"%1`""
Write-Note "HKCU:\Software\Classes\$ProgId (icon + open command)"

# ── Register the application ────────────────────────────────────────────────
#
# Applications\<exe> with SupportedTypes is what puts ImageViewer in Explorer's
# "Open with" list for .iwi; the ProgID above decides who gets the double-click.

Set-RegValue -Path "$ClassesRoot\Applications\$AppExeName"                     -Name 'FriendlyAppName' -Value 'ImageViewer'
Set-RegValue -Path "$ClassesRoot\Applications\$AppExeName\SupportedTypes"      -Name '*iwi' -Value ''
Set-RegValue -Path "$ClassesRoot\Applications\$AppExeName\shell\open\command"  -Name '' -Value "`"$exe`" `"%1`""
Write-Note "HKCU:\Software\Classes\Applications\$AppExeName (Open with list)"

# ── Explicit right-click entry ──────────────────────────────────────────────
#
# Under SystemFileAssociations so the entry is present whether or not
# ImageViewer is the default for .iwi.

$verb = "$ClassesRoot\SystemFileAssociations\$Extension\shell\$VerbKeyName"
Set-RegValue -Path $verb             -Name ''     -Value $VerbLabel
Set-RegValue -Path $verb             -Name 'Icon' -Value "`"$exe`",0"
Set-RegValue -Path "$verb\command"   -Name ''     -Value "`"$exe`" `"%1`""
Write-Note "right-click verb: `"$VerbLabel`""

# ── Verify ──────────────────────────────────────────────────────────────────
#
# Read every value back from the hive and compare. A write through the wrong
# key form does not throw — the registry accepts the name and stores the value
# under it — so a script that reports only what it sent can be wrong silently.
# This is the check that turns that into a non-zero exit.

Write-Step 'Verifying the registration'
$expectedOpen = "`"$exe`" `"%1`""
$checks = @(
    @{ Path = "$ClassesRoot\$Extension";                             Name = '';            Value = $ProgId }
    @{ Path = "$ClassesRoot\$Extension";                             Name = 'Content Type'; Value = $ContentType }
    @{ Path = "$ClassesRoot\$ProgId";                                Name = '';            Value = 'IWI Image' }
    @{ Path = "$ClassesRoot\$ProgId\DefaultIcon";                    Name = '';            Value = "`"$exe`",0" }
    @{ Path = "$ClassesRoot\$ProgId\shell\open\command";             Name = '';            Value = $expectedOpen }
    @{ Path = "$ClassesRoot\Applications\$AppExeName\SupportedTypes"; Name = '*iwi';        Value = '' }
    @{ Path = $verb;                                                  Name = '';            Value = $VerbLabel }
    @{ Path = "$verb\command";                                        Name = '';            Value = $expectedOpen }
)
$mismatched = 0
foreach ($check in $checks) {
    $actual = Get-RegValue -Path $check.Path -Name $check.Name
    $label = if ($check.Name -eq '') { '(default)' } else { $check.Name }
    if ($actual -ne $check.Value) {
        Write-Host "    MISMATCH  $label  at  $($check.Path)"
        Write-Host "              stored '$actual', expected '$($check.Value)'"
        $mismatched++
    }
}
if ($mismatched -gt 0) {
    Stop-With "$mismatched registration value(s) did not read back; the registry was not changed as intended"
}
Write-Note "$($checks.Count) values read back"

Update-ShellAssociations

Write-Host @"

ImageViewer is registered for $Extension.
  handler   $exe
  type      $ContentType
  undo      ./scripts/install-desktop-windows.ps1 -Uninstall
"@
