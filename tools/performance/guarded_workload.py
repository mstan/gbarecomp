"""Run a bounded unattended workload batch under one host-local reservation.

This records resource admission/execution, not a game performance verdict.
The workload remains responsible for correctness and its matched A/B protocol.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from host_reservation import headroom,lease,utc


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()


def system_busy():
    if os.name!='nt':
        values=list(map(int,Path('/proc/stat').read_text().splitlines()[0].split()[1:9]))
        return (sum(values)-values[3]-values[4])/os.sysconf('SC_CLK_TCK')
    import ctypes as c
    import ctypes.wintypes as w
    k=c.WinDLL('kernel32',use_last_error=True)
    k.GetSystemTimes.argtypes=[c.POINTER(w.FILETIME)]*3
    values=[w.FILETIME() for _ in range(3)]
    if not k.GetSystemTimes(*[c.byref(value) for value in values]): raise c.WinError(c.get_last_error())
    seconds=lambda value:((value.dwHighDateTime<<32)+value.dwLowDateTime)/1e7
    return seconds(values[1])+seconds(values[2])-seconds(values[0])


def main():
    parser=argparse.ArgumentParser();parser.add_argument('spec');parser.add_argument('--output',required=True)
    args=parser.parse_args();spec=json.loads(Path(args.spec).read_text(encoding='utf-8'));output=Path(args.output)
    if output.exists(): raise SystemExit('Preserve existing workload receipt')
    root=Path(spec['work_root']); assert root.is_dir()
    record={'schema':'benchmark-guarded-workload/2','started_utc':utc(),'spec_sha256':sha(args.spec),'helper_sha256':sha(Path(__file__).with_name('host_reservation.py')),'harness_sha256':sha(__file__),'runs':[],'performance_claim':False}
    try:
        with lease() as ownership:
            record['reservation']=ownership;selected_cpu=None
            for item in spec['runs']:
                ready=headroom(root,spec.get('min_free_gib',4),spec.get('require_gpu',True))
                assert ready['eligible'],'Insufficient headroom: '+str(ready['reasons'])
                if selected_cpu is None: selected_cpu=ready['selected_cpu']
                chosen=next(row for row in ready['observations']['candidates'] if selected_cpu in row['siblings'])
                assert chosen['peak_load_percent']<=20,'Previously selected physical core became busy'
                command=item['argv']; assert isinstance(command,list) and command and all(isinstance(part,str) for part in command)
                executable=Path(command[0]); assert executable.is_file()
                assert sha(executable)==item['binary_sha256'],'Unexpected native executable identity'
                own=root/item['id'];own.mkdir(exist_ok=False)
                env=dict(os.environ)
                env.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy',GBARECOMP_AUDIO_VOLUME='0')
                env.update(item.get('environment',{}))
                row={'id':item['id'],'command':command,'binary_sha256':sha(executable),'headroom':ready,'selected_cpu':selected_cpu,'stdout':str(own/'stdout.bin'),'stderr':str(own/'stderr.log')}
                record['runs'].append(row)
                kwargs={}
                if os.name=='nt':
                    kwargs['creationflags']=subprocess.CREATE_NO_WINDOW|subprocess.BELOW_NORMAL_PRIORITY_CLASS
                else:
                    def child_setup():
                        os.sched_setaffinity(0,{selected_cpu});os.nice(10)
                    kwargs.update(preexec_fn=child_setup,start_new_session=True)
                before=system_busy();start=time.monotonic()
                with (own/'stdout.bin').open('wb') as stdout,(own/'stderr.log').open('wb') as stderr:
                    child=subprocess.Popen(command,cwd=own,env=env,stdout=stdout,stderr=stderr,**kwargs)
                    row['pid']=child.pid
                    if os.name=='nt':
                        import ctypes as c
                        import ctypes.wintypes as w
                        k=c.WinDLL('kernel32',use_last_error=True)
                        k.SetProcessAffinityMask.argtypes=[w.HANDLE,c.c_size_t]
                        if not k.SetProcessAffinityMask(int(child._handle),1<<selected_cpu):
                            child.terminate();child.wait();raise c.WinError(c.get_last_error())
                    else:
                        import resource
                        usage_before=resource.getrusage(resource.RUSAGE_CHILDREN)
                    observations=[]
                    while child.poll() is None:
                        try: child.wait(timeout=min(1,max(.01,item.get('timeout_seconds',60)-(time.monotonic()-start))))
                        except subprocess.TimeoutExpired: pass
                        observations.append({'elapsed_seconds':time.monotonic()-start,'system_busy_seconds':system_busy()-before})
                        if time.monotonic()-start>item.get('timeout_seconds',60) and child.poll() is None:
                            child.terminate()
                            try: child.wait(timeout=5)
                            except subprocess.TimeoutExpired: child.kill();child.wait()
                            raise RuntimeError('Own bounded workload timed out')
                    wall=time.monotonic()-start;busy=system_busy()-before
                    if os.name=='nt':
                        k.GetProcessTimes.argtypes=[w.HANDLE]+[c.POINTER(w.FILETIME)]*4
                        values=[w.FILETIME() for _ in range(4)]
                        if not k.GetProcessTimes(int(child._handle),*[c.byref(value) for value in values]):raise c.WinError(c.get_last_error())
                        cpu=sum(((value.dwHighDateTime<<32)+value.dwLowDateTime)/1e7 for value in values[2:])
                    else:
                        usage_after=resource.getrusage(resource.RUSAGE_CHILDREN)
                        cpu=usage_after.ru_utime+usage_after.ru_stime-usage_before.ru_utime-usage_before.ru_stime
                row.update(exit=child.returncode,wall_seconds=wall,cpu_seconds=cpu,other_cpu_equivalents=max(0,busy-cpu)/wall,load_observations=observations)
                row['outputs']={name:{'sha256':sha(own/name),'bytes':(own/name).stat().st_size} for name in ('stdout.bin','stderr.log')}
                assert child.returncode==0,'Native workload failed'
                assert row['other_cpu_equivalents']<=max(.8,ready['host']['physical_cores']-2),'Background CPU load exceeded unchanged campaign limit'
            record['status']='passed guarded execution; workload correctness/performance verdict separate'
    except BaseException as error:
        record.update(status='failed_or_deferred',error=str(error))
    finally:
        record['finished_utc']=utc();output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print(json.dumps(record))
    return 0 if record['status'].startswith('passed') else 75


if __name__=='__main__': sys.exit(main())
