#!/usr/bin/env bash
#
# install-desktop-linux.sh — register ImageViewer with the Linux desktop.
#
# Installs, for the current user only (no root, nothing outside $HOME):
#
#   $XDG_DATA_HOME/applications/ImageViewer.desktop      the launcher
#   $XDG_DATA_HOME/mime/packages/image-x-iwi.xml         the .iwi type
#   $XDG_DATA_HOME/icons/hicolor/scalable/apps/iview.svg the icon
#   ~/.local/bin/iview                                   a symlink to the build
#
# and makes ImageViewer the default handler for image/x-iwi.
#
#   ./scripts/install-desktop-linux.sh                 register, iview default for .iwi
#   ./scripts/install-desktop-linux.sh --binary PATH   use a specific build
#   ./scripts/install-desktop-linux.sh --default-all-images
#                                                      also take over PNG/JPEG/
#                                                      WebP/… (see the note below)
#
# # Why a symlink and not a copy
#
# iview loads its format plugins from beside the executable: PluginManager
# dlopen()s "libImageLoaderIWI.so" and, when that fails, retries next to
# /proc/self/exe.  That path is resolved with std::filesystem::canonical, which
# follows symlinks, so a symlink in ~/.local/bin still points the loader at the
# build directory where the DSOs live.  A *copy* of the binary would strand the
# plugins and .iwi files would fail to open — measured, not assumed.
#
# # Why .iwi only by default
#
# image/x-iwi is a new type with no existing handler, so claiming it takes
# nothing away.  The other image types usually already have a default viewer
# (this machine: Gwenview for PNG and JPEG), and that is the user's choice to
# change, not this script's.  Registering them in MimeType= still puts
# ImageViewer in their "Open With" list.  --default-all-images opts in to
# taking them over.

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

readonly DATA_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}"
readonly CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
readonly BIN_DIR="${IVIEW_INSTALL_BIN_DIR:-${HOME}/.local/bin}"

readonly APP_ID="ImageViewer"
readonly APP_FILE="${APP_ID}.desktop"
readonly MIME_PACKAGE="image-x-iwi.xml"
readonly MIME_TYPE="image/x-iwi"
readonly ICON_NAME="iview"

readonly DESKTOP_SRC="${REPO_ROOT}/resources/linux/${APP_FILE}"
readonly MIME_SRC="${REPO_ROOT}/resources/linux/${MIME_PACKAGE}"
readonly ICON_SRC="${REPO_ROOT}/assets/icon/${ICON_NAME}.svg"

WANT_ALL_DEFAULTS=0
FORCE=0
BINARY=""

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

usage() {
    sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)             [[ $# -ge 2 ]] || die "--binary needs a path"
                              BINARY="$2"; shift 2 ;;
        --default-all-images) WANT_ALL_DEFAULTS=1; shift ;;
        --force)              FORCE=1; shift ;;
        -h|--help)            usage ;;
        *)                    die "unknown argument: $1 (try --help)" ;;
    esac
done

for src in "$DESKTOP_SRC" "$MIME_SRC" "$ICON_SRC"; do
    [[ -f "$src" ]] || die "missing source file: $src"
done

# ── Locate the binary ────────────────────────────────────────────────────────
#
# --binary wins; then the newest build in the repo; then whatever `iview` on
# PATH resolves to.  `ls -td` sorts by mtime, so a freshly rebuilt tree is
# preferred over a stale one.

resolve_binary() {
    local candidate=""

    if [[ -n "$BINARY" ]]; then
        readlink -f -- "$BINARY"
        return 0
    fi

    candidate="$(ls -1dt "${REPO_ROOT}"/build*/src/imageviewer/iview \
                          "${REPO_ROOT}"/out/*/iview 2>/dev/null | head -n1 || true)"
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    if candidate="$(command -v iview 2>/dev/null)"; then
        candidate="$(readlink -f -- "$candidate")"
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    return 1
}

step "Finding the iview binary"
BINARY_RESOLVED="$(resolve_binary)" || die "no iview binary found.
Build one first:
    cmake -S \"$REPO_ROOT\" -B \"$REPO_ROOT/build\" -DIMAGEVIEWER_BUILD_VIEWER=ON -DIMAGEVIEWER_BUILD_PLUGINS=ON
    cmake --build \"$REPO_ROOT/build\" -j
