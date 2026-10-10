"""V3: repaired front-face mapping and unobstructed vertical board insertion.
Run with FreeCAD bundled Python. Original V2 files are read-only inputs.
"""
import sys,json,math,hashlib
from pathlib import Path
sys.path.append('/Applications/FreeCAD.app/Contents/Resources/lib')
import FreeCAD as A,Part,MeshPart,Mesh
V=A.Vector
ROOT=Path(__file__).resolve().parents[1]
for f in ['parts','print','assembly','checks','preview','trial','source/frozen_v2']: (ROOT/f).mkdir(exist_ok=True,parents=True)
d=json.loads((ROOT/'source/pcb_actual.json').read_text());th=d['board_thickness'];zboard=5.;ztop=5+th
T=math.tan(math.radians(25));FT=41*T-.6;O=V(0,37,43-(37-FT)*T)
def box(a,b,c,x,y,z):return Part.makeBox(x-a,y-b,z-c,V(a,b,c))
def cyl(r,h,x,y,z):return Part.makeCylinder(r,h,V(x,y,z))
def wire(p):return Part.makePolygon([V(*x) for x in p]+[V(*p[0])])
def poly(xy,z,h):return Part.Face(wire([(x,y,z) for x,y in xy])).extrude(V(0,0,h))
def octa(hw,f,r,cut,z):return [(-hw+cut,f,z),(hw-cut,f,z),(hw,f+cut,z),(hw,r-cut,z),(hw-cut,r,z),(-hw+cut,r,z),(-hw,r-cut,z),(-hw,f+cut,z)]
def ring(z,inside=0):return octa(31-inside,(z-2)*T-.6+inside,65-inside,8,z)
def loft(rings):return Part.makeLoft([wire(p) for p in rings],True,True)
def mirror(s):
 m=A.Matrix();m.A11=-1
 return s.transformGeometry(m)
def moved(s,dz):
 t=s.copy();t.translate(V(0,0,dz));return t

def optical(s):
 t=s.copy();t.rotate(V(0,0,0),V(1,0,0),-25);t.translate(O);return t
old={}
for n in ['01_main_shell','02_base','03_LCD_retainer','04_speaker_cradle','05_speaker_retainer','06_prism_carrier']:
 src=ROOT/'source/frozen_v2'/f'{n}.step'
 assert src.is_file(), f'Missing rebuild input: {src}'
 old[n]=Part.read(str(src))
print('inputs loaded',flush=True)
# Keep optical roof, button, upper grille and fastening geometry; rebuild entire lower skirt.
upper=mirror(old['01_main_shell']).common(box(-80,-20,14,80,100,100))
outer=loft([octa(32.5,4.4,68,6,2),octa(32.5,4.4,68,6,6),ring(14)])
inner=loft([octa(30.1,6.8,65.6,6,1.8),octa(30.1,6.8,65.6,6,6),ring(14,2.4)])
shell=upper.fuse(outer.cut(inner)).removeSplitter()
# Boss centers outside board swept outline, with full round pillars and outer webs.
centers=[(x,y) for y in (12.,61.5) for x in (-25.5,25.5)]
bosses=[]
for x,y in centers:
 boss=cyl(3.,4.2,x,y,2)
 web=box(x-2,4.4,2,x+2,y,6.2) if y<30 else box(x-2,y,2,x+2,68,6.2)
 boss=boss.fuse(web).common(outer).removeSplitter();bosses.append(boss)
 shell=shell.fuse(boss).cut(cyl(.8,4.4,x,y,1.9))
# Original speaker shelves/side holes kept in position, mirrored with wiring side.
for side in (-1,1):
 anchor=box(26.3 if side>0 else -31,47.5,10,31 if side>0 else -26.3,52.5,16)
 shelf=box(24.5 if side>0 else -28,47.8,8.6,28 if side>0 else -24.5,52.2,10)
 shell=shell.fuse(anchor).fuse(shelf)
 hole=Part.makeCylinder(1.15,16,V(-38 if side<0 else 22,50,13),V(1,0,0));shell=shell.cut(hole)
