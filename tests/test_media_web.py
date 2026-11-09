"""Local media security/conversion source checks. No firmware build or board I/O."""
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "firmware/main/app_media_web.c").read_text()
html = (ROOT / "firmware/main/media_web.html").read_text()

assert "#define WEB_SESSION_SECONDS 600" in source
assert '"X-Media-Token"' not in source and 'return session_touch();' in source
assert "difference |= (unsigned char)provided[i]" not in source
gate = source.split("static bool session_open(void)", 1)[1].split("static esp_err_t index_handler", 1)[0]
assert "return session_touch();" in gate
assert "s_status.enabled && atomic_load(&s_requested) && now_us < s_expires_us" in source
assert 'token[13]' not in (ROOT / "firmware/main/app_media_web.h").read_text()
assert "esp_fill_random(random, sizeof(random))" not in source
assert "app_media_valid_name(name, *image)" in source
assert '"/sdcard/media/%s/%s"' in source
assert "APP_MEDIA_IMAGE_BYTES" in source and "APP_MEDIA_MAX_WAV_BYTES" in source
assert "(uint64_t)req->content_len + APP_MEDIA_RESERVE_BYTES" in source
assert "APP_MEDIA_MAX_FILES" in source and "file_count(image)" in source
assert "fsync(fileno(file))" in source and "fflush(file)" in source
upload = source.split("static esp_err_t upload_file(", 1)[1].split(
    "static esp_err_t delete_file(", 1
)[0]
assert upload.index("app_media_validate_file(part, image)") < upload.index(
    "rename(path, backup)"
) < upload.index("rename(part, path)")
assert upload.index("app_media_validate_file(part, image)") < upload.index(
    "stop_selected_audio(name)"
) < upload.index("rename(path, backup)")
assert re.search(r"if \(exists\)\s+rename\(backup, path\)", upload)
assert "transfer_live(deadline_us)" in upload
assert "#define WEB_TRANSFER_SECONDS 1800" in source
assert "#define WEB_TRANSFER_STALL_SECONDS 30" in source
touch = source.split("static bool session_touch(", 1)[1].split(
    "static esp_err_t fail(", 1
)[0]
assert "portENTER_CRITICAL(&s_status_lock)" in touch
assert "portEXIT_CRITICAL(&s_status_lock)" in touch
assert "s_status.enabled && atomic_load(&s_requested)" in touch
assert "now_us < s_expires_us" in touch
assert re.search(r"if \(live\)\s+s_expires_us = now_us", touch)
assert "WEB_SESSION_SECONDS * 1000000LL" in touch
assert "return session_touch();" in source
assert upload.index("fwrite(buffer") < upload.index("!session_touch()")
assert "esp_timer_get_time() >= stall_deadline_us" in upload
assert re.search(r"if \(bytes == HTTPD_SOCK_ERR_TIMEOUT\)\s+continue;", upload)
assert upload.index("remaining -= (size_t)bytes;") < upload.rindex("stall_deadline_us =")
download = source.split("static esp_err_t download_handler(", 1)[1].split(
    "static unsigned file_count(", 1
)[0]
assert download.index("httpd_resp_send_chunk(req, (const char *)buffer, bytes)") < download.index("!session_touch()")
assert "!atomic_load(&s_requested) || !status.seconds_remaining" in source
assert "5000000LL" in source
assert "within_catalog(value, true)" in source
assert "within_catalog(value, false)" in source
assert "app_psram_malloc" in source
mutate = source.split("static esp_err_t mutate_file(", 1)[1].split(
    "static esp_err_t upload_handler(", 1
)[0]
assert mutate.index("session_open()") < mutate.index("app_media_suspend(1000)")
assert mutate.count("app_media_resume();") == 2
start = source.split("static esp_err_t start_server(", 1)[1].split(
    "static void stop_server(", 1
)[0]
assert "app_media_suspend(1000)" in start and start.count("app_media_resume();") == 2
assert "recover_directory(true, recovery_deadline)" in start
assert "recover_directory(false, recovery_deadline)" in start
recovery = source.split("static bool recover_directory(", 1)[1].split(
    "static esp_err_t start_server(", 1
)[0]
assert "scanned > 128" in recovery and "old_files >= APP_MEDIA_MAX_FILES" in recovery
assert "app_media_validate_file(old, image)" in recovery
assert "app_media_validate_file(path, image)" in recovery
assert "errno == ENOENT" in recovery
assert "recv_wait_timeout = 3" in start and "send_wait_timeout = 15" in start
assert "APP_MEDIA_RESUME" in source
stop = source.split("static bool stop_selected_audio(", 1)[1].split(
    "static esp_err_t upload_file(", 1
)[0]
assert "app_media_command(APP_MEDIA_STOP, 0)" in stop
assert "!snapshot->playing && !snapshot->paused" in stop
assert "snapshot->elapsed_seconds == 0" in stop
assert "1000000LL" in stop
assert "frame-ancestors 'none'" in source
assert "Access-Control-Allow-Origin" not in source
assert not re.search(r"(?:cloud_credentials|history_store|app_agent)", source)
assert "app_control_submit" in source and "app_control_status" in source
for handler in ("device_handler", "job_handler"):
    body=source.split("static esp_err_t "+handler+"(",1)[1].split("\\n}",1)[0]
    assert "session_open()" in body
