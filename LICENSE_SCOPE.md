# License scope / 许可范围

项目原创软件代码、配置和原创技术文档采用根 [MIT License](LICENSE)。具体文件与校验值列于 `license-scope.json` 的 `mit_files`。图片、影音、硬件、字体、数据及第三方组件不随项目软件统一重授权。

根 MIT 覆盖本仓库实际分发的上述原创软件与文字。清单还可记录作者其他版本的源码校验值：`repository_blob_sha` 指向所标明仓库参考提交中的文件，`recorded_source_available_in_repository=false` 表示该校验值对应的源码版本未在该参考提交中分发。此类条目仅为版本记录，不是必需文件链接或构建前提；哈希清单不向 MIT 正文附加使用限制。

Project-authored software, configuration, and original technical documentation are licensed under the root [MIT License](LICENSE). The file inventory and checksums are listed under `mit_files` in `license-scope.json`. Media, hardware, fonts, data, and third-party components are subject to the separate terms described below.

| 路径范围 | 许可边界 |
| --- | --- |
| firmware/main/ 原创 C/H、Kconfig、HTML、链接脚本及配置示例，除 app_ui_font_*.c 与 reminder.pcm | MIT；具体文件见清单 |
| firmware/ 项目 CMake、分区 CSV、sdkconfig、dependencies.lock 与项目配置 | MIT 覆盖项目配置内容；依赖许可不变 |
| server/*.py、server/requirements.txt、server/.env.example | 项目文件为 MIT；云服务与第三方 Python 包条款独立 |
| tools/ 原创脚本、tests/ 原创测试及替身、examples/ 原创示例（含 examples/lan_switch/main/Kconfig.projbuild） | MIT；所生成或处理的第三方资产按资产自身条款 |
| .gitignore、README.md、README.en.md、docs/ 原创技术 Markdown（不含媒体目录） | 原创配置与文字为 MIT；链接或嵌入的媒体、数据与引述不随正文重授权 |
| firmware/main/app_ui_font_*.c，所有字体二进制 | 不纳入项目 MIT；SourceHanSansSC/Montserrat 派生文件保留 OFL、原版权与保留名称条件 |
| firmware/managed_components/**、ESP-IDF、外部依赖及模型二进制 | 保留各自许可和版权，不由项目根 MIT 重新授权 |
| hardware/**，含 KiCad、制造文件、BOM、机械脚本、CAD、导出与厂商附件；docs/BOM.csv | 不纳入项目 MIT；原创硬件维持原有适用条件，厂商部分按其本身条款 |
| assets/**、docs/images/**、docs/videos/**、任何图片/音频/视频/字体资源 | 保留各自许可与作者归属，不因仓库位置取得项目 MIT 授权 |
| firmware/main/reminder.pcm | 不纳入项目 MIT；音源来源与再分发权限单独确认 |
| 数据库、SD/对话数据、私有凭据、运行日志、备份 | 不是本软件许可授予使用权的内容，不属于软件 MIT 范围 |

MIT 软件不附加非商业条件。`licenses/PolyForm-Noncommercial-1.0.0.txt` 保存原项目许可证，适用于未改变许可的项目原创硬件/资产及旧版本既有范围；它不是 MIT 软件的附加限制，也不替第三方材料授予新权利。

项目署名为 2026 云月（GitHub: LighthouseXy）。第三方版权和许可声明须继续保留，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。

Source inventory records are distinct from distributed files. Entries marked `recorded_source_available_in_repository=false` identify other authored source versions and do not promise that the exact source is present here. Repository blob IDs refer to the commit specified in `license-scope.json`; the inventory is not a build manifest and does not add restrictions to MIT.
