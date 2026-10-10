# VoxConductor 硬件设计

更新：2026-10-11。这里统一保存当前 KiCad 工程、制造文件与 V3-SLA 结构，旧方案和临时文件不放进活动目录。

## 文件入口

| 资料 | 入口 | 用途 |
| --- | --- | --- |
| KiCad 工程 | [VoxConductor.kicad_pro](VoxConductor.kicad_pro) | 用 KiCad 10 打开；原理图、PCB、项目库均在本目录 |
| 原理图 | [可浏览 PDF](exports/VoxConductor-schematic.pdf)、[源文件](VoxConductor.kicad_sch) | 完整单页主原理图 |
| PCB | [源文件](VoxConductor.kicad_pcb)、[顶层图](exports/pcb-top.svg) | 52 × 50 mm、四层板 |
| 板载物料 | [board-bom.csv](exports/board-bom.csv) | 从当前原理图导出，含 DNP；不是完整采购料号表 |
| 制板资料 | [2026-09-16 制造文件](fabrication/2026-09-16/README.md) | 已制板源版本的 Gerber、钻孔与坐标表 |
| 3D 外壳 | [V3-SLA 装配与打印说明](mechanical/V3-SLA/README.md) | STEP、STL、FreeCAD、PPT、预览和重建脚本 |
| 板级接线 | [硬件与 GPIO](../docs/HARDWARE_V1.md) | 固件接口与通电检查 |
| 供应商依据 | [显示屏](docs/display/README.md)、[扬声器](docs/audio/README.md) | 原始尺寸资料及接口限制 |
| 本轮校验 | [checks/README.md](checks/README.md)、[文件哈希](manifest.json) | 设计静态检查与交付完整性 |

## 版本与装配边界

KiCad 三个主文件与 2026-09-16 最终制板记录逐字节相同。本次整理未修改网络、铜线或器件位置，也没有生成新的投板修订。

实物屏幕接口 J2 曾旋转 180° 返修；这个改动尚未写回 KiCad，不能把当前生产图当作已修正的屏幕装配方向。FH12 立式插座要求排线金属接触面背向棕色锁扣；具体针序与原理图同时核对，不能只翻排线上电。下一次 PCB 修订需把实物接口修正纳入设计并重新审图。

V3-SLA 修复外壳 USB 左右镜像与 PCB 装入时的螺丝柱干涉，需要成套替换主壳、底盖和扬声器托架。CAD 检查已通过，树脂打印后的实际试装、线束弯折与螺纹强度仍待验证。先打印入口框和底盖，不将装配 STEP 当作单件打印模型。

`lib/` 只保留当前主工程使用的三个项目封装及符号库；标准库和标准 3D 模型由 KiCad 10 提供。J2 等自定义器件未绑定完整厂家三维模型；结构校验使用 PCB 平面数据与保守高度包络，不等于完整厂家装配模型。

GitHub 保存展开文件，不同时保存 ZIP 副本。生产方需要 ZIP 时，仅压缩制造目录的生产文件；结构打印包可用 `mechanical/V3-SLA/source/package.py --output <工程目录外的路径>` 生成。
