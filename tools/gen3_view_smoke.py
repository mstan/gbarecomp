#!/usr/bin/env python3
"""Qualify paired native/adaptive Gen III runs using isolated, user-owned saves.

All writable data stays beneath --output. The source ROM/save is read only.
Use --prepare to boot a supplied SRAM save and capture a current-build field
checkpoint, or --state to reuse a checkpoint captured by this build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time

PROFILES = {
    'firered': (0x030030F4, 0x080565B4),
    'leafgreen': (0x030030F4, 0x080565B4),
    'ruby': (0x03001774, 0x080543C4),
    'sapphire': (0x03001774, 0x080543C8),
}
REGIONS = [('ewram',0x02000000,0x40000), ('iwram',0x03000000,0x8000),
           ('vram',0x06000000,0x18000), ('pal',0x05000000,0x400), ('oam',0x07000000,0x400)]

class Client:
    def __init__(self, port):
        self.socket=socket.create_connection(('127.0.0.1',port),timeout=120)
        self.file=self.socket.makefile('rb')
    def call(self,cmd,_allow_error=False,**kw):
        self.socket.sendall((json.dumps(dict(cmd=cmd,**kw))+'\n').encode())
        reply=json.loads(self.file.readline())
        if not reply.get('ok') and not _allow_error:raise RuntimeError(reply)
        return reply
    def read(self,region,base,size):
        return b''.join(bytes.fromhex(self.call('read_'+region,addr=hex(base+i),len=min(8192,size-i))['data']) for i in range(0,size,8192))

def selection(root,enabled,aspect):
    (root/'mods/state.toml').write_text(f'''format_version = 1
[[package]]
id = "pokemon-gen3.enhancement.widescreen"
version = "1.0.0"
[[feature]]
package_id = "pokemon-gen3.enhancement.widescreen"
id = "widescreen"
enabled = {str(enabled).lower()}
[feature.values]
aspect = "{aspect}"
''')

def launch(args,name,enabled):
    root=args.output/name;root.mkdir(parents=True,exist_ok=True)
    exe=root/args.exe.name;shutil.copy2(args.exe,exe)
    for dll in args.exe.parent.glob('*.dll'):shutil.copy2(dll,root/dll.name)
    shutil.copytree(args.catalog,root/'mods',dirs_exist_ok=True)
    selection(root,enabled,args.aspect)
    if args.save:shutil.copy2(args.save,root/'test.sav')
    env=os.environ.copy()
    for key in list(env):
        if key.startswith('GBARECOMP_'):del env[key]
    env.update(GBARECOMP_STRICT_STATIC='1',GBARECOMP_DIAGNOSTICS='0',
               RECOMP_RTC_EPOCH='1792800000',SDL_AUDIODRIVER='dummy',
               TEMP=str(args.output),TMP=str(args.output))
    # Disabled mod must suppress stale generic view requests.
    if not enabled:env['GBARECOMP_VIEW_WIDTH']='569'
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    log=(root/'run.log').open('w')
    process=subprocess.Popen([str(exe),'--bios',str(args.bios),'--rom',str(args.rom),
        '--save-path',str(root/'test.sav'),'--volume','0','--no-window','--tcp',str(port)],
        cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    try:
        deadline=time.monotonic()+90
        while True:
            if process.poll() is not None:raise RuntimeError(f'{name} exited {process.returncode}: {root}/run.log')
            try:
                client=Client(port)
                with args.rom.open('rb') as source:header=source.read(64)
                if bytes.fromhex(client.call('read_rom',addr='0x08000000',len=64)['data'])!=header:
                    raise AssertionError('debug ROM inspection disagrees with the verified input')
                bad=client.call('read_rom',_allow_error=True,addr=hex(0x08000000+args.rom.stat().st_size-1),len=2)
                if bad.get('ok'):raise AssertionError('debug ROM inspection accepted an invalid range')
                return root,process,log,client
            except OSError:
                if time.monotonic()>deadline:raise
                time.sleep(.1)
    except BaseException:
        process.terminate();process.wait(timeout=15);log.close();raise

def start_rectangle(client,game):
    """Find the game's published Start frame without using renderer output."""
    io=client.read('io',0x04000000,24)
    screen=((int.from_bytes(io[8:10],'little')>>8)&31)*0x800
    tiles=struct.unpack('<1024H',client.read('vram',0x06000000+screen,2048))
    if game in ('firered','leafgreen'):
        windows=client.read('ewram',0x020204B4,384)
        for at in range(0,384,12):
            bg,x,y,w,h,pal,base,data=struct.unpack_from('<6BHI',windows,at)
            if bg or not data or x<20 or y>2 or not w or not h or x+w>30 or y+h>20:continue
            if not any(tiles[(y+ty)*32+x+tx]==((pal<<12)|(base+ty*w+tx)) for ty in range(h) for tx in range(w)):continue
            return (max(0,x-1)*8,max(0,y-1)*8,min(30,x+w+1)*8,min(20,y+h+1)*8)
    else:
        base=int.from_bytes(client.read('iwram',0x030005AC,2),'little')&1023
        if base:
            for y in range(3):
                for x in range(20,29):
                    corner=tiles[y*32+x]
                    if corner&1023!=base:continue
                    for r in range(x+2,30):
                        if tiles[y*32+r]!=corner+2:continue
                        for b in range(y+2,20):
                            if tiles[b*32+x]==corner+6 and tiles[b*32+r]==corner+8:
                                return (x*8,y*8,(r+1)*8,(b+1)*8)
    return None

