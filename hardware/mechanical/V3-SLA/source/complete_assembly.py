import sys,json,math,argparse
from pathlib import Path
sys.path.append('/Applications/FreeCAD.app/Contents/Resources/lib')
import FreeCAD as A,Part
R=Path(__file__).resolve().parents[1];V=A.Vector;T=math.tan(math.radians(25));O=V(0,37,43-(37-(41*T-.6))*T)
def box(x0,y0,z0,x1,y1,z1):return Part.makeBox(x1-x0,y1-y0,z1-z0,V(x0,y0,z0))
def op(s):s.rotate(V(0,0,0),V(1,0,0),-25);s.translate(O);return s
extras={'LCD_reference':op(box(-15.71,-16.835,-2.5,15.71,16.835,-.33)),'prism_reference':op(box(-15,-16.475,1.66,15,13.525,31.66)),'speaker_reference':box(-15.1,32.4,12,15.1,52.6,19.1)}
parser=argparse.ArgumentParser();parser.add_argument('--check-only',action='store_true');args=parser.parse_args()
doc=A.openDocument(str(R/'assembly/V3_assembly.FCStd'));scene=json.loads((R/'preview/scene.json').read_text());checks={}
# FreeCAD sanitizes 01_main_shell to _01_main_shell internally; labels keep the public part names.
parts={ob.Label:ob for ob in doc.Objects if ob.Label[:2].isdigit()}
assert len(parts)==6,('Expected six assembly parts',list(parts))
for name,s in extras.items():
 for label,ob in parts.items():
  v=s.common(ob.Shape).Volume;checks[name+'/'+label]=v
(R/'checks/optical_speaker_validation.json').write_text(json.dumps(checks,indent=2))
assert len(checks)==18
assert all(v<1e-5 for v in checks.values()),{k:v for k,v in checks.items() if v>=1e-5}
if args.check_only:
 print('Optical and speaker reference checks PASS:',len(checks),'pairs; geometry unchanged')
 sys.exit(0)
for name,s in extras.items():
 ob=doc.getObject(name) or doc.addObject('PartDesign::Feature',name);ob.Shape=s;ob.Label=name
 v,f=s.tessellate(.15);scene[name]={'vertices':[[p.x,p.y,p.z] for p in v],'faces':f}
doc.recompute();doc.save();Part.export(doc.Objects,str(R/'assembly/V3_assembly_DO_NOT_PRINT.step'))
(R/'preview/scene.json').write_text(json.dumps(scene))
print('Optical and speaker reference checks PASS:',len(checks),'pairs')
