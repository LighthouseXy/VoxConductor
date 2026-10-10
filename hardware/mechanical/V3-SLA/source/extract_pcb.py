"""Actual front-face coordinates: CAD X=195-KiCad X, Y=KiCad Y+92, Z up."""
import pcbnew,json,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parent
src=ROOT.parents[2]/'VoxConductor.kicad_pcb'
b=pcbnew.LoadBoard(str(src))
def xy(p):return [195-pcbnew.ToMM(p.x),pcbnew.ToMM(p.y)+92]
edges=[(xy(g.GetStart()),xy(g.GetEnd())) for g in b.GetDrawings() if g.GetLayer()==pcbnew.Edge_Cuts]
outline=[edges.pop(0)[0]]
# Build one closed polygon independent of segment direction.
edges=[(xy(g.GetStart()),xy(g.GetEnd())) for g in b.GetDrawings() if g.GetLayer()==pcbnew.Edge_Cuts]
while edges:
 for i,(a,c) in enumerate(edges):
  if a==outline[-1] or c==outline[-1]:
   outline.append(c if a==outline[-1] else a);edges.pop(i);break
 else:raise ValueError('non-connected Edge.Cuts')
assert outline[-1]==outline[0]
outline.pop()
heights={'U1':3.2,'U2':1.5,'U3':1.0,'U4':1.5,'U5':1.1,'U6':1.0,'L1':2.0,'J1':3.3,'J2':3.6,'J3':3.4,'J4':2.1,'J7':4.8,'SW1':3.6,'SW2':3.6}
components=[];mounts=[];pads={}
for f in b.GetFootprints():
 ref=f.GetReference(); pads[ref]=[{'number':p.GetNumber(),'xy':xy(p.GetPosition()),'net':p.GetNetname(),'size':[pcbnew.ToMM(p.GetSize().x),pcbnew.ToMM(p.GetSize().y)]} for p in f.Pads()]
 if ref.startswith('H'):
  mounts.append({'ref':ref,'xy':xy(f.GetPosition())});continue
 if f.GetValue().upper()=='DNP':continue
 pts=[]
 for g in f.GraphicalItems():
  if g.GetLayer()==pcbnew.F_Fab and isinstance(g,pcbnew.PCB_SHAPE):
   bb=g.GetBoundingBox();pts.extend([xy(bb.GetPosition()),xy(bb.GetEnd())])
 for p in f.Pads():
  bb=p.GetBoundingBox();pts.extend([xy(bb.GetPosition()),xy(bb.GetEnd())])
 if not pts:continue
 xs,ys=zip(*pts)
 assert max(xs)-min(xs)<40 and max(ys)-min(ys)<40,(ref,pts)
 components.append({'ref':ref,'value':f.GetValue(),'xy':xy(f.GetPosition()),'bounds':[min(xs),min(ys),max(xs),max(ys)],'height_envelope':heights.get(ref,2.0 if ref.startswith('C') else 1.25),'kicad_rotation':f.GetOrientationDegrees()})
mic=next(p['xy'] for p in pads['U5'] if p['number']=='')
d={'source':'hardware/VoxConductor.kicad_pcb','sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'coordinate_mapping':'X=195-x; Y=y+92; component side +Z; rear view looking along -Y: +X appears left','board_bottom_z':5,'board_thickness':pcbnew.ToMM(b.GetDesignSettings().GetBoardThickness()),'outline':outline,'mounts':mounts,'mic':mic,'components':components,'pads':pads,'limits':'Body/pad XY envelopes from PCB; heights conservative estimates, not supplier assembly CAD. Cable dressing not measured.'}
(ROOT/'pcb_actual.json').write_text(json.dumps(d,ensure_ascii=False,indent=2))
print('outline',outline,'mounts',mounts,'mic',mic)
for c in components:
 if c['ref'].startswith('J') or c['ref']=='U1':print(c)
