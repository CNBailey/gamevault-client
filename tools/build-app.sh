#!/usr/bin/env bash
# GameVault for PS5 - builds the native home-screen app (app/) with the
# ps5-homebrew-ui kit, the way NASBridge does: a private copy of the kit with
# our screens in place of its demo designs.
#
# usage: tools/build-app.sh app         -> build/app/PPSA99688 (the app folder)
#        tools/build-app.sh snapshots   -> build/app-snapshots/*.png (PC preview;
#                                          needs GameVault running on :8687)
#
# Needs clang-18, lld-18, ninja, python3, git and curl; the PC preview also
# needs the Mesa EGL/GL development files. The kit fetches the PS5 payload SDK
# and the ps5-opengl SDK itself, pinned and checked.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-app}

KIT_REPO=${GV_KIT_REPO:-https://github.com/J7mbo/ps5-homebrew-ui.git}
KIT_COMMIT=0f5742db7d5b767f58a1eb62544f5e43b7c411d2
KIT=${GV_KIT_DIR:-$root/.deps/ps5-homebrew-ui}
WORK=$root/build/app-work
export USE_CCACHE=0 HOST_CC=${HOST_CC:-clang-18} HOST_CXX=${HOST_CXX:-clang++-18}

# ---- the kit, once ----
if [[ "$(cat "$KIT/.gv-commit" 2>/dev/null)" != "$KIT_COMMIT" ]]; then
    echo "==> [kit] Fetching ps5-homebrew-ui $KIT_COMMIT"
    rm -rf "$KIT"
    git init -q "$KIT"
    git -C "$KIT" fetch -q --depth 1 "$KIT_REPO" "$KIT_COMMIT"
    git -C "$KIT" checkout -q FETCH_HEAD
    (cd "$KIT" && bash tools/setup-native-dependencies.sh >/dev/null && bash tools/prepare-opengl.sh >/dev/null)
    echo "$KIT_COMMIT" > "$KIT/.gv-commit"
fi

# ---- a working copy with our screens ----
echo "==> [app] Preparing $WORK"
mkdir -p "$WORK"
(cd "$KIT" && tar --exclude=./.git --exclude=./.deps --exclude=./build --exclude=./dist -cf - .) | tar -xf - -C "$WORK"
rm -rf "$WORK/.deps" && ln -s "$KIT/.deps" "$WORK/.deps"
rm -rf "$WORK/src/concepts" && mkdir -p "$WORK/src/concepts"
cp -a "$root/app/screens/." "$WORK/src/concepts/"
cp -a "$root/app/overrides/." "$WORK/"
# Boot markers (see src/runtime/gv_boot_marker.c): how far a launch got.
python3 - "$WORK/src/main.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
marks = [
    ('int main()\n{\n', 'int main()\n{\n    gv_boot_mark("boot: main");\n'),
    ('    gfx::Renderer renderer;\n', '    gv_boot_mark("boot: display open");\n    gfx::Renderer renderer;\n'),
    ('    demo::Catalog catalog;\n', '    gv_boot_mark("boot: renderer and fonts ready");\n    demo::Catalog catalog;\n'),
    ('    app::Shell shell(', '    gv_boot_mark("boot: audio ready");\n    app::Shell shell('),
]
for old, new in marks:
    assert s.count(old) == 1, old
    s = s.replace(old, new)
s = s.replace('int main()\n', 'extern "C" void gv_boot_mark(const char *line);\n\nint main()\n', 1)
open(p, 'w').write(s)
PY
mkdir -p "$WORK/src/third_party/stb_image"
cp -p "$root/app/third_party/stb_image/stb_image.h" "$root/app/third_party/stb_image/LICENSE" "$WORK/src/third_party/stb_image/"
rm -rf "$WORK/sce_sys" && cp -a "$root/app/sce_sys" "$WORK/sce_sys"
# param.json's contentVersion follows VERSION (0.3.0 -> 00.003.000).
python3 - "$WORK/sce_sys/param.json" "$(cat "$root/VERSION")" <<'PY'
import json, sys
path, version = sys.argv[1], sys.argv[2]
a, b, c = (int(x) for x in version.strip().split("."))
data = json.load(open(path))
data["contentVersion"] = f"{a:02d}.{b:03d}.{c:03d}"
open(path, "w").write(json.dumps(data, indent=2) + "\n")
PY
# Only the Glass sound set; no demo music.
rm -f "$WORK"/assets/audio/music/*.ogg
rm -rf "$WORK/assets/audio/sfx/paper"

case "$mode" in
app)
    echo "==> [app] Building PPSA99688"
    rm -rf "$WORK/dist/PPSA99688"
    if ! make --no-print-directory -C "$WORK" app >"$WORK/build.log" 2>&1; then
        tail -60 "$WORK/build.log" >&2
        echo "app build failed (log: $WORK/build.log)" >&2
        exit 1
    fi
    rm -rf "$root/build/app" && mkdir -p "$root/build/app"
    cp -a "$WORK/dist/PPSA99688" "$root/build/app/PPSA99688"
    du -sh "$root/build/app/PPSA99688"
    ;;
snapshots)
    echo "==> [snapshots] Rendering the app with the PC preview"
    rm -rf "$root/build/app-snapshots"
    (cd "$WORK" && GV_SNAPSHOT=1 MESA_SHADER_CACHE_DISABLE=true bash tools/host-snapshots.sh "$root/build/app-snapshots" all)
    ;;
*)
    echo "usage: tools/build-app.sh app|snapshots" >&2
    exit 2
    ;;
esac
