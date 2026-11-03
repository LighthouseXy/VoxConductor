#!/usr/bin/env python3
"""Offline contract gate; uses only synthetic SDK, model and device fixtures."""
from pathlib import Path
import argparse
import subprocess
import sys

here=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--only',choices=['reliability','runtime','transport','semantic','storage'])
args=parser.parse_args()
checks={'reliability':[sys.executable,str(here/'run_reliability_checks.py')],
        'runtime':[sys.executable,str(here/'run_runtime_checks.py')],
        'transport':[sys.executable,str(here/'run_transport_checks.py')],
        'semantic':['sh',str(here/'run_semantic_checks.sh')],
        'storage':[sys.executable,str(here/'run_storage_fault_checks.py')]}
for name,command in checks.items():
    if args.only and args.only!=name: continue
    print('RUN',name,flush=True)
    subprocess.run(command,check=True)
print('PASS offline Agent safety gate')
