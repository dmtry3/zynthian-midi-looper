#!/bin/bash
# Builds a ready-to-install release archive (plugin binary + widget + install scripts).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:-0.8.0}"
"$ROOT/scripts/build.sh"
NAME="midi_looper-$VER-aarch64"
OUT="$ROOT/dist/$NAME"
rm -rf "$OUT"
mkdir -p "$OUT/ui"
cp -r "$ROOT/build/midi_looper.lv2" "$OUT/"
cp "$ROOT/ui/zynthian_widget_midi_looper.py" "$OUT/ui/"
cp "$ROOT/README.md" "$ROOT/README.ru.md" "$ROOT/CHANGELOG.md" "$OUT/"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$OUT/"

cat > "$OUT/install.sh" <<'EOS'
#!/bin/bash
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR" "$UI_DIR/zyngui/widgets_user"
rm -rf "$LV2_DIR/midi_looper.lv2"
cp -r "$HERE/midi_looper.lv2" "$LV2_DIR/"
cp "$HERE/ui/zynthian_widget_midi_looper.py" "$UI_DIR/zyngui/widgets_user/"
echo "Installed. Restart Zynthian: systemctl restart zynthian"
EOS

cat > "$OUT/uninstall.sh" <<'EOS'
#!/bin/bash
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
rm -rf "$LV2_DIR/midi_looper.lv2"
rm -f  "$UI_DIR/zyngui/widgets_user/zynthian_widget_midi_looper.py"
echo "Removed."
EOS
chmod +x "$OUT/install.sh" "$OUT/uninstall.sh"

tar czf "$ROOT/dist/$NAME.tar.gz" -C "$ROOT/dist" "$NAME"
echo "Created: dist/$NAME.tar.gz"
