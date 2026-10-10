# 2026-09-16 制造文件

这是已制板的最终复核输出，与当前 KiCad 源文件对应。本次整理保留生产内容原样，不重复保存 ZIP、钻孔预览 SVG 或历史审图副本。

- 四层顺序 F.Cu / In1.Cu / In2.Cu / B.Cu；Gerber 为 RS-274X。
- PTH / NPTH 分开；制造要求见 [制造说明.txt](制造说明.txt)。
- `VoxConductor-F_Paste.gtp` 是顶层钢网数据，`VoxConductor-Edge_Cuts.gm1` 是板框，不再另存一份钢网目录。
- `placement.csv` 是原版器件坐标，不是完整采购 BOM；实物 J2 旋转返修未反映在这份坐标表中。
- 如重做 PCB，应先解决 J2 实物方向差异并完成新版本校验，不能把本目录称为新修订生产文件。

原始 PCB SHA-256：`aa969c4622226e21993787093240118a8f0222cf1b3dae50aa8d425ad1c36f35`。

本轮重新执行的 DRC/ERC 见 [检查说明](../../checks/README.md)。这属于静态设计检查，不能代替实物焊接、功能或量产验证。