# USB overmould reaches recessed connector; bottom-open throat admits installed connector.
ux=next(c['xy'][0] for c in d['components'] if c['ref']=='J1')
shell=shell.cut(box(ux-5.7,59,0,ux+5.7,85,11.2))
shell=shell.cut(box(ux-7,62,4.2,ux+7,85,12.3))
# SD insertion/removal including fingers and card body; slot opens to bottom for board assembly.
shell=shell.cut(box(-40,31.4,0,-24,46.8,10.7)).cut(box(-40,29.8,2,-29.3,48.4,12.2))
# Corrected cable side, with conservative mated-plug room.
shell=shell.cut(box(25.5,27,7,29.2,47,14))
base=poly([p[:2] for p in octa(32.2,4.7,67.7,6,0)],0,1.8)
for m in d['mounts']:
 x,y=m['xy'];base=base.fuse(cyl(2.45,3.2,x,y,1.8)).cut(cyl(.8,4.6,x,y,.6))
mx,my=d['mic'];base=base.fuse(cyl(2,2.95,mx,my,1.8)).cut(cyl(1,8,mx,my,-2))
for x,y in centers:
 base=base.fuse(cyl(3,.2,x,y,1.8))
 base=base.cut(cyl(1.15,3,x,y,-.1)).cut(cyl(1.9,.5,x,y,-.1)).cut(Part.makeCone(1.9,1.15,.75,V(x,y,.4)))
# SD sill stays below connector swept envelope and follows base, not shell.
base=base.fuse(box(-32.2,31.55,1.8,-30.3,46.65,5.1))
for x,y in [(-20,9),(20,9),(-20,64),(20,64)]:base=base.fuse(cyl(3,1.3,x,y,-1.3))
# Correct XY board envelope, offset 0.5 mm on all sides. Sweep is continuous, not sampled only.
board_face=Part.Face(wire([(x,y,0) for x,y in d['outline']]))
off=board_face.makeOffset2D(.5,0,False,False,False)
if off.Area<board_face.Area:off=board_face.makeOffset2D(-.5,0,False,False,False)
assert off.Area>board_face.Area
pcb=poly(d['outline'],5,th)
board_sweep=off.extrude(V(0,0,ztop+40));board_sweep.translate(V(0,0,-40))
# Component envelopes use fresh PCB bounds (including pads) and stated height estimates.
electronics={};sweeps={'PCB_0p5_clearance':board_sweep}
for c in d['components']:
 x0,y0,x1,y1=c['bounds'];h=c['height_envelope'];n=c['ref']
 electronics[n]=box(x0,y0,ztop,x1,y1,ztop+h)
 # 0.5 lateral, 0.3 vertical allowance for placement/solder; pad bounding boxes conservative.
 sweeps[n]=box(x0-.5,y0-.5,-40,x1+.5,y1+.5,ztop+h+.3)
# Mated connectors and wire-turn reserves; no claim on actual cable/flex bending.
electronics['J3_mated']=box(20.5,28,ztop,26.4,36.4,ztop+3.4)
electronics['J7_mated']=box(16.8,40,ztop,26.4,46,ztop+4.8)
for n in ['J3_mated','J7_mated']:
 b=electronics[n].BoundBox;sweeps[n]=box(b.XMin-.5,b.YMin-.5,-40,b.XMax+.5,b.YMax+.5,b.ZMax+.3)
# Relieve only true collisions with conservative sweep; report each cut, preserve source proof.
cutlog={}
for n,s in sweeps.items():
 overlap=shell.common(s).Volume
 if overlap>1e-6:
  cutlog[n]=overlap;shell=shell.cut(s)
shell=shell.removeSplitter();base=base.removeSplitter()
parts={'01_main_shell':shell,'02_base':base,'03_LCD_retainer':old['03_LCD_retainer'],'04_speaker_cradle':mirror(old['04_speaker_cradle']),'05_speaker_retainer':old['05_speaker_retainer'],'06_prism_carrier':old['06_prism_carrier']}
# Relieve the inherited tray locally against the full installed-board sweep.
tray=parts['04_speaker_cradle'];tray_relief={}
for n,sweep in sweeps.items():
 v=tray.common(sweep).Volume
 if v>1e-6:
  tray_relief[n]=v;tray=tray.cut(sweep)
