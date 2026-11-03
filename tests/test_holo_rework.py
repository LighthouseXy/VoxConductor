"""Read-only holographic/web coordinator checks; no C build or device calls."""
import array, json, re, shutil, subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
main=root/"firmware/main"
control=(main/"app_control.c").read_text()
desktop=(main/"app_desktop.c").read_text()
ui=(main/"app_ui.c").read_text()
core=(main/"app_media.c").read_text()
web=(main/"app_media_web.c").read_text()
runner=(main/"main.c").read_text()
hist=(main/"app_history_store.c").read_text()
assert "s_request[768]" in control and "s_phase != CONTROL_PENDING && s_phase != CONTROL_RUNNING" in control
service=control.split("void app_control_service(",1)[1]
assert service.index("s_phase = CONTROL_RUNNING") < service.index("execute(in)")
assert service.index("xSemaphoreGive(s_lock)") < service.index("execute(in)")
reminder=runner.split("static void reminder_task(",1)[1].split("static void handle_motion(",1)[0]
assert reminder.index("atomic_compare_exchange_strong") < reminder.index("app_control_service()") < reminder.index("app_desktop_service()")
assert "app_agent_" not in web and "app_storage_" not in web
for name in ("device_handler","job_handler"):
    body=web.split("static esp_err_t "+name+"(",1)[1].split("\n}",1)[0]
    assert "session_open()" in body
