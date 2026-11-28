#!/usr/bin/env python3
"""Package a built title with UI, own artwork and catalog; never publishes."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, sys, tarfile, urllib.request, zipfile
import re
sys.path.insert(0, str(Path(__file__).resolve().parent))
from consumer_build import pinned_source

ROOT = None
META = None

def run(*args,**kwargs): subprocess.run(list(map(str,args)),check=True,**kwargs)
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def stage_windows_dependencies(stage, mingw):
    """Follow imports so optional UI/rendering libraries are packaged completely."""
    pending = [*stage.glob('*.exe'), *stage.glob('*.dll')]
    seen = set()
    while pending:
        binary = pending.pop()
        if binary.name.lower() in seen: continue
        seen.add(binary.name.lower())
        imports = subprocess.check_output([str(mingw/'bin/objdump.exe'), '-p', str(binary)],
                                          text=True, errors='replace')
        for name in re.findall(r'DLL Name:\s*(\S+)', imports):
            local, source = stage/name, mingw/'bin'/name
            if not local.exists() and source.is_file(): shutil.copy2(source, local)
            if local.is_file(): pending.append(local)

def main():
    global ROOT, META
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--project-root',type=Path,required=True)
    p.add_argument('--engine',type=Path)
    p.add_argument('--ui',type=Path)
    p.add_argument('--mingw-root',type=Path,default=Path(os.environ.get('MINGW_PREFIX','C:/msys64/mingw64')))
    p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--variant')
    p.add_argument('--out-dir',type=Path,help='Fresh packaging destination (default: release-stage)')
    a=p.parse_args(); ROOT=a.project_root.resolve(); META=json.loads((ROOT/'release.json').read_text())
    a.variant=a.variant or META['default_variant']
    if a.variant not in META['variants']: p.error('Unknown title variant: '+a.variant)
    build=a.build_dir.resolve()
    for module, source in (('gbarecomp', a.engine or ROOT/'gbarecomp'),
                           ('recomp-ui', a.ui or ROOT/'recomp-ui')):
        pinned_source(ROOT, module, source)
    target=META['target']; windows=os.name=='nt'
    name=f'{target}-{a.variant}-{"windows" if windows else "linux"}-x64-v{META["version"]}'
    out=(a.out_dir or ROOT/'release-stage').resolve(); out.mkdir(parents=True,exist_ok=True)
    # Fail on existing staging instead of deleting a user's directory.
    stage=out/name; stage.mkdir()
    exe=target+('.exe' if windows else '')
    shutil.copy2(build/exe,stage/exe)
    shutil.copytree(build/'assets',stage/'assets')
    shutil.copytree(ROOT/'mods/preloaded',stage/'mods')
    shutil.copytree(ROOT/'docs',stage/'docs')
    shutil.copytree(ROOT/'recomp/launcher',stage/'recomp/launcher')
    for file in ('README.md','LICENSE', 'PERMISSIONS.md','baserom.md'):
        shutil.copy2(ROOT/file,stage/file)
    config=META['variants'][a.variant]['config']
    shutil.copy2(ROOT/config,stage/config)
    required=stage/'mods/packages'/META['package']/'1.0.0/manifest.toml'
    if not required.is_file(): raise SystemExit('Missing game-owned mod catalog')
    if (stage/'assets/img/boxart.tga').read_bytes()!=(ROOT/'recomp/launcher/boxart.tga').read_bytes():
        raise SystemExit('Wrong title boxart staged')
    if windows:
        for dll in ('SDL2.dll','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll','zlib1.dll','libEGL.dll','libGLESv2.dll'):
            shutil.copy2(a.mingw_root/'bin'/dll,stage/dll)
        stage_windows_dependencies(stage, a.mingw_root)
    notices=[sys.executable,Path(__file__).with_name('collect_licenses.py'),a.engine or ROOT/'gbarecomp',a.ui or ROOT/'recomp-ui',stage/'licenses','--title-root',ROOT,'--build-dir',build,'--shipped-dir',stage]
    if windows: notices+=['--mingw',a.mingw_root]
    run(*notices)
    # Opt-in source catalog; the owner QA copy is pre-enabled separately.
    for file in stage.rglob('*'):
        if file.is_file() and (file.suffix.lower() in ('.gba','.bin','.sav','.cap') or
           file.name in ('rom.cfg','rom-usa.cfg','bios.cfg','state.toml') or
           'psx.enhancement.cd-speed' in str(file) or 'psx.enhancement.fast-loading' in str(file)):
            raise SystemExit(f'Forbidden private data/loading package: {file}')
    provenance=dict(repository=META['repository'],version=META['version'],variant=a.variant,
        source=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        source_dirty=bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=ROOT,text=True).strip()),
        pins={mod:subprocess.check_output(['git','ls-tree','HEAD',mod],cwd=ROOT,text=True).split()[2] for mod in ('gbarecomp','recomp-ui')},
        executable_sha256=sha(stage/exe))
    (stage/'build-info.json').write_text(json.dumps(provenance,indent=2)+'\n')
    if windows:
        archive=out/(name+'.zip')
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
            for file in stage.rglob('*'):
                if file.is_file(): z.write(file,Path(name)/file.relative_to(stage))
        with zipfile.ZipFile(archive) as z:
            if z.testzip(): raise SystemExit('Archive CRC failure')
    else:
        # Portable directory archive as well as the launchable AppImage.
        archive=out/(name+'.tar.gz')
        with tarfile.open(archive,'w:gz') as t: t.add(stage,arcname=name)
        appimage(stage,out/name)
    (out/(name+'.sha256')).write_text(sha(archive)+'  '+archive.name+'\n')
    print('PACKAGED',archive,sha(archive),flush=True)

def tool(name,expected):
    cache=Path(os.environ.get('XDG_CACHE_HOME',Path.home()/'.cache'))/'recomp-appimage-tools'
    cache=cache/expected[:12]
    cache.mkdir(parents=True,exist_ok=True)
    dest=cache/name
    if not dest.exists():
        project='linuxdeploy/linuxdeploy' if name.startswith('linuxdeploy') else 'AppImage/appimagetool'
        urllib.request.urlretrieve(f'https://github.com/{project}/releases/download/continuous/{name}',dest)
    if sha(dest)!=expected: raise SystemExit(f'Packaging tool hash mismatch: {dest}')
    dest.chmod(0o755); return dest

def appimage(stage,prefix):
    target=META['target']; app=Path(str(prefix)+'.AppDir'); app.mkdir()
    usr=app/'usr/bin'; usr.mkdir(parents=True)
    for path in stage.iterdir():
        if path.is_dir(): shutil.copytree(path,usr/path.name)
        else: shutil.copy2(path,usr/path.name)
    icon=app/(target+'.png'); shutil.copy2(ROOT/'recomp/launcher/boxart.png',icon)
    desktop=app/(target+'.desktop')
    desktop.write_text(f'[Desktop Entry]\nType=Application\nName={META["name"]} Recompiled\nExec={target}\nIcon={target}\nCategories=Game;\nTerminal=false\n')
    env=dict(os.environ,APPIMAGE_EXTRACT_AND_RUN='1',ARCH='x86_64',NO_STRIP='1')
    deploy=tool('linuxdeploy-x86_64.AppImage','8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1')
    run(deploy,'--appdir',app,'--executable',usr/target,'--desktop-file',desktop,'--icon-file',icon,env=env)
    # Same writable per-title home policy as the existing Mega Man Zero releases.
    # Symlink invocation keeps argv[0] at the writable home; settings never land
    # in the read-only AppImage mount. User mods/settings are not overwritten.
    apprun=app/'AppRun'
    if apprun.is_symlink(): apprun.unlink()
    apprun.write_text('''#!/bin/sh
set -eu
HERE="$(dirname "$(readlink -f "$0")")"
DATA="${'''+target.upper()+'''_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/'''+target+'''}"
mkdir -p "$DATA/mods/packages"
ln -sfn "$HERE/usr/bin/'''+target+'''" "$DATA/'''+target+'''"
ln -sfn "$HERE/usr/bin/assets" "$DATA/assets"
cp -R "$HERE/usr/bin/mods/packages/." "$DATA/mods/packages/"
cp -n "$HERE/usr/bin/'''+META['variants'][next(variant for variant in META['variants'] if f'-{variant}-' in stage.name)]['config']+'''" "$DATA/" 2>/dev/null || true
export RECOMP_HOST_LD_LIBRARY_PATH="${LD_LIBRARY_PATH-}"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$DATA"
exec "$DATA/'''+target+'''" "$@"
''')
    apprun.chmod(0o755)
    pack=tool('appimagetool-x86_64.AppImage','95cbe7cce9717fce90c484e34052ee7c7f1d7635b33c12525b4776826a7d29b6')
    run(pack,'--no-appstream','--mksquashfs-opt','-processors','--mksquashfs-opt','4',app,str(prefix)+'.AppImage',env=env)
    result=Path(str(prefix)+'.AppImage')
    result.chmod(0o755)
    Path(str(result)+'.sha256').write_text(sha(result)+'  '+result.name+'\n')

if __name__=='__main__': main()
