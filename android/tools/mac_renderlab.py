#!/usr/bin/env python3
"""Run focused Z-A Vulkan checks on macOS. Does not connect to a device.

Uses the installed Chrome SwiftShader/loader read-only, or --vulkan-dir.
Needs clang, glslangValidator, spirv-val, spirv-cross and Android NDK headers.
Game shader files stay local; no game or save files are modified.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ANDROID = Path(__file__).resolve().parents[1]
PROJECT = ANDROID.parents[1]


def chrome_vulkan():
    root = Path('/Applications/Google Chrome.app/Contents/Frameworks/Google Chrome Framework.framework/Versions')
    candidates = sorted(root.glob('*/Libraries'),
                        key=lambda p: tuple(int(n) for n in re.findall(r'\d+', p.parent.name)), reverse=True)
    for path in candidates:
        if all((path / n).is_file() for n in ('libvulkan.dylib', 'libvk_swiftshader.dylib', 'vk_swiftshader_icd.json')):
            return path
    raise RuntimeError('No local SwiftShader found; supply --vulkan-dir')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan-dir', type=Path)
    parser.add_argument('--shader', type=Path, default=PROJECT / 'Test/20261027_audio_followup/shaders/681cc42e48c0bbc0fe93220107f30873-361950aad0c6.vks')
    parser.add_argument('--output', type=Path, default=PROJECT / 'Test' / (datetime.now().strftime('%Y%m%d_%H%M%S') + '_mac_renderlab'))
    args = parser.parse_args()
    for tool in ('clang', 'glslangValidator', 'spirv-val', 'spirv-cross'):
        if not shutil.which(tool):
            raise RuntimeError(f'Missing tool: {tool}')
    shader = args.shader.resolve(strict=True)
    vulkan = (args.vulkan_dir or chrome_vulkan()).resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    ndk = Path(os.environ.get('ANDROID_NDK_HOME', '/opt/homebrew/share/android-ndk'))
    headers = next((ndk / 'toolchains/llvm/prebuilt').glob('darwin-*/sysroot/usr/include'))
    include = output / 'include'
    include.mkdir(exist_ok=True)
    for name in ('vulkan', 'vk_video'):
        target = include / name
        if not target.exists():
            target.symlink_to(headers / name, target_is_directory=True)
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = str(vulkan)
    env['VK_ICD_FILENAMES'] = str(vulkan / 'vk_swiftshader_icd.json')
    # Do not let a machine-wide implicit layer/driver alter the reference run.
    env.pop('VK_DRIVER_FILES', None)
    results = []

    def run(label, command, expected_failure=False):
        proc = subprocess.run([str(x) for x in command], env=env, capture_output=True, text=True)
        log = proc.stdout + proc.stderr
        (output / (label + '.log')).write_text(log)
        passed = (proc.returncode != 0 and 'FAIL on ' in log) if expected_failure else proc.returncode == 0
        if not passed:
            raise RuntimeError(f'{label} failed ({proc.returncode}):\n{log}')
        if expected_failure:
            print(f'{label}: PASS (deliberately broken positions rejected)', flush=True)
        elif log.strip():
            print(log.strip(), flush=True)
        results.append({'check': label, 'expected_failure': expected_failure, 'returncode': proc.returncode})

    flags = ['-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-I', include]

    def build_gpu(name, source):
        run(name + '-build', ['clang', *flags, source, vulkan / 'libvulkan.dylib', '-o', output / name])

    build_gpu('storage-merge', ANDROID / 'tests/storage_merge_probe.c')
    run('storage-merge', [output / 'storage-merge'])
    compute = output / 'skinning.comp'
    run('wrap-compute', [sys.executable, ANDROID / 'tools/skin_compute_probe.py', shader, compute])
    run('compile-compute', ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', str(compute) + '.spv', compute])
    run('validate-compute', ['spirv-val', '--target-env', 'vulkan1.1', str(compute) + '.spv'])
    build_gpu('skinning-compute', ANDROID / 'tests/skinning_compute_probe.c')
    run('skinning-compute', [output / 'skinning-compute', str(compute) + '.spv'])
    vertex = output / 'skinning.vert'
    run('wrap-raster', [sys.executable, ANDROID / 'tools/skin_compute_probe.py', shader, vertex, '--stage', 'raster'])
    run('compile-vertex', ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', str(vertex) + '.spv', vertex])
    fragment = output / 'position.frag.spv'
    run('compile-fragment', ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', fragment, ANDROID / 'tests/shaders/skinning_position.frag'])
    run('validate-vertex', ['spirv-val', '--target-env', 'vulkan1.1', str(vertex) + '.spv'])
    build_gpu('skinning-raster', ANDROID / 'tests/skinning_raster_probe.c')
    run('skinning-raster', [output / 'skinning-raster', str(vertex) + '.spv', fragment])

    # Confirm that the reference comparison actually detects bad positions.
    broken = output / 'broken.comp'
    text = compute.read_text()
    text, n = re.subn(r'output_vertices\[i\]\.position = .*?;',
                      'output_vertices[i].position = vec4(0.0, 0.0, 0.0, 1.0);', text)
    if n != 1:
        raise RuntimeError('Could not insert the negative control')
    broken.write_text(text)
    run('compile-negative-control', ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', str(broken) + '.spv', broken])
    run('negative-control', [output / 'skinning-compute', str(broken) + '.spv'], expected_failure=True)

    # Production renderer code: submit-time capture must survive CPU rewrites.
    gfx = ANDROID.parent / 'games/totk/third_party/d2r_gfx/gfx'
    hostflags = ['-DVK_NO_PROTOTYPES', '-DVK_USE_PLATFORM_ANDROID_KHR', '-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip']
    for path in (ANDROID / 'build', gfx / 'nvn/include', gfx / 'nvn', gfx / 'tex'):
        hostflags += ['-I', path]
    run('ubo-capture-build', ['clang', *flags, *hostflags, ANDROID / 'tests/ubo_capture_test.c', '-o', output / 'ubo-capture'])
    run('ubo-capture', [output / 'ubo-capture'])
    summary = {'shader': str(shader), 'shader_sha256': hashlib.sha256(shader.read_bytes()).hexdigest(),
               'vulkan_directory': str(vulkan), 'checks': results,
               'limits': ['Synthetic fixtures, not a captured failing frame',
                          'SPIRV-Cross and glslang recompile the original shader arithmetic',
                          'No Android replay, native transform feedback, Adreno or Turnip execution']}
    (output / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'Report: {output}', flush=True)


if __name__ == '__main__':
    main()
