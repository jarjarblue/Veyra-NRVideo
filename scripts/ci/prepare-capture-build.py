"""Stage development headers/imports and the unchanged official AMD donor."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / 'out/capture-ci'
DEPS = STAGE / 'deps'
DOWNLOADS = STAGE / 'downloads'
for directory in (STAGE, DEPS, DOWNLOADS):
    directory.mkdir(parents=True, exist_ok=True)


def fetch(url, path, expected=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        request = urllib.request.Request(url, headers={'User-Agent': 'Veyra-capture-build'})
        with urllib.request.urlopen(request, timeout=120) as response, path.open('wb') as out:
            shutil.copyfileobj(response, out)
    if expected:
        actual = hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
        if actual.lower() != expected.lower():
            raise RuntimeError('SHA256 mismatch: ' + str(path))
    return path


def sdk(repository, commit, destination, select):
    tree_file = fetch(f'https://api.github.com/repos/{repository}/git/trees/{commit}?recursive=1',
                      DOWNLOADS / (destination + '-tree.json'))
    tree = json.loads(tree_file.read_text(encoding='utf-8'))
    if tree.get('truncated'):
        raise RuntimeError('Truncated dependency tree: ' + repository)
    files = [entry for entry in tree['tree'] if entry['type'] == 'blob' and select(entry['path'])]
    if not files:
        raise RuntimeError('No selected SDK files: ' + repository)
    def download(entry):
        path = entry['path']
        local = fetch(f'https://raw.githubusercontent.com/{repository}/{commit}/{path}', DEPS / destination / path)
        contents = local.read_bytes()
        actual = hashlib.sha1(b'blob ' + str(len(contents)).encode() + b'\0' + contents).hexdigest()
        if actual != entry['sha']:
            raise RuntimeError('Git blob mismatch: ' + repository + '/' + path)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(download, files))
    print(f'SDK {repository}@{commit}: {len(files)} verified files', flush=True)
    return DEPS / destination


def main():
    donor_zip = fetch(
        'https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.1.9/Veyra-2.1.9-AMD-win64-portable.zip',
        DOWNLOADS / 'AMD-2.1.9.zip',
        'f7113db09eff74065cfd9b7044df8db3c301ac5b7ff80f692da56fee2c2bf377')
    donor_parent = STAGE / 'donor'
    with zipfile.ZipFile(donor_zip) as archive:
        archive.extractall(donor_parent)
    donor = donor_parent / 'Veyra-2.1.9-AMD-win64-portable'
    assert (donor / 'veyra_qml_ui.exe').is_file()
    print('Official AMD 2.1.9 donor verified', flush=True)

    ffmpeg_zip = fetch(
        'https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.0.7-sources/FFmpeg-9.0.1-slices256-dav1d-zlib-source.zip',
        DOWNLOADS / 'ffmpeg-source.zip',
        '49db22049f072bd5448faa664aa736074dccfe6e72b4a933f673d8ae954b0bb6')
    ffmpeg = DEPS / 'ffmpeg'
    with zipfile.ZipFile(ffmpeg_zip) as archive:
        for info in archive.infolist():
            if info.filename.startswith('ffmpeg-patched/lib') and info.filename.endswith('.h'):
                relative = Path(info.filename).relative_to('ffmpeg-patched')
                target = ffmpeg / 'include' / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(info))
    # The public installed ABI configuration for Windows x64, not internal
    # FFmpeg build configuration. The runtime DLLs remain the exact donor.
    util = ffmpeg / 'include/libavutil'
    (util / 'avconfig.h').write_text('#pragma once\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n')
    (util / 'ffversion.h').write_text('#define FFMPEG_VERSION "9.0.1"\n')
    libraries = []
    for name in ('avformat', 'avcodec', 'avutil', 'swscale', 'swresample', 'avfilter'):
        matches = list(donor.glob(name + '-*.dll'))
        if len(matches) != 1:
            raise RuntimeError('Expected exactly one donor DLL: ' + name)
        dll = matches[0]
        exported = subprocess.check_output(['dumpbin', '/nologo', '/exports', str(dll)], text=True, encoding='utf-8', errors='replace')
        symbols = re.findall(r'^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+([^\s=]+)', exported, re.MULTILINE)
        if not symbols:
            raise RuntimeError('No DLL exports: ' + str(dll))
        imports = ffmpeg / 'lib'
        imports.mkdir(exist_ok=True)
        definition = imports / (name + '.def')
        definition.write_text('LIBRARY ' + dll.name + '\nEXPORTS\n' + '\n'.join(symbols) + '\n')
        library = imports / (name + '.lib')
        subprocess.run(['lib', '/nologo', '/machine:x64', '/def:' + str(definition), '/out:' + str(library)], check=True, timeout=120)
        libraries.append(library.as_posix())
    find = ffmpeg / 'share/ffmpeg/FindFFMPEG.cmake'
    find.parent.mkdir(parents=True, exist_ok=True)
    find.write_text('set(FFMPEG_FOUND TRUE)\nset(FFMPEG_INCLUDE_DIRS "' + (ffmpeg / 'include').as_posix() + '")\n'
                    + 'set(FFMPEG_LIBRARY_DIRS "' + (ffmpeg / 'lib').as_posix() + '")\n'
                    + 'set(FFMPEG_LIBRARIES "' + ';'.join(libraries) + '")\n')

    ngx = sdk('NVIDIA/DLSS', '9981c6350dc58c0ce21f024fd65e31e851c31689', 'ngx',
              lambda p: p.startswith('include/') or p == 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib' or p.lower().startswith('license'))
    of = sdk('SlimRG/NvidiaOF5', '99e8d8eb8f269591e5e4bd5a7f3c248bbbc3a3b9', 'nvof',
             lambda p: p.endswith(('nvOpticalFlowCommon.h', 'nvOpticalFlowD3D12.h')) or p.lower().startswith('license'))
    for header in ('nvOpticalFlowCommon.h', 'nvOpticalFlowD3D12.h'):
        path = next(of.rglob(header))
        target = of / 'NvOFInterface' / header
        target.parent.mkdir(exist_ok=True)
        if path != target:
            shutil.copyfile(path, target)
    xess = sdk('intel/xess', '5ec517a49a1ba8c4ad640361e4eb5227bf0742a5', 'xess',
               lambda p: p.startswith('inc/') or p.lower().startswith('license'))
    nvenc = sdk('FFmpeg/nv-codec-headers', '57f8cc0bb68e5f16f3787ea92cea59000f7bf97f', 'nvenc',
                lambda p: p.startswith('include/') or p.lower().startswith('license'))
    amf = sdk('GPUOpen-LibrariesAndSDKs/AMF', '57ee0cfb0e1a08d9041f77435653794c1b012be0', 'amf',
              lambda p: p.startswith('amf/public/include/') or p.lower().startswith('license'))
    amf_root = DEPS / 'amf-layout'
    shutil.copytree(amf / 'amf/public/include', amf_root / 'include', dirs_exist_ok=True)
    fsr = sdk('GPUOpen-LibrariesAndSDKs/FidelityFX-SDK', '60f4ea81909200d8542eca14dccb2628b763a9a3', 'fsr',
              lambda p: any(p.startswith('Kits/FidelityFX/' + part + '/include/') for part in ('api', 'framegeneration', 'upscalers')) or p.lower().startswith('license'))

    options = {
        'CMAKE_BUILD_TYPE': 'Release',
        'VEYRA_DISPLAY_VERSION': '2.1.9-capture-latency-test',
        'VEYRA_BUILD_QML_UI': 'ON', 'VEYRA_ENABLE_D3D12_DEBUG': 'OFF',
        'VEYRA_ENABLE_EXPERIMENTAL_DLSSNR': 'ON', 'VEYRA_ENABLE_DLSS_SR': 'ON',
        'VEYRA_ENABLE_DLSS_FG': 'ON', 'VEYRA_ENABLE_NVOF': 'ON',
        'VEYRA_DLSS_SDK_ROOT': ngx.as_posix(), 'VEYRA_NVOF_SDK_ROOT': of.as_posix(),
        'VEYRA_XESS_ROOT': xess.as_posix(), 'VEYRA_FFMPEG_ROOT': ffmpeg.as_posix(),
        'VEYRA_NVENC_HEADERS_ROOT': nvenc.as_posix(), 'VEYRA_AMF_SDK_ROOT': amf_root.as_posix(),
        'VEYRA_FIDELITYFX2_ROOT': (fsr / 'Kits/FidelityFX').as_posix(),
        'Python3_EXECUTABLE': os.sys.executable,
    }
    (STAGE / 'options.json').write_text(json.dumps(options, indent=2))
    command = ['cmake', '-S', str(ROOT), '-B', str(STAGE / 'build'), '-G', 'Ninja']
    command += ['-D' + key + '=' + str(value) for key, value in options.items()]
    subprocess.run(command, check=True, timeout=300)


if __name__ == '__main__':
    main()
