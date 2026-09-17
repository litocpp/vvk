#!/usr/bin/env python3
"""Build headers-only probes, run vvk tests, and verify an isolated Linux runtime."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--headers-prefix', type=Path, default=Path('/usr'))
parser.add_argument('--compiler', default=shutil.which('clang++') or '/nix/opt/llvm/22/bin/clang++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
work = root / 'build/loader-validation'
source = work / 'source'
prefix = work / 'headers'
source.mkdir(parents=True, exist_ok=True)
for name in ['vulkan', 'vk_video']:
    shutil.copytree(args.headers_prefix / 'include' / name, prefix / 'include' / name, dirs_exist_ok=True)
package = args.headers_prefix / 'share/cmake/VulkanHeaders'
if not package.is_dir():
    raise SystemExit(f'VulkanHeaders package not found: {package}; set --headers-prefix')
shutil.copytree(package, prefix / 'share/cmake/VulkanHeaders', dirs_exist_ok=True)
for path in (root / 'tests/loader').iterdir():
    if path.is_file():
        shutil.copy2(path, source / path.name)
(source / 'CMakeUserPresets.json').write_text(json.dumps({
    'version': 6,
    'configurePresets': [{
        'name': 'headers-only', 'generator': 'Ninja', 'binaryDir': str(work / 'cmake'),
        'cacheVariables': {
            'CMAKE_CXX_COMPILER': args.compiler,
            'CMAKE_FIND_ROOT_PATH': str(prefix),
            'CMAKE_FIND_ROOT_PATH_MODE_PACKAGE': 'ONLY',
            'CMAKE_DISABLE_FIND_PACKAGE_Vulkan': True,
        },
    }],
    'buildPresets': [{'name': 'headers-only', 'configurePreset': 'headers-only'}],
}, indent=2))

def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(command, check=True, **kwargs)

run(['cmake', '--preset', 'headers-only'], cwd=source)
run(['cmake', '--build', '--preset', 'headers-only'], cwd=source)
run([str(work / 'cmake/headers-only')])
env = os.environ.copy()
env['VVK_TEST_LOADER'] = str(work / 'cmake/libvvk-test-loader.so')
env['VVK_TEST_MISSING_ROOT'] = str(work / 'cmake/libvvk-test-missing-root.so')
run(['lito', 'test', '-p', 'vvk', '--profile', 'debug', '-j6'], cwd=root, env=env)
binary = root / 'build/debug/test/vvk/vvk-memory-tests'
if not binary.is_file():
    raise SystemExit(f'Test binary not found at documented output path: {binary}')
needed = subprocess.check_output(['readelf', '-d', binary], text=True)
undefined = subprocess.check_output(['nm', '-u', root / 'build/debug/lib/vvk/libvvk.a'], text=True)
if re.search(r'libvulkan', needed, re.I) or re.search(r'\bU vk[A-Z]', undefined):
    raise SystemExit('Direct Vulkan loader dependency remains')
symbols = subprocess.check_output(['nm', '--defined-only', binary], text=True)
if not re.search(r'\b[Tt] vmaCreateAllocator$', symbols, re.M):
    raise SystemExit('VMA implementation was not linked into the test executable')
libs = subprocess.check_output(['ldd', binary], text=True)
if 'libvulkan' in libs:
    raise SystemExit('Transitive Vulkan loader dependency remains')
print('\n'.join(line for line in needed.splitlines() if 'NEEDED' in line), flush=True)
if not shutil.which('bwrap'):
    raise SystemExit('bwrap is required for the isolated no-loader check')
# Mount only the executable, its ELF dependencies and the two test libraries.
# System Vulkan libraries and ICDs are never mounted or modified.
runtime = work / 'runtime'
if runtime.exists():
    shutil.rmtree(runtime)
runtime.mkdir()
for name in ['dev', 'proc', 'tmp']:
    (runtime / name).mkdir()
for library in re.findall(r'(/[^\s()]+)', libs):
    path = Path(library)
    if path.is_file():
        target = runtime / str(path).lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
interpreter = subprocess.check_output(['readelf', '-l', binary], text=True)
match = re.search(r'Requesting program interpreter: ([^\]]+)', interpreter)
if match:
    path = Path(match.group(1))
    target = runtime / str(path).lstrip('/')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, target)
shutil.copy2(binary, runtime / 'test')
shutil.copy2(env['VVK_TEST_LOADER'], runtime / 'test-loader.so')
shutil.copy2(env['VVK_TEST_MISSING_ROOT'], runtime / 'missing-root.so')
run(['bwrap', '--ro-bind', str(runtime), '/', '--dev', '/dev', '--proc', '/proc',
     '--tmpfs', '/tmp', '--chdir', '/', '--clearenv',
     '--setenv', 'VVK_TEST_LOADER', '/test-loader.so',
     '--setenv', 'VVK_TEST_MISSING_ROOT', '/missing-root.so',
     '--setenv', 'VVK_TEST_EXPECT_NO_LOADER', '1',
     '/test', '--gtest_filter=-MemoryVulkan.*'])
print('Headers-only, VMA-linked ELF and isolated no-loader checks passed.')
