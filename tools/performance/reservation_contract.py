"""Exercise real subprocess lease exclusion/release; no game or benchmark."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from host_reservation import Occupied,lease,utc


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--try-lock')
    parser.add_argument('--workspace')
    parser.add_argument('--output')
    args=parser.parse_args()
    if args.try_lock:
        try:
            with lease(args.try_lock): print('acquired')
            return 0
        except Occupied as error:
            print(str(error)); return 75
    root=Path(tempfile.mkdtemp(prefix='.fleet-contract-',dir=args.workspace))
    lock=root/'empty.lock'; nonempty=root/'nonempty.lock'
    record={'schema':'benchmark-reservation-contract/2','utc':utc(),'temporary':str(root),'children':[]}
    def child():
        command=[sys.executable,str(Path(__file__).resolve()),'--try-lock',str(lock)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=10)
        record['children'].append({'command':command,'exit':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
        return result.returncode
    try:
        with lease(lock):
            assert child()==75,'A concurrent subprocess acquired the held reservation'
            assert lock.stat().st_size==0
        assert child()==0,'Released reservation was not reusable'
        assert lock.stat().st_size==0
        sentinel=b'owned-test-content-preserve\n'; nonempty.write_bytes(sentinel)
        rejected=False
        try:
            with lease(nonempty): pass
        except Occupied: rejected=True
        assert rejected and nonempty.read_bytes()==sentinel,'Nonempty sentinel was modified'
        record.update(status='passed',exclusion=True,release_and_reuse=True,nonempty_preserved=True)
    finally:
        # Only these exact agent-created regular files may be cleaned up.
        for path in (lock,nonempty):
            if path.exists():
                assert path.parent==root and not path.is_symlink() and path.is_file()
                path.unlink()
        root.rmdir()
        record['temporary_removed']=not root.exists()
        if args.output: Path(args.output).write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(record))
    return 0


if __name__=='__main__': sys.exit(main())
