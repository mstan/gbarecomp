"""Host-local benchmark lease and measured headroom, Windows and Linux.

Never infer contention from executable names. Keep comparison/variation gates
in the workload driver; this module decides only whether a host can admit it.
"""
import argparse
import contextlib
import datetime
import glob
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import struct
import sys
import time


class Occupied(RuntimeError):
    pass


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def default_lock():
    # Preserve the shared Windows convention used by other console agents.
    return Path('G:/Temp/Codex/.benchmark.lock') if os.name == 'nt' else Path.home()/'.cache/codex-performance/benchmark.lock'


@contextlib.contextmanager
def lease(path=None):
    path = Path(path) if path else default_lock()
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise Occupied('Reservation path is a symlink; preserved')
    if os.name == 'nt':
        import ctypes as c
        import ctypes.wintypes as w
        k = c.WinDLL('kernel32', use_last_error=True)
        k.CreateMutexW.argtypes=[c.c_void_p,w.BOOL,w.LPCWSTR]; k.CreateMutexW.restype=w.HANDLE
        k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD]
        k.ReleaseMutex.argtypes=[w.HANDLE]; k.CloseHandle.argtypes=[w.HANDLE]
        k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE]; k.CreateFileW.restype=w.HANDLE
        k.GetFileSizeEx.argtypes=[w.HANDLE,c.POINTER(c.c_longlong)]
        mutex=k.CreateMutexW(None,False,'Local\\Codex_Performance_Benchmark')
        held=False; handle=None
        try:
            if not mutex or k.WaitForSingleObject(mutex,0) not in (0,128):
                raise Occupied('Host benchmark mutex occupied')
            held=True
            handle=k.CreateFileW(str(path),0xc0000000,0,None,4,0x100,None) # OPEN_ALWAYS, no truncate
            if handle in (None,c.c_void_p(-1).value):
                handle=None; raise Occupied('Host cross-session benchmark file occupied')
            size=c.c_longlong()
            if not k.GetFileSizeEx(handle,c.byref(size)):
                raise c.WinError(c.get_last_error())
            if size.value:
                raise Occupied('Nonempty reservation file preserved')
            yield {'kind':'Windows mutex plus exclusive OPEN_ALWAYS file','path':str(path)}
        finally:
            if handle: k.CloseHandle(handle)
            if held: k.ReleaseMutex(mutex)
            if mutex: k.CloseHandle(mutex)
    else:
        import fcntl
        import stat
        fd=os.open(path,os.O_RDWR|os.O_CREAT|getattr(os,'O_NOFOLLOW',0),0o600)
        try:
            try: fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError: raise Occupied('Host benchmark file occupied') from None
            info=os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size:
                raise Occupied('Unexpected/nonempty reservation file preserved')
            yield {'kind':'Linux nonblocking flock, no truncate','path':str(path)}
        finally:
            os.close(fd)


def parse_cpu_list(value):
    result=set()
    for part in value.strip().split(','):
        if not part: continue
        endpoints=part.split('-'); first=int(endpoints[0]); last=int(endpoints[-1])
        result.update(range(first,last+1))
    return sorted(result)


def linux_cpu_times():
    result={}
    for line in Path('/proc/stat').read_text().splitlines():
        fields=line.split()
        if not fields[0].startswith('cpu') or not fields[0][3:].isdigit(): continue
        values=list(map(int,fields[1:9])) # Guest times are already included.
        result[int(fields[0][3:])]=(sum(values),values[3]+values[4])
    return result


def linux_disk(path):
    device=Path('/sys/dev/block')/f'{os.major(path.stat().st_dev)}:{os.minor(path.stat().st_dev)}'
    device=device.resolve()
    if (device/'partition').exists(): device=device.parent
    major,minor=map(int,(device/'dev').read_text().strip().split(':'))
    for line in Path('/proc/diskstats').read_text().splitlines():
        fields=line.split()
        if (int(fields[0]),int(fields[1]))==(major,minor):
            return {'device':fields[2],'in_flight':int(fields[11]),'busy_ms':int(fields[12]),'weighted_ms':int(fields[13])}
    raise RuntimeError('Workspace disk counters unavailable')


