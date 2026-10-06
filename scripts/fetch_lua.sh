#!/bin/bash
set -euo pipefail

LUA_VER="5.4.7"
LUA_URL="https://www.lua.org/ftp/lua-${LUA_VER}.tar.gz"
DEST_DIR="third_party"

mkdir -p "$DEST_DIR"
cd "$DEST_DIR"

if [ -d "lua-${LUA_VER}" ]; then
  echo "Lua ${LUA_VER} already unpacked."
else
  echo "Downloading Lua ${LUA_VER}..."
  curl -L "$LUA_URL" -o "lua-${LUA_VER}.tar.gz"
  tar xzf "lua-${LUA_VER}.tar.gz"
fi

if [ -d "lua-5.4" ]; then
  echo "Updating symlink lua-5.4 -> lua-${LUA_VER}"
  rm -f lua-5.4
fi
ln -s "lua-${LUA_VER}" lua-5.4

echo "Done. Enable with: make ENABLE_LUA=1"




















































