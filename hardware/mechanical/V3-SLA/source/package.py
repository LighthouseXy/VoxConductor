from pathlib import Path
import json,hashlib,zipfile,argparse
R=Path(__file__).resolve().parents[1]
args=argparse.ArgumentParser()
args.add_argument('--output', type=Path, help='Optional ZIP directory outside this design tree')
output=args.parse_args().output
if output:
 output=output.resolve()
 assert not output.is_relative_to(R), 'ZIP output must be outside the design tree'
 output.mkdir(parents=True,exist_ok=True)
files=sorted(p for p in R.rglob('*') if p.is_file() and p.suffix not in ['.zip','.FCStd1','.FCBak','.pyc'] and p.name not in ['manifest.json','.DS_Store'] and '__pycache__' not in p.parts)
manifest={'version':'V3-SLA-20261011','status':'CAD_VALIDATED_PHYSICAL_FIT_PENDING','reprint':['01_main_shell','02_base','04_speaker_cradle'],'reuse':['03_LCD_retainer','05_speaker_retainer','06_prism_carrier'],'files':{str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
(R/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False))
def pack(name,paths):
 with zipfile.ZipFile(output/name,'w',zipfile.ZIP_DEFLATED) as z:
  for p in paths:z.write(p,p.relative_to(R))
 with zipfile.ZipFile(output/name) as z:assert z.testzip() is None
if output:
 pack('01_先试装_入口框与底盖.zip',[R/'README.md',R/'print/02_base.stl',R/'trial/07_entry_frame_first_print.stl',R/'trial/08_pilot_coupon_1p5_1p6_1p7.stl'])
 pack('02_试装通过后_替换三件.zip',[R/'README.md']+[R/'print'/f'{n}.stl' for n in manifest['reprint']])
 pack('V3-SLA_完整设计资料.zip',files+[R/'manifest.json'])
 print('Packaged',len(files),'files')