or point at an existing build with --binary PATH."
[[ -x "$BINARY_RESOLVED" ]] || die "not executable: $BINARY_RESOLVED"
note "binary: $BINARY_RESOLVED"

# The binary is only half of iview: every format lives in a DSO beside it.  An
# install that skips them produces a launcher that cannot open anything, so
# fail here with the reason instead of at the first double-click.
plugin_dir="$(dirname -- "$BINARY_RESOLVED")"
if ! compgen -G "${plugin_dir}/libImageLoader*.so" >/dev/null; then
    if [[ $FORCE -eq 1 ]]; then
        note "warning: no libImageLoader*.so beside the binary; .iwi will not open"
    else
        die "no loader plugins beside ${BINARY_RESOLVED}
Expected libImageLoader*.so in ${plugin_dir}.
Rebuild with -DIMAGEVIEWER_BUILD_PLUGINS=ON, or pass --force to install anyway."
    fi
else
    note "plugins: $(compgen -G "${plugin_dir}/libImageLoader*.so" | wc -l) loader DSO(s) beside it"
fi

# ── Install the binary symlink ──────────────────────────────────────────────
#
# A regular file already at this path is somebody else's iview; overwriting it
# silently is not this script's call.

step "Linking ${BIN_DIR}/iview"
mkdir -p "$BIN_DIR"
if [[ -e "${BIN_DIR}/iview" && ! -L "${BIN_DIR}/iview" ]]; then
    if [[ $FORCE -eq 1 ]]; then
        note "replacing existing file (--force)"
    else
        die "${BIN_DIR}/iview exists and is not a symlink; refusing to replace it.
Move it aside, or re-run with --force."
    fi
fi
ln -sfn "$BINARY_RESOLVED" "${BIN_DIR}/iview"
note "-> $BINARY_RESOLVED"

# ── Install the MIME type ───────────────────────────────────────────────────

step "Installing the image/x-iwi MIME type"
mkdir -p "${DATA_HOME}/mime/packages"
install -m 644 "$MIME_SRC" "${DATA_HOME}/mime/packages/${MIME_PACKAGE}"
note "${DATA_HOME}/mime/packages/${MIME_PACKAGE}"

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "${DATA_HOME}/mime"
    note "mime database updated"
else
    printf 'warning: update-mime-database not found; file types will not resolve\n' >&2
fi

# ── Install the launcher ────────────────────────────────────────────────────

step "Installing ${APP_FILE}"
mkdir -p "${DATA_HOME}/applications"

# Exec=iview relies on ~/.local/bin being on PATH.  When it is not, bake the
# absolute path in: a launcher whose Exec cannot be resolved is a dead entry
# that looks installed.
bin_on_path() {
    case ":${PATH}:" in
        *":${BIN_DIR}:"*) return 0 ;;
        *)                return 1 ;;
    esac
}

tmp_desktop="$(mktemp)"
trap 'rm -f "$tmp_desktop"' EXIT

if bin_on_path; then
    cp "$DESKTOP_SRC" "$tmp_desktop"
    note "Exec=iview (${BIN_DIR} is on PATH)"
else
    exec_path="${BIN_DIR}/iview"
    tryexec_path="$exec_path"
    # Quote for the desktop-entry Exec grammar if the path has a space.
    [[ "$exec_path" == *\ * ]] && exec_path="\"${exec_path}\""
    # TryExec is a bare path in the key file's syntax: quoting it would make
    # the value start with a quote character and never match.
    sed -e "s|^Exec=iview |Exec=${exec_path} |" \
        -e "s|^TryExec=iview\$|TryExec=${tryexec_path}|" \
        "$DESKTOP_SRC" > "$tmp_desktop"
    note "Exec=${exec_path} (${BIN_DIR} is not on PATH)"
fi

install -m 644 "$tmp_desktop" "${DATA_HOME}/applications/${APP_FILE}"
note "${DATA_HOME}/applications/${APP_FILE}"

if command -v desktop-file-validate >/dev/null 2>&1; then
    if desktop-file-validate "${DATA_HOME}/applications/${APP_FILE}"; then
        note "desktop entry is valid"
    else
        printf 'warning: desktop-file-validate reported problems (see above)\n' >&2
    fi
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${DATA_HOME}/applications"
    note "desktop database updated"
