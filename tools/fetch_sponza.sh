#!/bin/sh
# Sponza (Khronos glTF-Sample-Assets, CC-BY 3.0 : Crytek, Frank Meinl) : géométrie seule.
set -e
mkdir -p assets/sponza
for f in Sponza.gltf Sponza.bin; do
  curl -sSfL -o "assets/sponza/$f" "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Sponza/glTF/$f"
done
echo "Sponza téléchargée dans assets/sponza"