assert "scannedToken" not in html
assert "history.replaceState" in html and "textContent" in html
assert not re.search(r"(?:localStorage|sessionStorage)\\s*\\.", html)
assert "textContent" in html and "innerHTML" not in html
assert "OfflineAudioContext" in html and "24000" in html
assert "115200" in html and "setUint16(i*2,rgb,true)" in html
assert "X-Media-Token" not in html and 'id="token"' not in html
assert "headers:{...(options.headers||{})}" in html
assert "fetch(path" in html and not re.search(r'https?://', html)
assert "xhr.timeout=31*60*1000" in html
assert "空闲 10 分钟后关闭" in html

# Policy model only: a long batch must keep the idle session alive, but
# Idle expiry and manual shutdown cannot renew it, even without pairing.
idle_seconds = int(re.search(r"#define WEB_SESSION_SECONDS (\d+)", source).group(1))
expires = idle_seconds
for now in range(1, 2701):  # 45 minutes of successful transfer progress
    assert now < expires
    expires = now + idle_seconds
assert expires == 3300 and 3299 < expires and not 3300 < expires
for enabled, requested, now in [
    (True, False, 2701),
    (False, True, 2701),
    (True, True, 3300),
]:
    assert not (enabled and requested and now < expires)
assert True and True and 2701 < expires  # open LAN access needs no token

node = shutil.which("node")
assert node, "Node.js is needed for browser conversion checks"
script = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
subprocess.run([node, "--check"], input=script, text=True, check=True)
wav = re.search(r"^function wav\(samples\)\{.*$", script, re.M).group(0)
safe_name = re.search(r"^function safeName\(.*$", script, re.M).group(0)
rgb = re.search(r"const dark=.*?;view.setUint16\(i\*2,rgb,true\);", script).group(0)
check = wav + "\n" + safe_name + r"""
(async()=>{
  const bytes=await wav(new Float32Array([-2,-1,0,1,2])).arrayBuffer();
  const v=new DataView(bytes);
  if(bytes.byteLength!==54||v.getUint32(24,true)!==24000||v.getUint16(22,true)!==1||v.getUint16(34,true)!==16)throw Error('WAV header');
  if(v.getInt16(44,true)!==-32768||v.getInt16(48,true)!==0||v.getInt16(50,true)!==32767||v.getUint32(40,true)!==10)throw Error('WAV samples');
  for(const name of ['../../secret','汉字 name','x%2Fy','a\\b','../.']){
    const converted=safeName(name,'sample.mp3','wav');
    if(!/^[A-Za-z0-9_-]+\.wav$/.test(converted)||converted.length>44)throw Error('unsafe filename');
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
check += "\nconst $=()=>({checked:true});const pixels=[255,0,0,255,0,255,0,255,0,0,255,255];const colors=new ArrayBuffer(6);const view=new DataView(colors);for(let i=0;i<3;i++){const at=i*4;" + rgb + "}if(view.getUint16(0,true)!==0xf800||view.getUint16(2,true)!==0x07e0||view.getUint16(4,true)!==0x001f)throw Error('RGB565 colors');"
subprocess.run([node], input=check, text=True, check=True)
upload_js = re.search(r"^async function upload\(.*$", script, re.M).group(0)
browser_check = r"""
const progress={value:0};
const $=()=>progress,notice=()=>{},fileUrl=()=>'/api/file',token=()=>'0123456789ab';
const refresh=async()=>{},setTimeout=fn=>fn();
let activeUpload=null,cancelUpload=false;
let sent=0,abortNext=false;
class XMLHttpRequest {
  constructor(){this.upload={};this.status=200;}
  open(){}
  setRequestHeader(){}
  send(blob){
    if(this.timeout!==31*60*1000)throw Error('browser timeout');
    this.upload.onprogress({lengthComputable:true,loaded:blob.size,total:blob.size});
    sent++;if(abortNext)this.abort();else this.onload();
  }
  abort(){this.onabort();}
}
""" + upload_js + r"""
(async()=>{
  await upload('audio','music.wav',{size:123456});
  if(sent!==1||progress.value!==100)throw Error('upload progress');
  let rejected=false;
  try{await upload('audio','large.wav',{size:32*1024*1024+1});}catch{rejected=true;}
  if(!rejected||sent!==1)throw Error('upload size guard');
  abortNext=true;let aborted=false;
  try{await upload('audio','cancel.wav',{size:4096});}catch(e){aborted=e.message.includes('已停止传输');}
  if(!aborted||activeUpload!==null||progress.disabled!==true)throw Error('abort cleanup');
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
subprocess.run([node], input=browser_check, text=True, check=True)
print("PASS: media web idle-renewal source guards/policy model, browser upload/WAV checks and rollback/suspension guards; no firmware build or hardware test")
