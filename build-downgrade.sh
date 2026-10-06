#!/bin/bash

echo "=== DEBUG ICON ==="
echo "PWD: $(pwd)"
echo "APP_TITLE: ${APP_TITLE:-<unset>}"
echo "ICON: ${ICON:-<unset>}"
echo "APP_ICON: ${APP_ICON:-<unset>}"
find /app/romfs -maxdepth 3 -type f -print
ls -lah /app/romfs/icon/ || true
echo "=================="

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

filename="odyssey_downgrade"

export ICON="romfs/icon/odyssey_downgrade.jpg"
export APP_TITLE="Odyssey Downgrade"
export APP_AUTHOR="Shadów,Skylanderfree"

# Clean-up from last build
rm -rf ./out/
mkdir -p ./out/

if [ -f ./source/main.cpp ] ; then
  mv ./source/main.cpp ./main.cpp
fi

make clean_all

# Build odyssey_downgrade.nro
f="./$filename.c"

rm -f ./source/main.c
cp $f ./source/main.c

make BUILD_TYPE="$filename" -j$(nproc)

cp ./$filename.nro ./out/$filename.nro
cp ./$filename.elf ./out/$filename.elf

make BUILD_TYPE="$filename" clean

# Post build clean-up
make clean_all

# Final clean-up
rm -f ./source/main.c
mv ./main.cpp ./source/main.cpp
