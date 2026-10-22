#!/bin/sh
# Sponza (Khronos glTF-Sample-Assets, CC-BY 3.0 : Crytek, Frank Meinl) : géométrie et textures.
set -e
mkdir -p assets/sponza
base="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Sponza/glTF"
for f in Sponza.gltf Sponza.bin; do
  curl -sSfL -o "assets/sponza/$f" "$base/$f"
done
# textures référencées par le glTF
for f in $(python -I -c "import json;print(' '.join(i['uri'] for i in json.load(open('assets/sponza/Sponza.gltf'))['images']))"); do
  [ -f "assets/sponza/$f" ] || curl -sSfL -o "assets/sponza/$f" "$base/$f"
done
echo "Sponza téléchargée dans assets/sponza"
