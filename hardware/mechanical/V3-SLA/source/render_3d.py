"""Orthographic CAD triangle render with per-pixel depth (no invented geometry)."""
from pathlib import Path
import json,math
import numpy as np
from PIL import Image,ImageDraw,ImageFont
R=Path(__file__).resolve().parents[1];scene=json.loads((R/'preview/scene.json').read_text())
colors={'01_main_shell':(188,207,217),'02_base':(69,120,143),'03_LCD_retainer':(218,167,95),'04_speaker_cradle':(146,166,184),'05_speaker_retainer':(102,125,147),'06_prism_carrier':(202,218,226),'PCB':(43,139,89),'U1':(188,190,194),'J1':(220,145,63),'J4':(211,90,111),'J2':(239,197,76),'LCD_reference':(36,42,52),'prism_reference':(106,180,194),'speaker_reference':(40,44,49)}
font='/System/Library/Fonts/Supplemental/Arial Unicode.ttf'
def render(filename,names,title,explode):
 az=math.radians(110);el=math.radians(36);view=np.array([math.cos(az)*math.cos(el),math.sin(az)*math.cos(el),math.sin(el)])
 right=np.array([-math.sin(az),math.cos(az),0]);up=np.cross(view,right)
 tris=[];cols=[]
 for n in names:
  verts=np.array(scene[n]['vertices']);f=np.array(scene[n]['faces'])
  if explode:
   dz=38 if n.startswith(('03','06')) or n in ('LCD_reference','prism_reference') else 19 if n.startswith(('04','05')) or n=='speaker_reference' else 0
   verts[:,2]+=dz
  t=verts[f];norm=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]);norm/=np.maximum(np.linalg.norm(norm,axis=1,keepdims=True),1e-12)
  light=np.array([-.3,.5,1]);light/=np.linalg.norm(light);shade=.52+.48*np.abs(norm@light)
  cols.extend(np.clip(np.array(colors.get(n,(125,134,141)))[None,:]*shade[:,None],0,255));tris.extend(t)
 tris=np.array(tris);cols=np.array(cols);project=np.stack([tris@right,-tris@up,tris@view],axis=-1)
 lo=project[:,:,:2].min(axis=(0,1));hi=project[:,:,:2].max(axis=(0,1));scale=min(1050/(hi[0]-lo[0]),900/(hi[1]-lo[1]));project[:,:,:2]=(project[:,:,:2]-(lo+hi)/2)*scale+[600,550]
 W,H=1200,1120;rgb=np.full((H,W,3),247,dtype=np.uint8);depth=np.full((H,W),-np.inf)
 for t,c in zip(project,cols):
  x0=max(0,int(np.floor(t[:,0].min())));x1=min(W-1,int(np.ceil(t[:,0].max())));y0=max(0,int(np.floor(t[:,1].min())));y1=min(H-1,int(np.ceil(t[:,1].max())))
  if x1<x0 or y1<y0:continue
  a,b,q=t;den=(b[1]-q[1])*(a[0]-q[0])+(q[0]-b[0])*(a[1]-q[1])
  if abs(den)<1e-9:continue
  yy,xx=np.mgrid[y0:y1+1,x0:x1+1];xx=xx+.5;yy=yy+.5
  w0=((b[1]-q[1])*(xx-q[0])+(q[0]-b[0])*(yy-q[1]))/den;w1=((q[1]-a[1])*(xx-q[0])+(a[0]-q[0])*(yy-q[1]))/den;w2=1-w0-w1
  z=w0*a[2]+w1*b[2]+w2*q[2];buf=depth[y0:y1+1,x0:x1+1];mask=(w0>=-1e-7)&(w1>=-1e-7)&(w2>=-1e-7)&(z>buf);buf[mask]=z[mask];rgb[y0:y1+1,x0:x1+1][mask]=c
 im=Image.fromarray(rgb);draw=ImageDraw.Draw(im);draw.text((35,25),title,font=ImageFont.truetype(font,32),fill=(27,49,65));draw.text((35,1058),'USB 后侧视角：USB 左 / SD 右；依据真实 CAD 网格渲染',font=ImageFont.truetype(font,23),fill=(43,57,65));im.save(R/'preview'/filename)
render('01_shell.png',['01_main_shell','02_base'],'V3：修正接口方向，螺丝柱移出装入通道',False)
render('02_internal_exploded.png',[n for n in scene if n not in ['01_main_shell','entry_frame']],'内部装配分解：外壳隐藏，电子器件为包络参考',True)
print('depth rendered')