parts['04_speaker_cradle']=tray.removeSplitter()
# Short full-perimeter fit frame retains actual screw bosses and entry geometry.
trial=shell.common(box(-80,-20,1.9,80,100,12)).removeSplitter()
print('geometry built; exporting',cutlog,flush=True)
report={'coordinate_mapping':d['coordinate_mapping'],'pcb_sha256':d['sha256'],'board_mm':[52,50,th],'clearance_mm':{'board_xy':.5,'component_xy':.5,'component_top':.3},'new_bottom_screw_centers':centers,'sweep_relief_mm3':cutlog,'tray_relief_mm3':tray_relief,'parts':{},'sweep_collisions_mm3':{},'final_collisions_mm3':{},'limits':['Component heights are envelope estimates, not measured assemblies.','Physical resin shrinkage, thread strength, flex length and cable bends require fit test.','03/05/06 reused unchanged; optical fit unchanged and not newly qualified.']}
def export(n,s,folder='parts'):
 assert s.isValid() and len(s.Solids)==1,(n,'invalid solid',len(s.Solids))
 path=ROOT/folder/f'{n}.step';s.exportStep(str(path));r=Part.read(str(path))
 assert r.isValid() and len(r.Solids)==1
 bottom=moved(r,-r.BoundBox.ZMin)
 mesh=MeshPart.meshFromShape(Shape=bottom,LinearDeflection=.04,AngularDeflection=.12,Relative=False)
 out=ROOT/('print' if folder=='parts' else folder)/f'{n}.stl';mesh.write(str(out))
 loaded=Mesh.Mesh(str(out))
 assert loaded.isSolid() and loaded.Volume>0,(n,'bad mesh')
 report['parts'][n]={'valid':r.isValid(),'solids':len(r.Solids),'stl_solid':loaded.isSolid(),'volume_mm3':r.Volume,'bounds_mm':[(min if axis<3 else max)(getattr(v, 'xyz'[axis%3]) for v in r.tessellate(.04)[0]) for axis in range(6)]}
 return r
for n,s in parts.items():parts[n]=export(n,s)
export('07_entry_frame_first_print',trial,'trial')
# Hole fit coupon for selected resin, separate from assembled product.
coupon=box(-15,-5,0,15,5,5)
for x,r in [(-10,.75),(0,.8),(10,.85)]:coupon=coupon.cut(cyl(r,4,x,0,1))
export('08_pilot_coupon_1p5_1p6_1p7',coupon,'trial')
# Independent round-trip checks on actual final exported STEP, continuous insertion volume.
print('round-trip checks',flush=True)
for n,s in sweeps.items():
 v=parts['01_main_shell'].common(s).Volume;report['sweep_collisions_mm3'][n]=v
 assert v<1e-5,('sweep',n,v)
placed={n:optical(s) if n.startswith(('03','06')) else s for n,s in parts.items()}
for n in ['04_speaker_cradle','05_speaker_retainer']:
 for ref,s in sweeps.items():
  # Board enters shell after tray is secured; needs same continuous path.
  v=placed[n].common(s).Volume;report['sweep_collisions_mm3'][n+'/'+ref]=v
  assert v<1e-5,('tray sweep',n,ref,v)
for i,(n,s) in enumerate(placed.items()):
 for m,t in list(placed.items())[i+1:]:
  v=s.common(t).Volume;report['final_collisions_mm3'][n+'/'+m]=v
  assert v<1e-5,('plastic pair',n,m,v)
 for ref,t in {'PCB':pcb,**electronics}.items():
  v=s.common(t).Volume;report['final_collisions_mm3'][n+'/'+ref]=v
  assert v<1e-5,('final',n,ref,v)
for i,b in enumerate(bosses):
 report.setdefault('boss_board_sweep_gap_mm',[]).append(b.distToShape(board_sweep)[0])
 assert b.distToShape(board_sweep)[0]>.2
# References keep labels and actual coordinates in a native editable assembly.
doc=A.newDocument('V3_SLA_corrected')
for n,s in {**placed,'PCB_reference':pcb,**electronics}.items():
 ob=doc.addObject('PartDesign::Feature',n);ob.Label=n;ob.Shape=s
# Complete source assembly excludes fictitious mirrored solid electronic models; these are envelopes.
doc.recompute();doc.saveAs(str(ROOT/'assembly/V3_assembly.FCStd'))
Part.export(doc.Objects,str(ROOT/'assembly/V3_assembly_DO_NOT_PRINT.step'))
# Export tessellation for precise rendered previews.
scene={}
for n,s in {**placed,'PCB':pcb,**electronics,'entry_frame':trial}.items():
 v,f=s.tessellate(.15);scene[n]={'vertices':[[p.x,p.y,p.z] for p in v],'faces':f}
(ROOT/'preview/scene.json').write_text(json.dumps(scene))
(ROOT/'checks/validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
print('PASS',json.dumps({k:v for k,v in report.items() if k in ['boss_board_sweep_gap_mm','parts']},ensure_ascii=False),flush=True)
