#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR" "$UI_DIR/zyngui/widgets_user"
ln -sfn "$ROOT/build/midi_looper.lv2" "$LV2_DIR/midi_looper.lv2"
ln -sfn "$ROOT/ui/zynthian_widget_midi_looper.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_midi_looper.py"
echo "Installed. Restart Zynthian: systemctl restart zynthian"
