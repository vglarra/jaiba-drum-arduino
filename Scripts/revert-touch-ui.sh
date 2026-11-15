#!/usr/bin/env bash
#
# revert-touch-ui.sh — undo the touch-keyboard customisation on this machine.
#
# The touch setup has two independent parts, so they can be reverted separately:
#
#   shift     Remove the "window slides up when the keyboard opens" helper
#             (jbshift@jaiba). The neon keyboard itself is left untouched.
#
#   keyboard  Restore gjs-osk to its stock stylesheet and stock settings
#             (layout, size, colours, docking). The shift helper is left
#             untouched.
#
#   all       Both of the above (default).
#
# Usage:
#   ./revert-touch-ui.sh            # same as "all"
#   ./revert-touch-ui.sh shift
#   ./revert-touch-ui.sh keyboard
#   ./revert-touch-ui.sh all --dry-run
#
# Notes:
#   * gjs-osk stylesheets and settings are re-read on extension enable, so no
#     logout is needed for the keyboard part.
#   * The shift helper is JavaScript and GNOME caches extension code for the
#     whole session; disabling it takes effect immediately, but if you want to
#     re-enable it later you need a logout/login to load its code again.
#
set -euo pipefail

EXT_DIR="$HOME/.local/share/gnome-shell/extensions/gjsosk@vishram1123.com"
SHIFT_DIR="$HOME/.local/share/gnome-shell/extensions/jbshift@jaiba"
OSK_SCHEMA="org.gnome.shell.extensions.gjsosk"
GSETTINGS_SCHEMA_DIR="$EXT_DIR/schemas"
export GSETTINGS_SCHEMA_DIR

MODE="all"
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        shift|keyboard|all) MODE="$arg" ;;
        --dry-run|-n)       DRY_RUN=1 ;;
        -h|--help)
            sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown argument: $arg (expected shift|keyboard|all|--dry-run)" >&2
            exit 2
            ;;
    esac
done

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  would run: %s\n' "$*"
    else
        printf '  %s\n' "$*"
        "$@"
    fi
}

remove_shift_helper() {
    echo "== shift helper (jbshift@jaiba) =="

    if [ ! -d "$SHIFT_DIR" ]; then
        echo "  not installed, nothing to do"
        return
    fi

    if command -v gnome-extensions >/dev/null && gnome-extensions list 2>/dev/null | grep -qx 'jbshift@jaiba'; then
        run gnome-extensions disable jbshift@jaiba || true
    fi

    # Drop it from the auto-enable list, if present.
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  would remove jbshift@jaiba from org.gnome.shell enabled-extensions"
    else
        python3 - "$SHIFT_DIR" <<'PY'
import ast
import shutil
import subprocess
import sys

path = sys.argv[1]
cur = subprocess.run(
    ["gsettings", "get", "org.gnome.shell", "enabled-extensions"],
    capture_output=True, text=True, check=True,
).stdout.strip()
try:
    items = ast.literal_eval(cur)
except (SyntaxError, ValueError):
    items = []
items = [i for i in items if i != "jbshift@jaiba"]
new = "[" + ", ".join("'%s'" % i for i in items) + "]"
subprocess.run(
    ["gsettings", "set", "org.gnome.shell", "enabled-extensions", new],
    check=True,
)
print("  enabled-extensions ->", new)
shutil.rmtree(path)
print("  removed", path)
PY
    fi
}

revert_keyboard() {
    echo "== gjs-osk keyboard =="

    if [ ! -d "$EXT_DIR" ]; then
        echo "  gjs-osk is not installed, nothing to do"
        return
    fi

    if [ -f "$EXT_DIR/stylesheet.css.orig-jbneon" ]; then
        run cp "$EXT_DIR/stylesheet.css.orig-jbneon" "$EXT_DIR/stylesheet.css"
    else
        echo "  warning: stock stylesheet backup not found; CSS left as-is"
    fi

    # Reset every key this setup touched back to the schema defaults.
    local keys=(
        custom-layout
        layout-landscape
        layout-portrait
        default-snap
        landscape-width-percent
        landscape-height-percent
        portrait-width-percent
        portrait-height-percent
        font-size-px
        font-bold
        round-key-corners
        outer-spacing-px
        background-r background-g background-b background-a
        background-r-dark background-g-dark background-b-dark background-a-dark
    )
    for key in "${keys[@]}"; do
        run gsettings reset "$OSK_SCHEMA" "$key"
    done

    # Re-read the stylesheet + settings.
    if command -v gnome-extensions >/dev/null; then
        run gnome-extensions disable gjsosk@vishram1123.com || true
        run gnome-extensions enable gjsosk@vishram1123.com || true
    fi
}

case "$MODE" in
    shift)    remove_shift_helper ;;
    keyboard) revert_keyboard ;;
    all)      remove_shift_helper; revert_keyboard ;;
esac

echo
if [ "$DRY_RUN" -eq 1 ]; then
    echo "dry run: nothing was changed"
elif [ "$MODE" = "all" ] || [ "$MODE" = "keyboard" ]; then
    echo "done. The keyboard is back to stock; log out/in if you also removed the shift helper and want its JS fully unloaded."
else
    echo "done."
fi
