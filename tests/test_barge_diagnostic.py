"""Source and browser-mock checks only; no C build, native DSP or device tests."""
from pathlib import Path
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
read = lambda name: (MAIN / name).read_text()
diag, header = read("app_barge_diagnostic.c"), read("app_barge_diagnostic.h")
acoustic, wake, tts = read("app_acoustic.c"), read("app_wake.c"), read("app_cloud_tts.c")
web, html = read("app_media_web.c"), read("media_web.html")
assert '#define BARGE_DIAG_SECONDS 12' in header
assert '#define BARGE_DIAG_TRACKS 4' in header
assert 'RETENTION_US (300 * 1000000LL)' in diag and 'WAIT_US (120 * 1000000LL)' in diag
assert 'app_psram_malloc(BYTES)' in diag and 'BYTES + 2 * 1024 * 1024' in diag
start = diag.split('esp_err_t app_barge_diagnostic_start', 1)[1].split('esp_err_t app_barge_diagnostic_clear', 1)[0]
assert start.count('app_wake_input_allowed()') == 2
assert 'app_wake_diagnostic_active()' in start
assert 'status.state = 1' in start and 'started_us =' not in start
begin = diag.split('void app_barge_diagnostic_begin', 1)[1].split('void app_barge_diagnostic_end', 1)[0]
assert 'status.state == 1' in begin and 'status.state = 2' in begin
assert 'atomic_store(&active_epoch, epoch)' in begin
capture = diag.split('void app_barge_diagnostic_pcm', 1)[1].split('size_t app_barge_diagnostic_read', 1)[0]
assert 'xSemaphoreTake(lock, 0)' in capture and 'CAP - status.samples[track]' in capture
assert capture.count('epoch') >= 3
download = diag.split('size_t app_barge_diagnostic_read', 1)[1]
assert 'id == status.id && status.state == 3 && offset < bytes' in download
assert 'track >= BARGE_DIAG_TRACKS' in download and 'tick_locked()' in download
for forbidden in ('fopen(', 'fwrite(', 'http_client', 'websocket', 'app_cloud_barge_in', 'wn->'):
    assert forbidden not in diag
assert 'app_barge_diagnostic_tick();' in wake
assert 'app_barge_diagnostic_clear();' in web.split('static void stop_server', 1)[1]
assert 'app_barge_diagnostic_busy()' in read('app_wake_diagnostic.c')
assert '"app_barge_diagnostic.c"' in read('CMakeLists.txt')
assert acoustic.index('app_barge_diagnostic_pcm(seen_generation, 0') < acoustic.index('if (echoing) aec_process')
assert acoustic.index('if (echoing) aec_process') < acoustic.index('app_barge_diagnostic_pcm(seen_generation, 2')
assert wake.index('work->pcm[i] = (int16_t)scaled') < wake.index('app_barge_diagnostic_pcm(work->acoustic_generation, 3')
assert 'BD_DISABLED' in wake and 'BD_PROCESSED' in wake and 'BD_DEBOUNCE' in wake
assert 'app_barge_diagnostic_input();' in read('app_audio.c')
assert 'BD_REJECT_EPOCH' in acoustic and 'BD_HANDOFF_RACE' in acoustic
assert 'app_barge_diagnostic_end(0, BD_UNAVAILABLE)' in tts
for name in ('barge_diagnostic_handler', 'barge_audio_handler'):
    part = web.split('static esp_err_t ' + name, 1)[1].split('\nstatic ', 1)[0]
    assert part.index('session_open()') < part.index('app_barge_diagnostic_')
assert '(channel[0] < \'0\' || channel[0] > \'3\' || channel[1])' in web
assert 'status.state != 3 || status.id != id || !status.samples[track]' in web
assert 'if (!count) { result = ESP_FAIL; break; }' in web
assert 'if (!session_touch()) result = ESP_FAIL;' in web

# Capacity, immutable completion and ID guards, including zero-audio timeout.
class Capture:
    def __init__(self):
        self.state, self.id, self.samples = 1, 7, [0] * 4
    def begin(self):
        if self.state == 1: self.state = 2
    def put(self, track, n):
        if self.state == 2: self.samples[track] = min(192000, self.samples[track] + n)
    def read(self, id, track, offset):
        return self.state == 3 and id == self.id and 0 <= track < 4 and offset < self.samples[track] * 2
c = Capture(); c.put(0, 512); assert c.samples == [0] * 4
c.begin()
for i in range(4): c.put(i, 200000)
assert c.samples == [192000] * 4
c.state = 3; c.put(0, 999); assert c.samples[0] == 192000
assert c.read(7, 3, 383998) and not c.read(7, 3, 384000)
assert not c.read(6, 3, 0) and not c.read(7, 4, 0)
c = Capture(); c.state = 3; assert not c.read(7, 0, 0)

