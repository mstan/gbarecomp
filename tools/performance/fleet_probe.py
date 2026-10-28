"""Inventory all authorized targets; one occupied machine does not block another.

Remote helpers must already be staged from this exact source revision. A probe
releases its lease; the chosen workload must reacquire and recheck atomically.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--registry',required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--target',action='append')
    args=parser.parse_args()
    output=Path(args.output)
    if output.exists(): raise SystemExit('Preserve existing fleet receipt; choose a new output')
    registry=json.loads(Path(args.registry).read_text(encoding='utf-8'))
    rows=[]
    for target in registry['targets']:
        if args.target and target['id'] not in args.target: continue
        helper=target.get('helper',str(Path(__file__).with_name('host_reservation.py')))
        command=[target['python'],helper,'--work-root',target['work_root']]
        if target['transport']=='ssh':
            command=['C:/Windows/System32/OpenSSH/ssh.exe','-o','BatchMode=yes','-o','ConnectTimeout=8',target['ssh_alias']]+command
        try:
            result=subprocess.run(command,capture_output=True,text=True,timeout=30)
            data=json.loads(result.stdout.strip().splitlines()[-1])
            assert data['schema']=='benchmark-host-headroom/2'
            assert result.returncode in (0,75) and (result.returncode==0)==data['eligible']
            assert data['helper_sha256']==hashlib.sha256(Path(__file__).with_name('host_reservation.py').read_bytes()).hexdigest(),'Remote helper source mismatch'
            rows.append({'target':target['id'],'command':command,'exit':result.returncode,'headroom':data})
        except (OSError,ValueError,AssertionError,IndexError,KeyError,subprocess.TimeoutExpired) as error:
            rows.append({'target':target['id'],'eligible':False,'error':str(error)})
        output.write_text(json.dumps({'schema':'benchmark-fleet-probe/2','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'helper_sha256':hashlib.sha256(Path(__file__).with_name('host_reservation.py').read_bytes()).hexdigest(),'targets':rows},indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'receipt':str(output),'eligible_targets':[row['target'] for row in rows if row.get('headroom',{}).get('eligible')]}))


if __name__=='__main__': main()