def linux_snapshot(work_root):
    online=set(parse_cpu_list(Path('/sys/devices/system/cpu/online').read_text()))
    groups=set()
    for cpu in online:
        siblings=parse_cpu_list(Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list').read_text())
        groups.add(tuple(n for n in siblings if n in online))
    groups=sorted(groups)
    before=linux_cpu_times(); samples=[]; gpu=[]; disk=[]
    disk_before=linux_disk(Path(work_root)); disk_time=time.monotonic()
    gpu_paths=sorted(glob.glob('/sys/class/drm/card[0-9]*/device/gpu_busy_percent'))
    for _ in range(2):
        time.sleep(1); after=linux_cpu_times(); row={}
        for cpu in online:
            total=after[cpu][0]-before[cpu][0]; idle=after[cpu][1]-before[cpu][1]
            if total<=0 or idle<0 or idle>total: raise RuntimeError('Invalid Linux CPU counters')
            row[cpu]=100*(total-idle)/total
        samples.append({'utc':utc(),'cpu_percent':row}); before=after
        current=linux_disk(Path(work_root)); stamp=time.monotonic(); elapsed_ms=(stamp-disk_time)*1000
        current.update(average_queue=(current['weighted_ms']-disk_before['weighted_ms'])/elapsed_ms,
                       busy_percent=100*(current['busy_ms']-disk_before['busy_ms'])/elapsed_ms)
        disk.append(current); disk_before=current; disk_time=stamp
        gpu.append({'utc':utc(),'engines':{p:float(Path(p).read_text()) for p in gpu_paths}})
    candidates=[{'siblings':list(group),'peak_load_percent':max(sample['cpu_percent'][cpu] for sample in samples for cpu in group)} for group in groups]
    pressure={}
    for kind in ('cpu','io','memory'):
        p=Path('/proc/pressure')/kind
        if p.exists():
            pressure[kind]={line.split()[0]:{k:float(v) for k,v in (field.split('=') for field in line.split()[1:])} for line in p.read_text().splitlines()}
    gpu_values=[v for row in gpu for v in row['engines'].values()]
    if any(not math.isfinite(v) or not 0<=v<=100 for v in gpu_values): raise RuntimeError('Invalid GPU counters')
    # Include measured IO stall rather than relying on a compiler/game blacklist.
    io_stall=pressure.get('io',{}).get('some',{}).get('avg10')
    thermals={p:Path(p).read_text().strip() for p in glob.glob('/sys/class/thermal/thermal_zone[0-9]*/temp')}
    power={p:Path(p).read_text().strip() for pattern in ('/sys/class/power_supply/*/online','/sys/class/power_supply/*/capacity','/sys/class/power_supply/*/status') for p in glob.glob(pattern)}
    return {'candidates':candidates,'raw_cpu':samples,'raw_gpu':gpu,'raw_disk':disk,'disk_queue_peak':max(max(row['in_flight'],row['average_queue']) for row in disk),'gpu_peak_percent':max(gpu_values) if gpu_values else None,'io_stall_avg10_percent':io_stall,'pressure':pressure,'thermal_millidegrees':thermals,'power':power,'method':'Two complete1second /proc/stat windows; every SMT sibling measured; DRM GPU busy, physical backing disk and kernel PSI'}


def windows_cpu_groups():
    import ctypes as c
    import ctypes.wintypes as w
    # Layout from the installed Windows SDK winnt.h. Processor groups beyond
    # group0 need an explicit group-aware affinity API, so decline them safely.
    k=c.WinDLL('kernel32',use_last_error=True)
    k.GetLogicalProcessorInformationEx.argtypes=[c.c_int,c.c_void_p,c.POINTER(w.DWORD)]
    size=w.DWORD(); k.GetLogicalProcessorInformationEx(0,None,c.byref(size))
    if not size.value: raise c.WinError(c.get_last_error())
    buffer=c.create_string_buffer(size.value)
    if not k.GetLogicalProcessorInformationEx(0,buffer,c.byref(size)): raise c.WinError(c.get_last_error())
    data=buffer.raw[:size.value]; offset=0; groups=[]
    while offset<len(data):
        relation,length=struct.unpack_from('<II',data,offset)
        if length<32 or offset+length>len(data): raise RuntimeError('Invalid CPU topology record')
        count=struct.unpack_from('<H',data,offset+30)[0]
        if relation!=0 or count!=1: raise RuntimeError('Unsupported processor-group topology')
        mask,group=struct.unpack_from('<QH',data,offset+32)
        if group or not mask: raise RuntimeError('Unsupported processor-group affinity')
        groups.append([n for n in range(64) if mask&(1<<n)]); offset+=length
    return groups


def windows_counters():
    import ctypes as c
    import ctypes.wintypes as w
    class Union(c.Union):
        _fields_=[('longValue',w.LONG),('doubleValue',c.c_double),('largeValue',c.c_longlong),('stringValue',c.c_void_p)]
    class Value(c.Structure): _fields_=[('status',w.DWORD),('value',Union)]
    class Item(c.Structure): _fields_=[('name',w.LPWSTR),('formatted',Value)]
    pdh=c.WinDLL('pdh')
    pdh.PdhOpenQueryW.argtypes=[w.LPCWSTR,c.c_size_t,c.POINTER(w.HANDLE)]
    pdh.PdhAddEnglishCounterW.argtypes=[w.HANDLE,w.LPCWSTR,c.c_size_t,c.POINTER(w.HANDLE)]
    pdh.PdhCollectQueryData.argtypes=[w.HANDLE]
    pdh.PdhGetFormattedCounterArrayW.argtypes=[w.HANDLE,w.DWORD,c.POINTER(w.DWORD),c.POINTER(w.DWORD),c.c_void_p]
    pdh.PdhCloseQuery.argtypes=[w.HANDLE]
    def checked(code):
        if code: raise RuntimeError('PDH error '+hex(code&0xffffffff))
    query=w.HANDLE(); checked(pdh.PdhOpenQueryW(None,0,c.byref(query)))
    counters={}; rows={name:[] for name in ('cpu','gpu','io')}
    try:
        for name,path in {'cpu':r'\Processor(*)\% Processor Time','gpu':r'\GPU Engine(*)\Utilization Percentage','io':r'\PhysicalDisk(*)\Current Disk Queue Length'}.items():
            counter=w.HANDLE(); checked(pdh.PdhAddEnglishCounterW(query,path,0,c.byref(counter))); counters[name]=counter
        checked(pdh.PdhCollectQueryData(query))
        for window in range(2):
            time.sleep(1); checked(pdh.PdhCollectQueryData(query)); stamp=utc()
            for name,counter in counters.items():
                size=w.DWORD(); count=w.DWORD()
                status=pdh.PdhGetFormattedCounterArrayW(counter,0x200,c.byref(size),c.byref(count),None)
                if status&0xffffffff!=0x800007d2: checked(status)
                if not size.value or not count.value: raise RuntimeError('Missing '+name+' counters')
                buffer=c.create_string_buffer(size.value)
                checked(pdh.PdhGetFormattedCounterArrayW(counter,0x200,c.byref(size),c.byref(count),buffer))
                items=c.cast(buffer,c.POINTER(Item))
                for index in range(count.value):
                    item=items[index]; value=float(item.formatted.value.doubleValue)
                    if item.formatted.status not in (0,1) or not math.isfinite(value): raise RuntimeError('Invalid '+name+' counter')
                    rows[name].append({'name':item.name,'value':value,'utc':stamp,'window':window})
    finally: pdh.PdhCloseQuery(query)
    return rows


def windows_snapshot(work_root):
    groups=windows_cpu_groups(); rows=windows_counters()
    usage={}
    for row in rows['cpu']:
        if row['name'].isdigit(): usage.setdefault(int(row['name']),[]).append(row['value'])
    if any(cpu not in usage for group in groups for cpu in group): raise RuntimeError('Incomplete CPU counters')
    candidates=[{'siblings':group,'peak_load_percent':max(max(usage[cpu]) for cpu in group)} for group in groups]
    engines={}
    for row in rows['gpu']:
        name=row['name']; key=(row['window'],name[name.index('_luid_'):] if '_luid_' in name else name)
        engines[key]=engines.get(key,0)+max(0,row['value'])
    drive=Path(work_root).resolve().drive.lower()
    queue=[row['value'] for row in rows['io'] if drive in row['name'].lower().split()]
    if not queue: raise RuntimeError('Cannot identify workspace physical disk counters')
    return {'candidates':candidates,'raw_counters':rows,'gpu_peak_percent':max(engines.values()),'disk_queue_peak':max(queue),'method':'Windows native physical-core topology and primed PDH CPU/GPU/disk, two complete1second windows'}


def headroom(work_root, min_free_gib=4, require_gpu=True):
    root=Path(work_root)
    if not root.is_dir(): raise RuntimeError('Workspace is not an existing ready directory')
    if not math.isfinite(min_free_gib) or min_free_gib<0: raise RuntimeError('Invalid minimum free space')
    free=shutil.disk_usage(root).free
    snapshot=windows_snapshot(root) if os.name=='nt' else linux_snapshot(root)
    best=min(snapshot['candidates'],key=lambda item:item['peak_load_percent'])
    reasons=[]
    if best['peak_load_percent']>20: reasons.append('No physical core with all siblings <=20percent')
    gpu=snapshot['gpu_peak_percent']
    if require_gpu and gpu is None: reasons.append('GPU headroom unavailable')
    elif require_gpu and gpu>10: reasons.append('GPU load exceeds10percent')
    if free<min_free_gib*1024**3: reasons.append('Insufficient workspace free space')
    if snapshot.get('disk_queue_peak',0)>2: reasons.append('Workspace disk queue exceeds2')
    if snapshot.get('io_stall_avg10_percent',0) is not None and snapshot.get('io_stall_avg10_percent',0)>1: reasons.append('IO pressure exceeds1percent')
    return {'schema':'benchmark-host-headroom/2','utc':utc(),'host':{'name':platform.node(),'os':platform.platform(),'machine':platform.machine(),'logical_cpus':os.cpu_count(),'physical_cores':len(snapshot['candidates'])},'workspace':str(root.resolve()),'free_bytes':free,'minimum_free_gib':min_free_gib,'selected_cpu':best['siblings'][0],'selected_affinity_mask':1<<best['siblings'][0],'selected_peak_load_percent':best['peak_load_percent'],'gpu_required':require_gpu,'eligible':not reasons,'reasons':reasons,'observations':snapshot,'process_name_exclusions':False,'comparison_scope':'Same host/source/workload/features; original A/A, balanced-pair spread and per-game regression gates still required'}


def probe(work_root, min_free_gib=4, require_gpu=True, lock_path=None):
    with lease(lock_path) as ownership:
        result=headroom(work_root,min_free_gib,require_gpu)
        result['reservation']=ownership
        return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--work-root',required=True)
    parser.add_argument('--min-free-gib',type=float,default=4)
    parser.add_argument('--gpu',choices=('required','observe'),default='required')
    parser.add_argument('--output')
    args=parser.parse_args()
    try:
        result=probe(args.work_root,args.min_free_gib,args.gpu=='required')
    except (Occupied,OSError,RuntimeError) as error:
        result={'schema':'benchmark-host-headroom/2','utc':utc(),'host':platform.node(),'eligible':False,'reasons':[str(error)]}
    result['helper_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if args.output: Path(args.output).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result))
    return 0 if result['eligible'] else 75


if __name__=='__main__':
    sys.exit(main())
