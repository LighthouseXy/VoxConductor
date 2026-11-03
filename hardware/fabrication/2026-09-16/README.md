# 2026-09-16 制造文件

这是已制板的最终复核输出，与当前 KiCad 源文件对应。本次整理保留生产内容原样，不重复保存 ZIP、钻孔预览 SVG 或历史审图副本。

- 四层顺序 F.Cu / In1.Cu / In2.Cu / B.Cu；Gerber 为 RS-274X。
- PTH / NPTH 分开；制造要求见 [制造说明.txt](制造说明.txt)。
- `VoxConductor-F_Paste.gtp` 是顶层钢网数据，`VoxConductor-Edge_Cuts.gm1` 是板框，不再另存一份钢网目录。
- `placement.csv` 记录原版器件坐标；采购需另行核对完整 BOM，实物 J2 旋转返修待回写到坐标表。
- 重做PCB时，先解决J2实物方向差异并完成新版校验，再生成对应修订的生产文件。

原始 PCB SHA-256：`aa969c4622226e21993787093240118a8f0222cf1b3dae50aa8d425ad1c36f35`。

本轮DRC/ERC结果见 [检查说明](../../checks/README.md)；实物焊接、功能及量产需按各自项目另行验证。
