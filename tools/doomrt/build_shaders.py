"""Compile les shaders de RTGL1 (branche krc de F:/RayTracedGL1) avec glslangValidator.

RTGL1 utilise glslc (Vulkan SDK), absent ici : glslangValidator (compilé avec Quake II RTX) le
remplace ; il lui faut l'extension GL_GOOGLE_include_directive, ajoutée à une copie de chaque
shader.

    python -I tools/doomrt/build_shaders.py [--out F:/doom-rt-krc/ovrd/shaders] [fichier ...]
"""
import argparse
import os
import subprocess
import sys

RTGL = 'F:/RayTracedGL1'
SRC = os.path.join(RTGL, 'Source', 'Shaders')
GEN = os.path.join(RTGL, 'Source', 'Generated')
GLSLANG = 'F:/Q2RTX-KRC/build/Bin/glslangValidator.exe'
EXTS = ('.comp', '.rgen', '.rchit', '.rmiss', '.rahit', '.vert', '.frag')


def include_dirs():
    dirs = [SRC, GEN]
    for root, _, _ in os.walk(SRC):
        if root != SRC:
            dirs.append(root)
    return dirs


def compile_one(path, out_dir, tmp_dir):
    name = os.path.basename(path)
    with open(path, encoding='utf8') as f:
        lines = f.read().split('\n')
    # extension d'inclusion juste après #version
    for i, l in enumerate(lines):
        if l.strip().startswith('#version'):
            lines.insert(i + 1, '#extension GL_GOOGLE_include_directive : require')
            break
    tmp = os.path.join(tmp_dir, name)
    with open(tmp, 'w', encoding='utf8') as f:
        f.write('\n'.join(lines))
    args = [GLSLANG, '-V', '--target-env', 'vulkan1.2', '--quiet']
    for d in include_dirs():
        args += ['-I' + d]
    args += [tmp, '-o', os.path.join(out_dir, name + '.spv')]
    r = subprocess.run(args, capture_output=True, text=True)
    msg = (r.stdout + r.stderr).strip()
    return r.returncode == 0, msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='F:/doom-rt-krc/ovrd/shaders')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = os.path.join(RTGL, 'build', 'shader_tmp')
    os.makedirs(tmp, exist_ok=True)
    files = a.files or sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(EXTS))
    errors = 0
    for f in files:
        ok, msg = compile_one(f, a.out, tmp)
        if not ok:
            errors += 1
            print(f'ÉCHEC {os.path.basename(f)}\n{msg}\n', flush=True)
    print(f'{len(files) - errors}/{len(files)} shaders compilés')
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