else
    printf 'warning: update-desktop-database not found; associations may not refresh\n' >&2
fi

# ── Default handler ─────────────────────────────────────────────────────────
#
# xdg-mime writes $CONFIG_HOME/mimeapps.list.  Keep one copy of the original so
# a "why did my viewer change" question has an answer on disk.

step "Setting the default handler"
if [[ -f "${CONFIG_HOME}/mimeapps.list" && ! -f "${CONFIG_HOME}/mimeapps.list.pre-iview" ]]; then
    cp "${CONFIG_HOME}/mimeapps.list" "${CONFIG_HOME}/mimeapps.list.pre-iview"
    note "backed up mimeapps.list -> mimeapps.list.pre-iview"
fi

set_default() {
    # xdg-mime's argument order is `default <application> <mimetype>`.  Getting
    # it backwards writes a nonsense pair into mimeapps.list *and* exits 0, so
    # there is no safe fallback to try: assert the order instead.
    if xdg-mime default "$APP_FILE" "$1"; then
        note "$1 -> $APP_FILE"
    else
        printf 'warning: could not set the default for %s\n' "$1" >&2
    fi
}

set_default "$MIME_TYPE"

if [[ $WANT_ALL_DEFAULTS -eq 1 ]]; then
    # Every type the launcher advertises, so a double-click on any of them
    # lands here instead of whichever viewer held them before.
    mapfile -t all_types < <(
        sed -n 's/^MimeType=//p' "$DESKTOP_SRC" | tr ';' '\n' | sed '/^$/d'
    )
    for t in "${all_types[@]}"; do
        [[ "$t" == "$MIME_TYPE" ]] && continue
        set_default "$t"
    done
fi

# ── Icon ────────────────────────────────────────────────────────────────────

step "Installing the icon"
icon_dir="${DATA_HOME}/icons/hicolor/scalable/apps"
mkdir -p "$icon_dir"
install -m 644 "$ICON_SRC" "${icon_dir}/${ICON_NAME}.svg"
note "${icon_dir}/${ICON_NAME}.svg"
if command -v gtk-update-icon-cache >/dev/null 2>&1 \
   && [[ -f "${DATA_HOME}/icons/hicolor/index.theme" ]]; then
    gtk-update-icon-cache -q -t -f "${DATA_HOME}/icons/hicolor" 2>/dev/null || true
    note "icon cache refreshed"
fi

# ── Verify ──────────────────────────────────────────────────────────────────
#
# The point of the script is that a double-click resolves to this launcher.
# Assert the two queries that decide it, using a real IWI fixture from the repo
# rather than a file invented for the test.

step "Verifying"
probe="${REPO_ROOT}/src/tests/abi/fixtures/rgba_4x4.iwi"
status=0

if [[ -f "$probe" ]]; then
    detected="$(xdg-mime query filetype "$probe")"
    note "xdg-mime query filetype rgba_4x4.iwi -> ${detected:-<none>}"
    if [[ "$detected" != "$MIME_TYPE" ]]; then
        printf 'error: expected %s, got %s\n' "$MIME_TYPE" "${detected:-<none>}" >&2
        status=1
    fi
else
    note "skipped file-type check (no fixture at $probe)"
fi

default_handler="$(xdg-mime query default "$MIME_TYPE")"
note "xdg-mime query default ${MIME_TYPE} -> ${default_handler:-<none>}"
if [[ "$default_handler" != "$APP_FILE" ]]; then
    printf 'error: expected %s, got %s\n' "$APP_FILE" "${default_handler:-<none>}" >&2
    status=1
fi

if [[ $status -ne 0 ]]; then
    printf '\nRegistration did not take.  Some desktops cache MIME associations;\n' >&2
    printf 'logging out and back in (or restarting the file manager) picks it up.\n' >&2
    exit 1
fi

printf '\nImageViewer is registered.\n'
printf '  double-click a .iwi file, or run: iview <file>\n'
if [[ $WANT_ALL_DEFAULTS -eq 0 ]]; then
    printf '  to make it the default viewer for PNG/JPEG/… too, re-run with --default-all-images\n'
fi
