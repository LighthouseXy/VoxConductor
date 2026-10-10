# V3-SLA — USB镜像 / PCB装入阻挡修正版

日期：2026-10-11。基于用户确认的 V2-SLA 树脂件。旧整包已移出活动工程；重建所需的六个输入保存在 source/frozen_v2。

一、原因与已修改内容
1. 旧机械坐标 X=KiCad X-195、Y=KiCad Y+92，使PCB元件面被左右翻转。
   新坐标 X=195-KiCad X、Y=KiCad Y+92，元件面为+Z。
   从USB侧往天线方向看：USB在左，SD在右；这与实物一致。
   USB中心新X=+11.7175，SD中心新X=-19.3，麦克风孔X=-14.9425。
2. 旧底盖柱 X=±19.8、Y=10.9/59.5 侵入PCB直线装入通道。
   新柱 X=±25.5、Y=12/61.5，柱外径6mm，底部先导孔1.6mm。
   下部外轮廓宽65mm，Y=4.4..68；底盖宽64.4mm，Y=4.7..67.7。
   壳底局部外扩，顶部光学/按钮安装位置保持。不是简单缩短柱子。
3. USB/SD开口在壳底开通，允许焊好连接器通过；USB后侧保留14×8mm插头外模包络。
4. 麦克风导音孔移到正确侧；线束避让及喇叭出声侧随壳体方向纠正。
5. 托架04镜像单侧避让，并局部避开USB的焊后预留空间。
6. ESP32天线前端的装入避让已补齐；前壁相应局部开口，不要自行封堵。

二、打印清单（不要把整个资料包当作打印数量）
第一批建议：trial/07_entry_frame_first_print.stl ×1，print/02_base.stl ×1；
            trial/08_pilot_coupon_1p5_1p6_1p7.stl ×1。
用实际焊好PCB固定到底盖，入口框从上方罩下；应自然到位，不弯板、不用螺丝拉齐。
入口框是Z=2..12的完整低高度壳圈，包含四根真实柱位与接口入口，不安装到最终产品。
孔径试片从上面看依次为1.5/1.6/1.7mm，盲孔深4mm；正中1.6mm为模型先导孔。
试装通过后最终替换：01_main_shell、02_base、04_speaker_cradle，各1件。
03_LCD_retainer、05_speaker_retainer、06_prism_carrier三件与旧V2几何一致，可复用。
07/08为试件，不与01同时装配。assembly STEP和FCStd仅看装配，不能整件打印。
STL只有整体Z平移到底面，没有缩放；需打印方合理安排支撑，不能盲目按落地姿态直接无支撑打印。

三、装配顺序
1. 断电取下外部线缆，保留已验证工作的电路板。
2. PCB的H1/H2/H3固定到新底盖02，确认麦克风孔对齐，不压连接器或飞线。
3. 扬声器放入新托架04，装压框05；托架装入上壳01，以两侧螺丝固定。
4. PCB/底盖保持平放，上壳沿Z方向垂直罩下。模型验证包含PCB与已装器件包络。
5. 屏幕排线接到实物已旋转的J2；屏幕/棱镜组件沿用03/06，最后就位。
   排线不得压在壳体接缝、螺丝柱、SD或扬声器之间；实物弯折未由刚体包络验证。
6. USB插头实际插到底、SD卡实际插拔、扬声器线束不夹紧后，再轻拧四颗底盖螺丝。

四、紧固件（直径×长度，全部塑料用自攻）
PCB H1/H2/H3：2×5mm盘头，3颗。
底盖四角：2×4mm沉头，4颗；沿用V2选定长度，不能照旧V1文档换成2×6。
扬声器托架两侧：2×8mm盘头，2颗。
扬声器压框四角：2×4mm盘头，4颗。
屏幕/棱镜组件四孔：2×6mm盘头，4颗。共17颗。
底盖螺丝模型按90度沉头、头径3.58mm检查；实际商品尺寸不同需重新核对。
树脂先导孔与咬合强度未实测，先用试片，手动轻拧，不用电批强压。

五、完成的验证
checks/validation.json：最终STEP重新载入；全PCB和元件包络从下方到最终位置的连续扫掠；
  主壳/托架通道、最终塑料件配合与器件包络交叠均小于1e-5 mm³。
  PCB轮廓每侧0.5mm余量；元件/插头包络每侧0.5mm、高度0.3mm余量。
  新螺丝柱距已扩张PCB扫掠体最小约1.45mm。
checks/delivery_validation.json：8个STL封闭、每边两面；STL/STEP体积误差<0.5%；
  03/05/06与原件几何差为零；底盖螺丝包络与PCB、底盖通孔及USB/SD出口检查通过。
checks/optical_speaker_validation.json：保留光学件与LCD/30mm棱镜/扬声器参考包络检查通过。

六、实物验证边界
此版修复已证实的方向错误和装入干涉；不是树脂公差或强度的实物认证。
元器件XY包络来自当前PCB；部分高度为保守估计，并非厂家完整三维模型。
焊锡凸起、维修飞线、实际线束、FPC弯曲长度、胶厚、玻璃夹紧、收缩和螺纹咬合需实配。
03/06光学夹持未在本次重新设计，不代表旧玻璃过盈值已获验证。
新01和02必须成套，不能与V2主壳/底盖混用。先打印低高度入口框再做整壳。

七、源文件与重建
source/pcb_actual.json保存本次实际PCB数据和SHA256；source/frozen_v2保存不变输入。
source/extract_pcb.py用KiCad内置Python运行；通过相对路径只读 hardware/VoxConductor.kicad_pcb，输出修正坐标快照。
source/build.py、verify_delivery.py、complete_assembly.py依次用FreeCAD内置Python运行。
source/render.py生成俯视布局，source/render_3d.py生成带逐像素深度的CAD预览；source/make_presentation.py用python-pptx生成装配PPT。
默认FreeCAD路径：/Applications/FreeCAD.app/Contents/Resources/bin/python。
presentation/V3-SLA_装配与修正说明.pptx为当前版本装配说明。

## 浏览与重建

![壳体预览](preview/01_shell.png)

[内部装配预览](preview/02_internal_exploded.png) · [PCB 与柱位](preview/03_board_and_bosses.png) · [装配 PPT](presentation/V3-SLA_装配与修正说明.pptx)

源文件运行顺序：`extract_pcb.py` → `build.py` → `verify_delivery.py` → `complete_assembly.py` → `render.py` → `render_3d.py` → `make_presentation.py` → `package.py`。提取用 KiCad Python，建模/检查用 FreeCAD Python，预览需要 NumPy、Pillow 和 Matplotlib，PPT 需要 python-pptx。脚本中的 macOS 应用库路径在其他系统上需按安装位置调整。

`source/frozen_v2/` 的六个 STEP 是重建所需的固定输入，不能当作废弃旧版本删除。它们不属于打印清单。`preview/scene.json` 是预览脚本的数据输入，STEP 用于 CAD，STL 用于打印，PPT 用于装配，各有用途。

运行 `source/package.py` 默认仅刷新文件哈希。需要交给打印方的 ZIP 时，用 `--output` 指定工程树外目录；生成包不纳入 Git。`manifest.json` 状态为 CAD_VALIDATED_PHYSICAL_FIT_PENDING。