def prepare_outdoors(client,game,output):
    """Cheat-assisted renderer fixture; bypasses story/door prerequisites.

    Only the isolated native test instance is changed. The original game
    executes WarpIntoMap and CB2_LoadMap; no completion/story flags are set.
    This establishes loaded scene coverage, not a passed door/quest check.
    """
    frlg=game in ('firered','leafgreen')
    callback=PROFILES[game][0]
    groups={'firered':0x083526A8,'leafgreen':0x08352688,'ruby':0x083085A0,'sapphire':0x08308530}[game]
    group,number=(3,0) if frlg else (0,9)
    def read(addr,size):return client.read('rom',addr,size)
    def u32(addr):return int.from_bytes(read(addr,4),'little')
    header=u32(u32(groups+group*4)+number*4)
    layout=u32(header);w,h=struct.unpack('<II',read(layout,8));data=u32(layout+12)
    if not 0<w*h<=10240 or read(header+23,1)[0] not in (1,2,3):raise AssertionError('outdoor map metadata invalid')
    tiles=struct.unpack('<'+str(w*h)+'H',read(data,w*h*2))
    candidates=[(abs(x-w//2)+abs(y-h//2),x,y) for y in range(2,h-2) for x in range(2,w-2)
                if not tiles[y*w+x]&0xC00 and tiles[y*w+x]&0x3FF!=0x3FF]
    _,x,y=min(candidates)
    def write(addr,payload):client.call('write_mem',addr=hex(addr),hex=payload.hex())
    write(0x02031DBC if frlg else 0x020297F8,struct.pack('<bbbBhh',group,number,-1,0,x,y))
    client.call('set_keyinput',value=1023)
    write(callback,struct.pack('<I',(0x08055378 if frlg else 0x08053460)|1))
    client.call('step')
    loader=0x0805671C if frlg else 0x080544FC if game=='ruby' else 0x08054500
    write(callback,struct.pack('<I',loader|1))
    for _ in range(180):client.call('step')
    actual=int.from_bytes(client.read('iwram',callback,4),'little')&~1
    if actual!=PROFILES[game][1]:raise AssertionError('fixture loader did not return to overworld')
    return dict(method='cheat-assisted native map load',destination=[group,number],position=[x,y],
                bypassed='story and doorway prerequisites; no transition-success claim')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('exe','bios','rom','catalog','output'):
        p.add_argument('--'+key,type=lambda s:Path(s).resolve(),required=True)
    p.add_argument('--game',choices=list(PROFILES),required=True)
    p.add_argument('--save',type=lambda s:Path(s).resolve())
    p.add_argument('--state',type=lambda s:Path(s).resolve())
    p.add_argument('--prepare',action='store_true')
    p.add_argument('--outdoors',action='store_true',help='Use an isolated cheat-assisted outdoor renderer fixture')
    p.add_argument('--menu',action='store_true',help='Open Start and verify its actual anchored text pixels')
    p.add_argument('--frames',type=int,default=360)
    p.add_argument('--aspect',choices=['fit','16:9','21:9','32:9'],default='32:9')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    runs=[];report=dict(game=args.game,center_checks=0,memory_checks=0,nonblack_margin_samples=0,ui_checks=0)
    try:
        runs.append(launch(args,'native',False))
        c=runs[0][3]
        state=args.state
        if args.prepare:
            callback,expected=PROFILES[args.game]
            for frame in range(12000):
                key=1023
                if frame>120 and frame%80 in (0,1):key=1022
                if frame>240 and frame%240 in (0,1):key=1015
                c.call('set_keyinput',value=key);c.call('step')
                if frame%60==0 and frame>300:
                    cb=int.from_bytes(c.read('iwram',callback,4),'little')&~1
                    if cb==expected:
                        c.call('set_keyinput',value=1023)
                        for _ in range(30):c.call('step')
                        c.call('savestate_save',path=str(args.output/'indoor.state'))
                        if args.outdoors:report['fixture_route']=prepare_outdoors(c,args.game,args.output)
                        state=args.output/'field.state';c.call('savestate_save',path=str(state));break
            else:raise AssertionError('SRAM boot did not reach an overworld; field fixture required')
        if not state:raise ValueError('Provide --prepare with --save, or --state')
        c.call('savestate_load',path=str(state))
        if args.outdoors and not args.prepare:
            report['fixture_route']=prepare_outdoors(c,args.game,args.output)
            state=args.output/'field.state';c.call('savestate_save',path=str(state))
        runs.append(launch(args,'expanded',True));runs[1][3].call('savestate_load',path=str(state))
        clients=[r[3] for r in runs]
        width={'fit':284,'16:9':284,'21:9':373,'32:9':569}[args.aspect]
        # No interaction inputs in this route; UI is tested separately because
        # its intentionally relocated rectangles change native-center pixels.
        for frame in range(args.frames):
            key=(1015 if frame in (3,4) else 1023) if args.menu else 1023 if frame<30 else (991 if frame<90 else 1007 if frame<150 else 1023)
            for c in clients:c.call('set_keyinput',value=key);c.call('step')
            if frame>=2 and (frame%8==0 or frame==args.frames-1):
                shots=[c.call('screenshot') for c in clients]
                if [(s['w'],s['h']) for s in shots]!=[(240,160),(width,160)]:raise AssertionError('mod capability/size gate failed')
                native,wide=[bytes.fromhex(s['data']) for s in shots];left=(width-240)//2
                center=b''.join(wide[(y*width+left)*3:(y*width+left+240)*3] for y in range(160))
                rect=start_rectangle(clients[0],args.game) if args.menu else None
                if rect:
                    x1,y1,x2,y2=rect
                    different=any(a!=b and not (x1<=(i//3)%240<x2 and y1<=(i//3)//240<y2)
                                  for i,(a,b) in enumerate(zip(center,native)))
                else:different=center!=native
                if different:raise AssertionError(f'native center changed at frame {frame}')
                if args.menu and frame>=60:
                    if not rect:raise AssertionError('Start menu did not publish a visible frame')
                    # Compare opaque interior text/cursor pixels, avoiding the
                    # transparent frame corners which reveal different terrain.
                    x1+=16;x2-=16;y1+=16;y2-=16
                    right=width-240-left
                    source=b''.join(native[(y*240+x1)*3:(y*240+x2)*3] for y in range(y1,y2))
                    dest=b''.join(wide[(y*width+left+x1+right)*3:(y*width+left+x2+right)*3] for y in range(y1,y2))
                    if not source or source!=dest:
                        from PIL import Image
                        for name,shot,raw in zip(('native','expanded'),shots,(native,wide)):
                            Image.frombytes('RGB',(shot['w'],shot['h']),raw).save(args.output/f'{name}-failure.png')
                        raise AssertionError(f'Start menu text failed right anchor at frame {frame}: {rect}')
                    report['ui_checks']+=1
                report['center_checks']+=1
                report['nonblack_margin_samples']+=any(any(wide[(y*width)*3:(y*width+left)*3]) or
                    any(wide[(y*width+left+240)*3:((y+1)*width)*3]) for y in range(160))
                if frame==args.frames-1:
                    from PIL import Image
                    for name,shot,raw in zip(('native','expanded'),shots,(native,wide)):
                        Image.frombytes('RGB',(shot['w'],shot['h']),raw).save(args.output/f'{name}.png')
            if frame in (30,args.frames-1):
                for region,base,size in REGIONS:
                    a,b=[c.read(region,base,size) for c in clients]
                    if a!=b:raise AssertionError(f'guest {region} changed at frame {frame}')
                    report['memory_checks']+=1
                    if frame==args.frames-1:(runs[1][0]/f'{region}.bin').write_bytes(b)
        if not report['nonblack_margin_samples']:raise AssertionError('no authentic expanded scenery observed')
        report['coverage']=[c.call('misses') for c in clients]
        for coverage in report['coverage']:
            if any(coverage[k] for k in ('distinct_misses','interpreted_insns','healed_native')):raise AssertionError('strict execution failed')
        (runs[1][0]/'io.bin').write_bytes(clients[1].read('io',0x04000000,0x400))
        clients[1].call('savestate_save',path=str(args.output/'final.state'))
        report['state']=str(state);report['rom_sha1']=hashlib.sha1(args.rom.read_bytes()).hexdigest()
    finally:
        for root,process,log,c in runs:
            try:
                if process.poll() is None:c.call('quit');process.wait(timeout=30)
            finally:
                if process.poll() is None:process.terminate();process.wait(timeout=15)
                c.file.close();c.socket.close();log.close()
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':main()