assert "nvs_commit(h)" in desktop and "persist_settings(&snapshot, set->home_style" in desktop
configuration=desktop.split("app_config_result_t app_desktop_configuration_patch(",1)[1].split("bool app_desktop_configuration(",1)[0]
assert configuration.rindex("lvgl_port_unlock();",0,configuration.index("bool saved")) < configuration.index("persist_settings(")
assert "return saved ? APP_CONFIG_SAVED : APP_CONFIG_SAVE_FAILED" in configuration
assert "lv_qrcode_update" in desktop and '#token=%s' not in desktop
assert "CONFIG_LV_USE_QRCODE=y" in (root/"firmware/sdkconfig").read_text()
assert "s_grid_tiles[4]" in desktop and "s_grid_vertical" in desktop and "dir * 2" in desktop
assert 's_hint = holo_label(' in desktop and 's_hint = "";' not in desktop
assert 'show(s_hint, s_feedback[0] != 0 &&' in desktop
assert "lv_timer_create(wallpaper_tick, 100" in ui
assert "#define WALL_PARTICLES 40" in ui and "NO_VOICE_STATE" in ui.split("static void wallpaper_tick(",1)[1].split("static void page_footer",1)[0]
assert "s_desktop_open" in ui and "static holo_wall_t s_animated;" in ui
assert "s_animated[2]" not in ui and "s_idle_image" not in ui
display=(main/"app_display.c").read_text()
assert "app_psram_malloc(widget_bytes)" in display
assert "lv_mem_add_pool(widget_memory + added * LCD_WIDGET_POOL_BYTES," in display
assert "#define LCD_WIDGET_POOL_BYTES (64U * 1024U)" in display
assert "#define LCD_WIDGET_POOL_COUNT 3U" in display
assert "r < 32 && g < 32 && b < 32" in core or "r<32 && g<32 && b<32" in core
assert 'history_query_impl(directory, "", before, NULL, NULL, true)' in hist or 'history_query_impl(directory,"",before,NULL,NULL,true)' in hist
pack=root/"assets/media-pack"
for name in ("wall_05_lunar.rgb565","wall_06_jelly.rgb565"):
    pixels=array.array("H");pixels.frombytes((pack/"sdcard/media/images"/name).read_bytes())
    assert len(pixels)==57600 and sum(p==0 for p in pixels)/57600>.55
    for y in range(240):
        for x in range(240):
            p=pixels[y*240+x]
            if x<12 or x>=228 or y<12 or y>=228:assert p==0,(name,x,y)
            if p:assert max(((p>>11)&31)*255//31,((p>>5)&63)*255//63,(p&31)*255//31)>=32
html=(main/"media_web.html").read_text()
script=re.search(r"<script>(.*?)</script>",html,re.S).group(1)
assert 'id="wallpaperEnabled"' not in html and 'id="idleAlbum"' not in html
node=shutil.which("node")
assert node
mock=r"""
const vm=require('vm'),assert=require('assert');
class Element {
 constructor(){this.children=[];this.value='';this.checked=false;this.hidden=false;this.disabled=false;this.style={};this.dataset={};this.textContent='';}
 append(...x){this.children.push(...x);}
 replaceChildren(...x){this.children=x;}
 set innerHTML(_){throw Error('unsafe HTML insertion');}
}
const elements=new Map(),byId=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
// Simulate stored old settings that used to enable an album screensaver.
const catalog={images:[],audio:[],repeat:false,playing:false,elapsed:0,duration:0,message:'',appearance:{wallpaper_enabled:false,night_enabled:false,idle_album:true,night_start_minute:1320,night_end_minute:420,night_brightness:20,idle_seconds:60},anniversaries:[{label:'生日',year:2026,month:5,day:1,annual:true},{label:'测试',year:2026,month:6,day:1,annual:false}]};
const state={ok:true,config_revision:123,config:{home_style:1,wallpaper_mode:2,volume:60,brightness:75,wake_enabled:true,tts_rate_percent:100,tts_voice_id:0},tts_voices:[{id:0,label:'固件默认',voice:'Cherry'},{id:1,label:'芊悦',voice:'Cherry'},{id:2,label:'苏瑶',voice:'Serena'},{id:3,label:'晨煦',voice:'Ethan'}],wifi_connected:true,time_valid:true,utc_seconds:1791210000,uptime_seconds:42,free_heap:90000,storage_ready:true,history_bytes:1000,history_archives:1,tasks:[]};
let conflictNext=false;
const location={hash:'#token=0123456789ab',pathname:'/',search:''};
let id=0,jobs=new Map(),calls=[],fail=false,mediaSaved=true;
let timerId=0;const requestTimers=new Map();
const context={document:{getElementById:byId,createElement:()=>new Element(),createTextNode:x=>x,querySelectorAll:()=>[]},location,
 history:{replaceState:(_,__,url)=>{assert.equal(url,'/');location.hash='';}},URLSearchParams,URL,Date,Number,String,Math,JSON,RegExp,Map,Set,TextEncoder,Array,Object,DataView,Uint8Array,Float32Array,Blob,Promise,Error,AbortController,console,window:{},confirm:()=>true,prompt:()=>null,
 setTimeout:(fn,ms)=>{if(ms>=30000){requestTimers.set(++timerId,fn);return timerId;}fn();return 0;},clearTimeout:id=>requestTimers.delete(id)};
context.fetch=async(path,options={})=>{
 assert.equal(location.hash,'');assert(path.startsWith('/api/'));assert.equal(options.headers['X-Media-Token'],undefined);
 if(path==='/api/catalog')return {ok:true,json:async()=>catalog};
 if(path==='/api/state')return {ok:true,json:async()=>state};
 if(path.startsWith('/api/media-job?'))return {ok:true,json:async()=>({done:true,applied:true,saved:mediaSaved,message:mediaSaved?'已执行':'媒体保存失败'})};
 if(path==='/api/command'){
  const body=JSON.parse(options.body);calls.push(body);
  if(body.action==='appearance')catalog.appearance={...body};
  return {ok:true,json:async()=>({job_id:100})};
 }
 if(path==='/api/device'){
  const body=JSON.parse(options.body);calls.push(body);id++;
  let r={ok:true,message:'已保存'};
  if(body.action==='state')r=state;
  if(body.action==='configure'){
   if(conflictNext){state.config.brightness=19;state.config_revision++;conflictNext=false;}
   if(body.expected_revision!==undefined&&body.expected_revision!==state.config_revision)r={ok:false,status:'conflict',message:'设备设置已被其他操作更改，请刷新后核对草稿再保存'};
   else{const {action,expected_revision,...patch}=body;state.config={...state.config,...patch};if(state.config_revision!==undefined)state.config_revision++;if(fail)r={ok:false,message:'NVS写入失败'};}
  }
  if(body.action==='history')r={ok:true,matches:[{event:'用户文本',turn_id:'turn1',utc_seconds:1791210000,text:'<img src=x onerror=alert(1)>',record_id:'h-00000001-00000002',truncated:false}],more:true,next_before_offset:19};
  jobs.set(id,r);return {ok:true,json:async()=>({job_id:id,status:'pending'})};
 }
 if(path.startsWith('/api/job?'))return {ok:true,json:async()=>({status:'done',result:jobs.get(Number(path.split('=')[1]))})};
 throw Error(path);
};
vm.createContext(context);
"""
program=mock+"\nvm.runInContext("+json.dumps(script)+",context);\n"+r"""
(async()=>{
 await context.window.onload();
 for(let i=0;i<8;i++)await new Promise(setImmediate);
 assert.equal(byId('token').value,'');
 assert.equal(Number(byId('wallMode').value),2);
 assert.equal(Number(byId('ttsRate').value),100);assert.equal(Number(byId('ttsVoice').value),0);
 byId('ttsRate').value='125';byId('ttsVoice').value='3';
 await byId('refreshDevice').onclick();
 assert.equal(Number(byId('ttsRate').value),125);assert.equal(Number(byId('ttsVoice').value),3);
 await byId('saveDevice').onclick();
 assert.equal(state.config.tts_rate_percent,125);assert.equal(state.config.tts_voice_id,3);
 byId('ttsRate').value='50';byId('ttsVoice').value='2';byId('resetDevice').onclick();
 assert.equal(Number(byId('ttsRate').value),125);assert.equal(Number(byId('ttsVoice').value),3);
 let voiceSaves=calls.filter(c=>c.action==='configure').length;
 byId('ttsRate').value='201';await byId('saveDevice').onclick();
 assert.equal(calls.filter(c=>c.action==='configure').length,voiceSaves);
 byId('ttsRate').value='101';await byId('saveDevice').onclick();
 assert.equal(calls.filter(c=>c.action==='configure').length,voiceSaves);
 byId('ttsRate').value='125';byId('ttsVoice').value='99';await byId('saveDevice').onclick();
 assert.equal(calls.filter(c=>c.action==='configure').length,voiceSaves);
 byId('resetDevice').onclick();
 vm.runInContext('dateDrafts[0].label.value="草稿"',context);
 catalog.anniversaries[0].label='设备新值';catalog.anniversaries[1].label='外部修改';
 await byId('connect').onclick();
 assert.equal(vm.runInContext('dateDrafts[0].label.value',context),'草稿');
 assert.equal(vm.runInContext('dateDrafts[1].label.value',context),'外部修改');
 byId('homeStyle').value='1';byId('wallMode').value='4';byId('volume').value='70';byId('brightness').value='80';byId('wakeEnabled').checked=false;
 await byId('saveDevice').onclick();
 assert(calls.some(c=>c.action==='configure'&&c.wallpaper_mode===4&&c.volume===70));
 // Switching back retains the chosen wallpaper and saves the explicit style.
 byId('homeStyle').value='0';await byId('saveDevice').onclick();
 assert.equal(state.config.home_style,0);assert.equal(state.config.wallpaper_mode,4);
 assert.equal(Number(byId('homeStyle').value),0);
 assert(!elements.has('wallpaperEnabled'));assert(!elements.has('idleAlbum'));
 byId('nightEnabled').checked=true;byId('nightStart').value='22:00';byId('nightEnd').value='07:00';
 byId('nightBrightness').value='15';byId('idleSeconds').value='30';
 await byId('saveAppearance').onclick();
 assert(calls.some(c=>c.action==='appearance'&&c.idle_seconds===30&&c.night_enabled&&
    c.wallpaper_enabled===true&&c.idle_album===false));
 await byId('historySearch').onclick();assert.equal(byId('historyOlder').disabled,false);
 assert.equal(byId('history').children[0].children[1].textContent,'<img src=x onerror=alert(1)>');
 await byId('historyOlder').onclick();assert(calls.some(c=>c.action==='history'&&c.before===19));
 byId('taskTitle').value='喝水';byId('taskKind').value='alarm';byId('taskDue').value='2026-10-07T09:00';byId('taskRepeat').value='daily';
 await byId('addTask').onclick();
 assert(calls.some(c=>c.action==='task_create'&&c.due_utc===Date.UTC(2026,9,7,1,0)/1000&&c.repeat==='daily'));
 // Refresh keeps a dirty volume but adopts an externally changed brightness.
 byId('volume').value='42';state.config.brightness=33;
 await byId('refreshDevice').onclick();
 assert.equal(Number(byId('volume').value),42);
 assert.equal(Number(byId('brightness').value),33);
 // Save merges only our edited field with the latest device snapshot.
 state.config.brightness=28;await byId('saveDevice').onclick();
 assert.equal(state.config.volume,42);assert.equal(state.config.brightness,28);
 byId('volume').value='17';byId('resetDevice').onclick();
 assert.equal(Number(byId('volume').value),42);
 // Invalid values must not reach the backend.
 const before=calls.filter(c=>c.action==='configure').length;
 byId('volume').value='101';await byId('saveDevice').onclick();
 assert.equal(calls.filter(c=>c.action==='configure').length,before);
 assert(byId('notice').textContent.includes('0–100'));
 await vm.runInContext("command('image',{index:0,wallpaper:true})",context);
 assert.equal(state.config.home_style,1);assert.equal(state.config.wallpaper_mode,0);
 mediaSaved=false;
 await assert.rejects(vm.runInContext("command('pause',{value:0})",context),/媒体保存失败/);
 assert.equal(vm.runInContext('pendingMediaJob',context),0);
 mediaSaved=true;fail=true;
 await assert.rejects(vm.runInContext("command('image',{index:0,wallpaper:true})",context),/图片选择已完成，但主页切换未确认/);
 byId('volume').value='41';await byId('saveDevice').onclick();assert.equal(byId('notice').textContent,'NVS写入失败');
 fail=false;await byId('refreshDevice').onclick();byId('resetDevice').onclick();
 // A no-op must not create a job or rewrite NVS.
 let count=calls.filter(c=>c.action==='configure').length;
 await byId('saveDevice').onclick();assert.equal(calls.filter(c=>c.action==='configure').length,count);
 // Race after GET but before execution: reject, keep draft, refresh actual state.
 byId('volume').value='55';conflictNext=true;
 await byId('saveDevice').onclick();
 assert.equal(state.config.volume,41);assert.equal(state.config.brightness,19);
 assert.equal(Number(byId('volume').value),55);assert.equal(Number(byId('brightness').value),19);
 assert(byId('notice').textContent.includes('核对草稿'));
 assert.equal(calls.filter(c=>c.action==='configure').length,count+1); // no automatic retry
 const patch=calls.filter(c=>c.action==='configure').at(-1);
 assert.deepEqual(Object.keys(patch).sort(),['action','expected_revision','volume']);
 // User reviews and explicitly saves again; backend preserves unrelated value.
 await byId('saveDevice').onclick();assert.equal(state.config.volume,55);assert.equal(state.config.brightness,19);
 // Old firmware still allows ordinary configuration without synthesizing TTS=0.
 delete state.config_revision;delete state.config.tts_rate_percent;delete state.config.tts_voice_id;
 await byId('refreshDevice').onclick();assert(byId('ttsRate').disabled&&byId('ttsVoice').disabled);
 assert.equal(byId('deviceFields').disabled,false);
 byId('volume').value='35';await byId('saveDevice').onclick();
 const legacy=calls.filter(c=>c.action==='configure').at(-1);
 assert(!('expected_revision' in legacy)&&!('tts_rate_percent' in legacy)&&!('tts_voice_id' in legacy));
 assert.equal(state.config.volume,35);assert.equal(state.config.brightness,19);
 assert.equal(requestTimers.size,0);
 // Requests must time out before headers OR while consuming a stalled body.
 for(const phase of ['headers','body']){
  let sent=0;
  context.fetch=async(path,options)=>{
   sent++;
   const stall=()=>new Promise((_,reject)=>options.signal.addEventListener('abort',()=>reject(Error('aborted')),{once:true}));
   return phase==='headers'?stall():{ok:true,json:stall};
  };
  const pending=vm.runInContext('guard(()=>request("/api/device",{method:"POST"}))',context);
  await new Promise(setImmediate);
  assert.equal(requestTimers.size,1);assert(vm.runInContext('busy',context));
  for(const expire of requestTimers.values())expire();
  await pending;
  assert.equal(sent,1);assert.equal(requestTimers.size,0);assert(!vm.runInContext('busy',context));
  assert(byId('notice').textContent.includes('设备可能已执行'));
 }
 context.fetch=async()=>({ok:true,json:async()=>({job_id:0})});
 await assert.rejects(vm.runInContext('device("configure",{volume:10})',context),/有效操作编号/);
 assert.equal(vm.runInContext('pendingJob',context),0);
 console.log('PASS: mocked browser QR/config/task/history, safe rendering, response timeouts and no automatic mutation replay');
})().catch(e=>{console.error(e);process.exitCode=1;});
"""
subprocess.run([node],input=program,text=True,check=True)
print("PASS: bounded session-gated coordinator, NVS/media receipts, wallpaper readback, four animations and pure-black asset margins; C/device NOT tested")
