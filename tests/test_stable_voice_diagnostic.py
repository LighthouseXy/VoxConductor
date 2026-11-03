"""Stable-mode source guards and mocked browser diagnostics.

No firmware compile, native WakeNet/AEC, physical microphone or network test.
Real recognition accuracy requires the exported device recordings.
"""
import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
read = lambda name: (MAIN / name).read_text()
config = read("Kconfig.projbuild")
sdk = (ROOT / "firmware/sdkconfig").read_text()
assert "CONFIG_VOX_ACOUSTIC_BARGE_IN=y" in sdk
barge = config.split("config VOX_ACOUSTIC_BARGE_IN", 1)[1].split("config VOX_AEC_DELAY_MS", 1)[0]
assert "default y" in barge and "VOX_WAKE_WORD" in barge
acoustic = read("app_acoustic.c").split("#else", 1)[1]
assert "bool app_acoustic_wake_detected(unsigned epoch)" in acoustic
assert "bool app_acoustic_pending(void) { return false; }" in acoustic
runner = read("main.c")
turn = runner.split("static esp_err_t run_voice_turn", 1)[1].split("static void voice_conversation_task", 1)[0]
guarded = turn.split("#if CONFIG_VOX_ACOUSTIC_BARGE_IN", 1)[1].split("#endif", 1)[0]
assert "app_acoustic_prepare()" in guarded and "本轮语音打断未就绪" in guarded
assert "bool continuous = automatic;" in runner and "app_audio_prepare_auto_recording();" in runner
assert "小冰小冰空闲唤醒及回答中打断" in runner
dispatch = runner.split("static bool dispatch_wake", 1)[1].split("static void wake_dispatch_task", 1)[0]
assert dispatch.index("app_wake_diagnostic_active()") < dispatch.index("start_voice(true)")

diag = read("app_wake_diagnostic.c")
assert "app_psram_malloc(CAPACITY * 2 * sizeof(int16_t))" in diag
start = diag.split("esp_err_t app_wake_diagnostic_start", 1)[1].split("esp_err_t app_wake_diagnostic_clear", 1)[0]
assert start.count("app_wake_input_allowed()") == 2
assert "RETENTION_US (300 * 1000000LL)" in diag
assert "APP_WAKE_DIAGNOSTIC_SECONDS * 1000000LL" in diag
assert "free(audio);" in diag and "memset(&status, 0" in diag
raw = diag.split("unsigned app_wake_diagnostic_raw", 1)[1].split("void app_wake_diagnostic_model", 1)[0]
model = diag.split("void app_wake_diagnostic_model", 1)[1].split("void app_wake_diagnostic_detected", 1)[0]
assert "xSemaphoreTake(lock, 0)" in raw and "xSemaphoreTake(lock, 0)" in model
assert "count <= CAPACITY - status.samples" in raw and "count <= CAPACITY - status.samples" in model
assert "id == status.id" in model and "pending_count == count" in model
commit = diag.split("void app_wake_diagnostic_detected", 1)[1].split("size_t app_wake_diagnostic_read", 1)[0]
assert "id == status.id && status.active && pending_model" in commit
assert "status.samples += pending_count;" in commit
readback = diag.split("size_t app_wake_diagnostic_read", 1)[1]
assert "id == status.id && !status.active && audio && offset < bytes" in readback
assert "if (copied > capacity) copied = capacity;" in readback
assert not any(word in diag for word in ("fopen", "fwrite", "esp_http", "app_cloud"))
wake = read("app_wake.c")
worker = wake.split("static void worker", 1)[1].split("void app_wake_init", 1)[0]
assert worker.index("app_wake_diagnostic_raw(work->pcm, chunk)") < worker.index("int32_t scaled =")
assert worker.index("app_wake_diagnostic_model(") < worker.index("wn->detect(model, work->pcm)")
assert worker.index("wn->detect(model, work->pcm)") < worker.index("app_wake_diagnostic_detected(")
assert worker.index("diagnostic_id || app_wake_diagnostic_active()") < worker.index("wakeup();")
assert "CONFIG_VOX_WAKE_GAIN=1" in sdk and "CONFIG_VOX_WAKE_THRESHOLD_PERCENT=60" in sdk
assert '"app_wake_diagnostic.c"' in read("CMakeLists.txt")

web = read("app_media_web.c")
for name in ("wake_diagnostic_handler", "wake_audio_handler"):
    body = web.split("static esp_err_t " + name, 1)[1].split("\n}", 1)[0]
    assert body.index("session_open()") < body.index("app_wake_diagnostic_")
assert "req->content_len" in web.split("static esp_err_t wake_diagnostic_handler", 1)[1].split("static void wav_u32", 1)[0]
assert 'strcmp(channel, "raw") && strcmp(channel, "model")' in web
assert 'strspn(id_text, "0123456789") != strlen(id_text)' in web
assert "status.active || status.id != id || !status.samples" in web
assert "app_wake_diagnostic_read((unsigned)id" in web
assert "wav_u32(header + 24, APP_WAKE_DIAGNOSTIC_RATE)" in web
assert "header[20] = 1; header[22] = 1;" in web
assert "header[32] = 2; header[34] = 16;" in web
stop = web.split("static void stop_server", 1)[1].split("static void web_worker", 1)[0]
assert "app_wake_diagnostic_clear()" in stop
handlers = web.split("static const httpd_uri_t handlers[]", 1)[1].split("};", 1)[0]
capacity = int(re.search(r"config.max_uri_handlers = (\d+)", web).group(1))
assert handlers.count(".uri =") <= capacity
assert "PCM underrun:" in read("app_cloud_tts.c") and "PCM resumed:" in read("app_cloud_tts.c")

