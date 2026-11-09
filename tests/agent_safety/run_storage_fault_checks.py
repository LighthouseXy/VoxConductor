#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
main=ROOT/'firmware/main'
cjson=ROOT/'firmware/managed_components/espressif__cjson/cJSON'
flags=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
       '-fsanitize=address,undefined','-g','-I'+str(main),'-I'+str(cjson)]
def run(args): subprocess.run([str(x) for x in args],check=True)
with tempfile.TemporaryDirectory(prefix='vox-storage-fault-') as temporary:
    out=Path(temporary)
    run(flags+['-Wno-deprecated-declarations','-c',cjson/'cJSON.c','-o',out/'json.o'])
    run(flags+['-Dfprintf=evidence_fprintf','-Dfsync=evidence_fsync','-Dfopen=evidence_fopen',
               '-c',main/'app_history_store.c','-o',out/'history.o'])
    run(flags+[HERE/'test_commit_failures.c',out/'history.o',main/'app_conversation.c',
               main/'app_turn.c',out/'json.o','-lm','-o',out/'check'])
    (out/'fixture').mkdir()
    run([out/'check',out/'fixture'])
print('PASS storage fault checks; temporary fixtures cleaned')
