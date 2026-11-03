// Local-only asset generation, no firmware compilation and no network/API calls.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const main=path.join(root,'firmware/main');
const run=(cmd,args)=>{const r=spawnSync(cmd,args,{stdio:'inherit'});if(r.status!==0)throw new Error(`${cmd} failed`);};
if(process.argv.includes('--voice')) {
  const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'vox-reminder-'));
  try {
    run('/usr/bin/say',['-v','Tingting','-r','175','-o',path.join(tmp,'reminder.aiff'),'时间到了，请查看屏幕上的提醒。']);
    run('/usr/bin/afconvert',['-f','WAVE','-d','LEI16@24000','-c','1',path.join(tmp,'reminder.aiff'),path.join(tmp,'reminder.wav')]);
    const wav=fs.readFileSync(path.join(tmp,'reminder.wav'));let pcm;
    for(let offset=12;offset+8<=wav.length;){
      const len=wav.readUInt32LE(offset+4),id=wav.toString('ascii',offset,offset+4);
      if(id==='fmt ' && (wav.readUInt16LE(offset+8)!==1 || wav.readUInt16LE(offset+10)!==1 || wav.readUInt32LE(offset+12)!==24000)) throw new Error('Unexpected WAV format');
      if(id==='data') pcm=wav.subarray(offset+8,offset+8+len);
      offset+=8+len+(len&1);
    }
    if(!pcm || pcm.length>480000 || pcm.length%2) throw new Error('Invalid PCM');
    fs.writeFileSync(path.join(main,'reminder.pcm'),pcm);
    console.log(`Local reminder: ${pcm.length} bytes, ${pcm.length/48000}s`);
  } finally {fs.rmSync(tmp,{recursive:true,force:true});}
}
const converter=process.env.LV_FONT_CONV;
if(converter) {
  let characters='：；？！、（）【】“”…—°';
  for(const filename of ['app_ui.c','app_ui_theme.h','app_desktop.c','app_words.c','app_games.c','main.c','app_media.c','app_media_web.c','app_control.c','app_weather.c','app_wake.h']) {
    const source=fs.readFileSync(path.join(main,filename),'utf8');
    for(const literal of source.matchAll(/"(?:\\.|[^"\\])*"/g)) {
      for(const c of literal[0]) if(c.codePointAt(0)>127) characters+=c;
    }
  }
  // Retain existing coverage including common reminder punctuation and words.
  const existing=fs.readFileSync(path.join(main,'app_ui_font_14.c'),'utf8');
  for(const match of existing.matchAll(/\/\* U\+([0-9A-Fa-f]+)/g)) characters+=String.fromCodePoint(parseInt(match[1],16));
  characters=[...new Set(characters)].filter(c=>c.codePointAt(0)>127).join('');
  for(const size of [14,16,20]) {
    const name=`app_ui_font_${size}`,output=path.join(main,`${name}.c`);
    run(process.execPath,[converter,'--size',String(size),'--bpp','4','--format','lvgl','--font',path.join(root,'firmware/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf'),'-r','0x20-0x7F','--symbols',characters,'--lv-font-name',name,'--lv-fallback','lv_font_source_han_sans_sc_14_cjk','--lv-include','lvgl.h','--no-compress','--no-prefilter','--force-fast-kern-format','-o',output]);
    fs.writeFileSync(output,fs.readFileSync(output,'utf8').trimEnd()+'\n');
  }
  const output=path.join(main,'app_ui_font_digits_56.c');
  run(process.execPath,[converter,'--size','56','--bpp','4','--format','lvgl','--font',path.join(root,'firmware/managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf'),'-r','0x20,0x2D,0x30-0x3A,0xB0','--lv-font-name','app_ui_font_digits_56','--lv-include','lvgl.h','--no-compress','--no-prefilter','--force-fast-kern-format','-o',output]);
  fs.writeFileSync(output,fs.readFileSync(output,'utf8').trimEnd()+'\n');
}