# Real browser handlers, with mocked HTTP and downloads, not a browser/device run.
script = html.split('function saveDiagnosticBlob', 1)[1].split('let wakeDiagnostic=null;', 1)[0]
script = 'function saveDiagnosticBlob' + script
node = shutil.which('node')
assert node, 'node is required for browser-mock checks'
mock = r'''
const vm=require('vm'),assert=require('assert');
const elements=new Map(),downloads=[],calls=[];
const el=id=>{if(!elements.has(id))elements.set(id,{disabled:false,textContent:''});return elements.get(id);};
let consent=false,truncated=false,failClear=false;
const empty=()=>({id:0,state:0,reason:0,rate:16000,seconds:12,elapsed_ms:0,clipped:0,skipped:0,counts:{},tracks:Array.from({length:4},()=>({samples:0,peak:0}))});
let state=empty();
const context={console,JSON,Array,Error,Blob,setTimeout:fn=>fn(),confirm:()=>consent,$:el,
 document:{createElement:()=>({click(){downloads.push(this.download);}})},
 URL:{createObjectURL:()=> 'blob:test',revokeObjectURL(){}},notice:t=>el('notice').textContent=t,
 guard:async fn=>{try{await fn();}catch(e){el('notice').textContent=e.message;}},
 request:async(path,options={})=>{
  calls.push([path,options.method||'GET']);
  if(path.includes('/audio?')){
   const track=Number(path.match(/track=(\d)/)[1]);
   assert(path.endsWith('id='+state.id));
   return new Blob([new Uint8Array(44+state.tracks[track].samples*2-(truncated?1:0))]);
  }
  assert.equal(path,'/api/barge-diagnostic');
  if(options.method==='POST')state={...empty(),id:7,state:1};
  if(options.method==='DELETE'){if(failClear)throw Error('未清除');state=empty();}
  return JSON.parse(JSON.stringify(state));
 }};
vm.createContext(context);
'''
checks = r'''
(async()=>{
 await el('startBargeDiagnostic').onclick();assert.equal(calls.length,0);
 consent=true;await el('startBargeDiagnostic').onclick();assert.equal(state.state,1);
 assert(el('startBargeDiagnostic').disabled);assert(el('downloadBargeSummary').disabled);
 await el('downloadBarge0').onclick();assert.equal(downloads.length,0);
 state.state=2;await el('refreshBargeDiagnostic').onclick();assert(el('downloadBarge0').disabled);
 state.state=3;state.reason=1;state.counts={mic:375,aec:375,queued:370,processed:370,hit:0,accepted:0};
 state.tracks=state.tracks.map(()=>({samples:16000,peak:1200}));
 await el('refreshBargeDiagnostic').onclick();assert(!el('downloadBarge3').disabled);
 for(let i=0;i<4;i++)await el('downloadBarge'+i).onclick();
 await el('downloadBargeSummary').onclick();
 assert.deepEqual(downloads,['barge-7-raw.wav','barge-7-reference.wav','barge-7-aec.wav','barge-7-model.wav','barge-7-summary.json']);
 truncated=true;await el('downloadBarge3').onclick();assert.equal(downloads.length,5);assert(el('notice').textContent.includes('不完整'));
 failClear=true;await el('clearBargeDiagnostic').onclick();assert.equal(state.id,7);assert.equal(el('notice').textContent,'未清除');
 failClear=false;await el('clearBargeDiagnostic').onclick();assert(el('downloadBarge0').disabled);
 state={...empty(),id:8,state:3,reason:3};await el('refreshBargeDiagnostic').onclick();
 assert(el('downloadBarge0').disabled);assert(!el('downloadBargeSummary').disabled);
 console.log('PASS: barge consent/wait/finish/four WAVs/summary/truncation/clear/timeout browser mocks');
})().catch(e=>{console.error(e);process.exitCode=1;});
'''
subprocess.run([node], input=mock + '\nvm.runInContext(' + json.dumps(script) + ',context);\n' + checks,
               text=True, check=True)
tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for source in (diag, header, acoustic, wake, web):
    stack = []
    for ch in tokens.sub('', source):
        if ch in '({[': stack.append(ch)
        elif ch in ')}]': assert stack and stack.pop() == {')':'(', '}':'{', ']':'['}[ch]
    assert not stack
print('PASS: bounded consent-only playback diagnostics and source instrumentation; C/native/device NOT run')
