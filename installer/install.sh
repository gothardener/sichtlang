#!/usr/bin/env sh
set -eu

SOURCE_ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
INSTALL_DIR="${SICHT_INSTALL_DIR:-$HOME/.local/share/sicht}"
BIN_LINK_DIR="${SICHT_BIN_DIR:-$HOME/.local/bin}"

echo "Installing Sicht from: $SOURCE_ROOT"
echo "Install dir: $INSTALL_DIR"

if [ ! -f "$SOURCE_ROOT/sicht" ]; then
  echo "Missing required binary: $SOURCE_ROOT/sicht"
  exit 1
fi

mkdir -p "$INSTALL_DIR"
mkdir -p "$BIN_LINK_DIR"

cp "$SOURCE_ROOT/sicht" "$INSTALL_DIR/sicht"
chmod +x "$INSTALL_DIR/sicht"

if [ -d "$SOURCE_ROOT/libs" ]; then
  rm -rf "$INSTALL_DIR/libs"
  cp -R "$SOURCE_ROOT/libs" "$INSTALL_DIR/libs"
fi

if [ -d "$SOURCE_ROOT/examples" ]; then
  rm -rf "$INSTALL_DIR/examples"
  cp -R "$SOURCE_ROOT/examples" "$INSTALL_DIR/examples"
fi

if [ -d "$SOURCE_ROOT/docs" ]; then
  rm -rf "$INSTALL_DIR/docs"
  cp -R "$SOURCE_ROOT/docs" "$INSTALL_DIR/docs"
fi

for runtime_dir in src headers debug; do
  if [ -d "$SOURCE_ROOT/$runtime_dir" ]; then
    rm -rf "$INSTALL_DIR/$runtime_dir"
    cp -R "$SOURCE_ROOT/$runtime_dir" "$INSTALL_DIR/$runtime_dir"
  fi
done

if [ -f "$SOURCE_ROOT/README.linux.md" ]; then
  cp "$SOURCE_ROOT/README.linux.md" "$INSTALL_DIR/README.md"
elif [ -f "$SOURCE_ROOT/README.md" ]; then
  cp "$SOURCE_ROOT/README.md" "$INSTALL_DIR/README.md"
fi

ln -sf "$INSTALL_DIR/sicht" "$BIN_LINK_DIR/sicht"

echo
echo "Sicht installed."
echo "Binary: $INSTALL_DIR/sicht"
echo "Symlink: $BIN_LINK_DIR/sicht"
echo
echo "If '$BIN_LINK_DIR' is not in PATH, add:"
echo "  export PATH=\"$BIN_LINK_DIR:\$PATH\""
