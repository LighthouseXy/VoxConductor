import os,json,sys
from pathlib import Path
os.environ['MPLCONFIGDIR']='/tmp/vox-v3-mpl'
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from matplotlib.colors import to_rgb
ROOT=Path(__file__).resolve().parents[1]
scene=json.loads((ROOT/'preview/scene.json').read_text());pcb=json.loads((ROOT/'source/pcb_actual.json').read_text())
plt.rcParams['font.sans-serif']=['Arial Unicode MS','PingFang SC','DejaVu Sans'];plt.rcParams['axes.unicode_minus']=False
colors={'01_main_shell':'#c6d7df','02_base':'#44758b','03_LCD_retainer':'#e1b371','04_speaker_cradle':'#9aafc0','05_speaker_retainer':'#687e95','06_prism_carrier':'#c7d6e2','LCD_reference':'#343c4a','prism_reference':'#77becb','speaker_reference':'#25272c','PCB':'#25855b','U1':'#b8b9ba','J1':'#d68742','J4':'#dc6470','J2':'#f3c959'}
def add(ax,n,dz=0,alpha=1):
 s=scene[n];v=np.array(s['vertices']);v[:,2]+=dz;f=np.array(s['faces']);tris=v[f]
 normals=np.cross(tris[:,1]-tris[:,0],tris[:,2]-tris[:,0]);normals/=np.maximum(np.linalg.norm(normals,axis=1,keepdims=True),1e-10)
 light=np.array([.4,.3,1]);light/=np.linalg.norm(light);shade=.5+.5*np.abs(normals@light)
 rgb=np.array(to_rgb(colors.get(n,'#8d9296')));fc=np.clip(shade[:,None]*rgb,0,1)
 ax._all_triangles.append(tris);ax._all_colors.append(fc)

def fig3(name,names,title,explode=False):
 fig=plt.figure(figsize=(11,9));ax=fig.add_subplot(projection='3d');ax.set_proj_type('ortho');ax._all_triangles=[];ax._all_colors=[]
 for n in names:
  dz=(32 if n.startswith(('03','06')) or n in ['LCD_reference','prism_reference'] else 15 if n.startswith(('04','05')) or n=='speaker_reference' else 0) if explode else 0
  add(ax,n,dz)
 ax.add_collection3d(Poly3DCollection(np.concatenate(ax._all_triangles),facecolors=np.concatenate(ax._all_colors),edgecolors='none',zsort='average'))
 ax.set(xlim=(-39,39),ylim=(0,78),zlim=(-3,102 if explode else 52));ax.set_box_aspect((78,78,88 if explode else 55));ax.view_init(elev=43,azim=110)
 ax.set_xlabel('X / mm');ax.set_ylabel('Y / mm');ax.set_zlabel('Z / mm');ax.set_title(title,fontsize=16,pad=15)
 fig.text(.07,.03,'从 USB 后侧看：USB 在左、SD 在右。电子器件为包络参考，不是厂家精细模型。',fontsize=11)
 fig.savefig(ROOT/'preview'/name,dpi=170,bbox_inches='tight');plt.close(fig)
fig3('01_shell.png',['01_main_shell','02_base'],'V3 修正版：USB 口移到正确侧，底部螺丝柱外移')
fig3('02_internal_exploded.png',[n for n in scene if n not in ['01_main_shell','entry_frame']],'内部装配分解示意（外壳隐藏）',True)
# Accurate rear-view-oriented XY plan: +X to page left and antenna to page top.
fig,ax=plt.subplots(figsize=(10,10));p=np.array(pcb['outline']);ax.fill(-p[:,0],p[:,1],color='#e3f3eb',ec='#278050',lw=2,label='52×50 PCB轮廓')
for x,y in [(x,y) for y in (12,61.5) for x in (-25.5,25.5)]:
 ax.add_patch(plt.Circle((-x,y),3,color='#dce7ef',ec='#234962',lw=2));ax.add_patch(plt.Circle((-x,y),.8,color='white',ec='#234962'))
 ax.text(-x,y-4,'底盖柱',ha='center',fontsize=10)
for c in pcb['components']:
 if c['ref'] not in ['U1','J1','J2','J3','J4','J7','U5']:continue
 x0,y0,x1,y1=c['bounds'];rect=plt.Rectangle((-x1,y0),x1-x0,y1-y0,fc=colors.get(c['ref'],'#ddd'),ec='#333',alpha=.8);ax.add_patch(rect);ax.text(-(x0+x1)/2,(y0+y1)/2,c['ref'],ha='center',va='center',fontsize=12)
for m in pcb['mounts']:
 x,y=m['xy'];ax.add_patch(plt.Circle((-x,y),1.5,fc='white',ec='#278050'));ax.text(-x,y+3,m['ref'],ha='center',fontsize=10)
ax.set(xlim=(-37,37),ylim=(75,0),aspect='equal',xlabel='从 USB 后侧看，左右对应实物 / mm',ylabel='前后位置 / mm');ax.grid(alpha=.15);ax.set_title('装入通道俯视：四个底盖柱均在 PCB 轮廓之外\n天线在上，USB 在下：USB 左侧 / SD 右侧',fontsize=16)
fig.savefig(ROOT/'preview/03_board_and_bosses.png',dpi=170,bbox_inches='tight');plt.close(fig)
print('rendered')
