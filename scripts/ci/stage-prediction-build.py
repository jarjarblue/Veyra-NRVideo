"""Package the new EXE with verified upstream AMD runtime files."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / 'out/capture-ci'
BUILD = STAGE / 'build'
DONOR = STAGE / 'donor/Veyra-2.1.9-AMD-win64-portable'
PACKAGE = STAGE / 'Veyra-2.1.9-AMD-GPU-Capture-Prediction'


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    shutil.copytree(DONOR, PACKAGE, dirs_exist_ok=True)
    executable = BUILD / 'veyra_qml_ui.exe'
    if not executable.is_file():
        raise RuntimeError('No compiled player executable')
    shutil.copyfile(executable, PACKAGE / executable.name)
    shutil.copytree(BUILD / 'qml', PACKAGE / 'qml', dirs_exist_ok=True)
    shaders = list((BUILD / 'shaders').rglob('*.dxil'))
    for source in shaders:
        target = PACKAGE / 'shaders' / source.relative_to(BUILD / 'shaders')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        assert sha(source) == sha(target)
    original_manifest = PACKAGE / 'package-manifest.json'
    if original_manifest.exists():
        original_manifest.rename(PACKAGE / 'UPSTREAM_PACKAGE_MANIFEST.json')
    notes = '''Veyra 2.1.9 - GPU capture prediction TEST build

This is an unofficial test build, not an upstream Veyra release.
Use a separate folder and retain your original version.

In the Frame Generation tab enable GPU capture prediction. Start at 8 ms,
120 Hz, 33 ms horizon and HUD protection. This selects native presentation
and disables interpolation. Reopen capture to apply minimum allocator mode.
No future-frame buffering is added. Card/driver hardware buffers cannot be
removed by this feature. Unknown future input/geometry cannot be recovered.

For the separate XeSS 4X mode, leave AMD Anti-Lag OFF.
XeLL remains enabled. The new XeSS lower-latency fallback option is
experimental and defaults OFF; it only affects the backup MFG pacer.
It cannot change controller response or rendering inside the PS5.
No Radeon hardware or physical latency benefit was tested in CI.

This build preserves upstream runtime files and the capture/XeSS paths.
Optional Remote Play, Moonlight, Xbox, external DeckLink SDK, static
libass and the proprietary AMD optical-flow SDK are not compiled into
this capture-focused test executable. Keep the official release for
those optional features. Built-in GPU DIS optical flow remains.

Source: scripts/ci/latency-source.patch on the fork's build branch.
'''
    (PACKAGE / 'READ_ME_CAPTURE_TEST.txt').write_text(notes, encoding='utf-8')
    result = {
        'kind': 'unofficial capture-focused Windows test build',
        'source_commit': os.environ.get('GITHUB_SHA'),
        'source_patch_sha256': sha(ROOT / 'scripts/ci/capture-prediction-player.patch'),
        'executable_sha256': sha(PACKAGE / executable.name),
        'upstream_executable_sha256': sha(DONOR / executable.name),
        'compiled_shaders': len(shaders),
        'hardware_latency_tested': False,
        'driver_calls_tested': False,
        'donor_dlls_preserved': True,
    }
    smoke_environment = dict(os.environ, QT_QUICK_BACKEND='software')
    smoke = subprocess.run([str(PACKAGE / executable.name), '--data-dir', str(STAGE / 'smoke-profile'),
                            '--exit-after', '3000'], cwd=PACKAGE, env=smoke_environment,
                           timeout=60, capture_output=True, text=True, errors='replace')
    result['startup_smoke_exit'] = smoke.returncode
    (STAGE / 'startup-smoke.txt').write_text(smoke.stdout + smoke.stderr, encoding='utf-8')
    if smoke.returncode != 0:
        (STAGE / 'BUILD_RESULT.json').write_text(json.dumps(result, indent=2))
        raise RuntimeError('Packaged GUI startup smoke failed: ' + str(smoke.returncode))
    for original in DONOR.rglob('*.dll'):
        assert sha(original) == sha(PACKAGE / original.relative_to(DONOR))
    (STAGE / 'BUILD_RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    (PACKAGE / 'BUILD_RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    shutil.copyfile(STAGE / 'options.json', PACKAGE / 'BUILD_OPTIONS.json')
    # Capture the actual post-patch source bytes, including new files introduced
    # by git apply without --index. Never collect dependency/build directories.
    source_files = set(subprocess.check_output(['git','ls-files'], cwd=ROOT, text=True).splitlines())
    for patch_name in ('latency-source.patch','capture-prediction-cmake.patch','capture-prediction-player.patch'):
        delta = (ROOT / 'scripts/ci' / patch_name).read_text(encoding='utf-8')
        source_files.update(re.findall(r'^diff --git a/\S+ b/(.+)$', delta, re.MULTILINE))
    with zipfile.ZipFile(PACKAGE / 'source-snapshot.zip','w',zipfile.ZIP_DEFLATED,compresslevel=4) as source_zip:
        for name in sorted(source_files):
            file = ROOT / name
            if file.is_file(): source_zip.write(file, 'veyra-source/' + name)
        source_zip.writestr('veyra-source/README_BUILD_SOURCE.txt',
            'This is the actual already-patched build source. Do not apply the bundled patches again.\n'
            'On Windows x64 with MSVC 2022, Qt 6.8.3 and Ninja, run scripts/ci/prepare-capture-build.py,\n'
            'then cmake --build out/capture-ci/build --parallel 2 --target veyra_qml_ui.\n'
            'BUILD_OPTIONS.json and BUILD_RESULT.json record this build. Runtime dependencies are verified downloads.\n')
    # Corresponding source delta and full notices travel with the binary.
    shutil.copyfile(ROOT / 'scripts/ci/latency-source.patch', PACKAGE / 'latency-source.patch')
    for name in ('capture-prediction-cmake.patch','capture-prediction-player.patch'):
        shutil.copyfile(ROOT / 'scripts/ci' / name, PACKAGE / name)
    shutil.copyfile(ROOT / 'THIRD_PARTY_NOTICES.md', PACKAGE / 'THIRD_PARTY_NOTICES.md')
    license_target = PACKAGE / 'licenses/AMD-Anti-Lag-2-MIT.txt'
    license_target.parent.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / 'third_party/amd-anti-lag-2/LICENSE.txt', license_target)
    with zipfile.ZipFile(STAGE / 'package.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=4) as archive:
        for path in PACKAGE.rglob('*'):
            if path.is_file():
                archive.write(path, PACKAGE.name + '/' + path.relative_to(PACKAGE).as_posix())
    # Small compiled update for an untouched copy of the official AMD 2.1.9
    # portable package. Full portable package.zip above remains self-contained.
    with zipfile.ZipFile(STAGE / 'update.zip','w',zipfile.ZIP_DEFLATED,compresslevel=4) as update:
        selected = {PACKAGE / 'veyra_qml_ui.exe'}
        for folder in ('qml','shaders'):
            selected.update(path for path in (PACKAGE / folder).rglob('*') if path.is_file())
        for name in ('BUILD_RESULT.json','BUILD_OPTIONS.json','READ_ME_CAPTURE_TEST.txt','source-snapshot.zip',
                     'THIRD_PARTY_NOTICES.md','latency-source.patch','capture-prediction-cmake.patch','capture-prediction-player.patch'):
            selected.add(PACKAGE / name)
        selected.add(PACKAGE / 'licenses/AMD-Anti-Lag-2-MIT.txt')
        for file in sorted(selected): update.write(file, file.relative_to(PACKAGE).as_posix())
        update.writestr('INSTALL_PREDICTION_UPDATE.txt',
            'Compiled player update, not merely source code.\n'
            'Close Veyra. Make a NEW copy of the original Veyra 2.1.9 AMD portable folder.\n'
            'Extract this update INTO that copied folder, replacing EXE/QML/shaders when asked.\n'
            'Run veyra_qml_ui.exe. In the Frame Generation tab, enable GPU capture prediction.\n'
            'Reopen capture to apply the minimum device allocator. Start at 8 ms, 120 Hz and HUD protection on.\n'
            'Do not layer this over another Veyra version. Full portable package is also available from the build.\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    main()
