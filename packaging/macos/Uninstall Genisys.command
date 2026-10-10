#!/bin/bash
# Removes Genisys from this Mac: double-click this file in Finder.
# Only Genisys's own files are removed.

echo ""
echo "  GENISYS uninstaller"
echo "  ==================="
echo ""

removed=0
for path in \
    "$HOME/Library/Audio/Plug-Ins/VST3/Genisys.vst3" \
    "$HOME/Library/Audio/Plug-Ins/Components/Genisys.component" \
    "/Applications/Genisys.app" \
    "$HOME/Applications/Genisys.app"; do
    if [ -e "$path" ]; then
        rm -rf "$path" && echo "  [ ok ] removed $path" && removed=1
    fi
done

killall -9 AudioComponentRegistrar >/dev/null 2>&1

echo ""
if [ "$removed" -eq 1 ]; then
    echo "  Genisys has been removed. Rescan plugins in your DAW to update its list."
else
    echo "  Genisys wasn't found, so there was nothing to remove."
fi
echo ""
read -r -p "  Press Return to close this window. " _
