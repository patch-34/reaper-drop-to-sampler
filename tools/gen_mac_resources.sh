#!/bin/sh
# Regenerate SWELL (macOS) dialog resources from src/prefs_dialog.rc.
# Run after editing prefs_dialog.rc; commit the generated .rc_mac_dlg / .rc_mac_menu.
set -e
cd "$(dirname "$0")/.."
perl vendor/WDL/WDL/swell/swell_resgen.pl src/prefs_dialog.rc
