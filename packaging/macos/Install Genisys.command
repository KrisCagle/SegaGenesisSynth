#!/bin/bash
# Installs Genisys on this Mac: double-click this file in Finder.
#
# Copies the plugins into your personal plug-in folders and the app into
# Applications, then clears macOS's "downloaded from the internet" flag so
# your DAW can load them. Nothing outside those folders is touched, and no
# password is needed.

cd "$(dirname "$0")" || exit 1

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
if [ -w /Applications ]; then APP_DIR="/Applications"; else APP_DIR="$HOME/Applications"; fi

echo ""
echo "  GENISYS installer"
echo "  ================="
echo ""

failed=0
install_item() {
    local item="$1" dest="$2" label="$3"
    if [ ! -e "$item" ]; then
        echo "  [skip] $label: $item isn't next to this installer"
        return
    fi
    mkdir -p "$dest" || { echo "  [fail] couldn't create $dest"; failed=1; return; }
    rm -rf "$dest/$item"
    if cp -R "$item" "$dest/"; then
        # Clear the quarantine flag macOS adds to downloads (the same as the
        # manual "xattr" step in the README).
        xattr -dr com.apple.quarantine "$dest/$item" 2>/dev/null
        echo "  [ ok ] $label -> $dest"
    else
        echo "  [fail] couldn't copy $item to $dest"
        failed=1
    fi
}

install_item "Genisys.vst3" "$VST3_DIR" "VST3 plugin (Ableton, Reaper, Bitwig, FL Studio, Studio One...)"
install_item "Genisys.component" "$AU_DIR" "Audio Unit (Logic Pro, GarageBand)"
install_item "Genisys.app" "$APP_DIR" "Standalone app"

# Make macOS re-read its list of Audio Units, so Logic sees Genisys
# without a restart of the whole computer.
killall -9 AudioComponentRegistrar >/dev/null 2>&1

echo ""
if [ "$failed" -eq 0 ]; then
    echo "  Done! Now open your DAW and rescan plugins."
    echo "  - Ableton Live: Settings > Plug-Ins > Use VST3 Plug-In System Folders > Rescan"
    echo "  - Logic Pro: just restart Logic"
    echo "  Look for Genisys under the maker name \"Kris Cagle\"."
else
    echo "  Something didn't install. See the [fail] lines above, or follow the"
    echo "  manual steps: https://github.com/KrisCagle/SegaGenesisSynth#install-the-plugin-for-your-daw"
fi
echo ""
read -r -p "  Press Return to close this window. " _