html = read("media_web.html")
script = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
assert "blob.size!==44+state.samples*2" in script
node = shutil.which("node")
assert node, "Node required for mocked browser checks"
mock = r'''
const vm=require('vm'),assert=require('assert');
class Element {
 constructor(){this.value='';this.textContent='';this.disabled=false;this.children=[];this.dataset={};this.style={};this.checked=false;this.hidden=false;}
 append(...x){this.children.push(...x);} replaceChildren(...x){this.children=x;}
 click(){downloads.push(this.download);}
 set innerHTML(_){throw Error('unsafe HTML');}
}
const elements=new Map(),el=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
const calls=[],downloads=[],revoked=[];
let consent=true,polls=0,truncated=false,failClear=false;
const empty={id:0,active:false,samples:0,hits:0,clipped:0,skipped_frames:0,raw_peak:0,model_peak:0,elapsed_ms:0,reason:0,available:true,enabled:true,input_allowed:true,computer_barge_in:true,rate:16000,seconds:12};
let state={...empty};
const context={document:{getElementById:el,createElement:()=>new Element(),querySelectorAll:()=>[]},
 location:{hash:'',pathname:'/',search:''},history:{replaceState(){}},URLSearchParams,
 URL:{createObjectURL:()=> 'blob:diagnostic',revokeObjectURL:u=>revoked.push(u)},
 Date,Number,String,Math,JSON,RegExp,Map,Set,TextEncoder,Array,Object,DataView,Uint8Array,Float32Array,Blob,Promise,Error,AbortController,console,window:{},
 confirm:()=>consent,prompt:()=>null,setTimeout:(fn,ms)=>{if(ms<30000)fn();},clearTimeout:()=>{}};
el('token').value='0123456789ab';
context.fetch=async(path,options={})=>{
 assert.equal(options.headers['X-Media-Token'],undefined);calls.push([path,options.method||'GET']);
 if(path.startsWith('/api/wake-diagnostic/audio?')){
   assert.equal(path,`/api/wake-diagnostic/audio?channel=${path.includes('channel=raw')?'raw':'model'}&id=${state.id}`);
   return {ok:true,blob:async()=>new Blob([new Uint8Array(44+state.samples*2-(truncated?2:0))])};
 }
 assert.equal(path,'/api/wake-diagnostic');
 if(options.method==='POST'){state={...empty,id:7,active:true};polls=0;}
 else if(options.method==='DELETE'){
   if(failClear)return {ok:false,text:async()=> '诊断忙，未清除，请重试'};
   state={...empty};
 }else if(state.active && ++polls>=2){state={...state,active:false,samples:32000,hits:2,clipped:4,reason:1};}
 return {ok:true,json:async()=>({...state})};
};
vm.createContext(context);
'''
checks = r'''
(async()=>{
 await el('refreshWakeDiagnostic').onclick();assert(el('downloadWakeRaw').disabled);assert(el('downloadWakeModel').disabled);
 consent=false;await el('startWakeDiagnostic').onclick();assert(!calls.some(c=>c[1]==='POST'));
 consent=true;await el('startWakeDiagnostic').onclick();assert.equal(state.active,false);assert.equal(state.hits,2);
 assert(!el('downloadWakeRaw').disabled);assert(el('wakeDiagnosticInfo').textContent.includes('小冰小冰回答中打断已配置'));
 await el('downloadWakeRaw').onclick();await el('downloadWakeModel').onclick();
 assert.deepEqual(downloads,['wake-7-raw.wav','wake-7-model.wav']);assert.equal(revoked.length,2);
 truncated=true;await el('downloadWakeRaw').onclick();assert.equal(downloads.length,2);assert(el('notice').textContent.includes('下载不完整'));
 failClear=true;await el('clearWakeDiagnostic').onclick();assert.equal(el('notice').textContent,'诊断忙，未清除，请重试');assert.equal(state.id,7);
 failClear=false;await el('clearWakeDiagnostic').onclick();assert(el('downloadWakeRaw').disabled);assert.equal(state.samples,0);
 await el('downloadWakeRaw').onclick();assert.equal(downloads.length,2);assert(el('notice').textContent.includes('请先完成'));
 state={...empty,available:false,enabled:false};await el('refreshWakeDiagnostic').onclick();assert(el('wakeDiagnosticInfo').textContent.includes('未加载'));
 console.log('PASS: consent, diagnostic polling, paired download, truncated transfer, clear failure and unavailable model browser mocks');
})().catch(e=>{console.error(e);process.exitCode=1;});
'''
subprocess.run([node], input=mock + "\nvm.runInContext(" + json.dumps(script) + ",context);\n" + checks,
               text=True, check=True)
print("PASS: stable configuration and bounded/private paired-capture source guards; firmware/native/device NOT tested")
