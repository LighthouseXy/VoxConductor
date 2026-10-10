"""Independent export/fastener checks, no edits to model geometry."""
import sys,json,collections,hashlib
from pathlib import Path
sys.path.append('/Applications/FreeCAD.app/Contents/Resources/lib')
import FreeCAD as A,Part,Mesh
V=A.Vector;R=Path(__file__).resolve().parents[1];d=json.loads((R/'source/pcb_actual.json').read_text())
report={'meshes':{},'unchanged':{},'fasteners':[],'ports':{}}
def cyl(r,h,x,y,z):return Part.makeCylinder(r,h,V(x,y,z))
def box(x0,y0,z0,x1,y1,z1):return Part.makeBox(x1-x0,y1-y0,z1-z0,V(x0,y0,z0))
parts={p.stem:Part.read(str(p)) for p in (R/'parts').glob('*.step')}
for p in list((R/'print').glob('*.stl'))+list((R/'trial').glob('*.stl')):
 mesh=Mesh.Mesh(str(p));v,f=mesh.Topology;edges=collections.Counter(tuple(sorted((a,b))) for tri in f for a,b in zip(tri,tri[1:]+tri[:1]));assert all(c==2 for c in edges.values()),p
 step=R/('parts' if p.parent.name=='print' else 'trial')/(p.stem+'.step');solid=Part.read(str(step));err=abs(mesh.Volume-solid.Volume)/solid.Volume;assert err<.005,(p,err)
 report['meshes'][p.name]={'closed_two_faces_per_edge':True,'relative_volume_error':err,'facets':len(f)}
for n in ['03_LCD_retainer','05_speaker_retainer','06_prism_carrier']:
 a=parts[n];b=Part.read(str(R/'source/frozen_v2'/f'{n}.step'));delta=a.cut(b).Volume+b.cut(a).Volume;assert delta<1e-5;report['unchanged'][n]=delta
poly=Part.Face(Part.makePolygon([V(x,y,5) for x,y in d['outline']]+[V(*d['outline'][0],5)]));pcb=poly.extrude(V(0,0,d['board_thickness']))
for x,y in [(x,y) for y in (12,61.5) for x in (-25.5,25.5)]:
 screw=cyl(1.79,.21,x,y,.3).fuse(Part.makeCone(1.79,1,.79,V(x,y,.51))).fuse(cyl(1,3,x,y,1.3))
 v=parts['02_base'].common(screw).Volume;gap=pcb.distToShape(screw)[0];assert v<1e-5 and gap>.5,(v,gap)
 report['fasteners'].append({'center':[x,y],'2x4_tip_z':4.3,'base_overlap_mm3':v,'pcb_gap_mm':gap})
# Connector opening/plug envelopes cross actual outer walls; no tabletop geometry guessing.
ux=next(c['xy'][0] for c in d['components'] if c['ref']=='J1')
for n,env in [('USB_plug_overmould_14x8',box(ux-7,62,4.25,ux+7,85,12.25)),('SD_card_exit',box(-45,33,6.8,-28.5,45,8.2))]:
 vol=parts['01_main_shell'].common(env).Volume;assert vol<1e-5,(n,vol);report['ports'][n]=vol
report['limits']='14x8 USB plug is a stated envelope; actual user cable and card finger grip still need physical trial.'
(R/'checks/delivery_validation.json').write_text(json.dumps(report,indent=2))
print('DELIVERY CHECK PASS',len(report['meshes']),'meshes; unchanged optics; fasteners and port access')
