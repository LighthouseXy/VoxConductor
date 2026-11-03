# License scope / 许可范围

许可表及清单列明的项目原创软件代码、配置和原创技术文档适用根 [MIT License](LICENSE)。具体文件与校验值列于 `license-scope.json` 的 `mit_files`；图片、影音、硬件、字体、数据及第三方组件按各自适用条款使用。

根 MIT 覆盖本仓库实际分发的上述原创软件与文字。清单还可记录作者其他版本的源码校验值：`repository_blob_sha` 指向所标明仓库参考提交中的文件，`recorded_source_available_in_repository=false` 标识所指定参考提交分发范围之外的源码版本。此类条目用于版本追溯，构建输入以实际分发的源码为准；MIT 使用条件以许可正文为准。

Project-authored software, configuration, and original technical documentation are licensed under the root [MIT License](LICENSE). The file inventory and checksums are listed under `mit_files` in `license-scope.json`. Media, hardware, fonts, data, and third-party components are subject to the separate terms described below.

| 路径范围 | 许可边界 |
| --- | --- |
| firmware/main/ 原创 C/H、Kconfig、HTML、链接脚本及配置示例，除 app_ui_font_*.c 与 reminder.pcm | MIT；具体文件见清单 |
| firmware/ 项目 CMake、分区 CSV、sdkconfig、dependencies.lock 与项目配置 | MIT 覆盖项目配置内容；依赖许可不变 |
| server/*.py、server/requirements.txt、server/.env.example | 项目文件为 MIT；云服务与第三方 Python 包条款独立 |
| tools/ 原创脚本、tests/ 原创测试及替身、examples/ 原创示例（含 examples/lan_switch/main/Kconfig.projbuild） | MIT；所生成或处理的第三方资产按资产自身条款 |
| .gitignore、README.md、README.en.md、docs/ 原创技术 Markdown（不含媒体目录） | 列明的原创配置与文字适用 MIT；链接或嵌入的媒体、数据与引述按各自条款使用 |
| firmware/main/app_ui_font_*.c，所有字体二进制 | SourceHanSansSC/Montserrat 派生文件适用 OFL，保留原版权与保留名称条件 |
| firmware/managed_components/**、ESP-IDF、外部依赖及模型二进制 | 按各自许可使用并保留版权 |
| hardware/**，含 KiCad、制造文件、BOM、机械脚本、CAD、导出与厂商附件；docs/BOM.csv | 原创硬件维持原有适用条件，厂商部分按其本身条款 |
| assets/**、docs/images/**、docs/videos/**、任何图片/音频/视频/字体资源 | 按各自许可使用并保留作者归属 |
| firmware/main/reminder.pcm | 音源来源与再分发权限单独确认 |
| 数据库、SD/对话数据、私有凭据、运行日志、备份 | 作为私有运行资料保留，使用权由其权利人另行授权 |

项目软件的 MIT 许可允许按正文条件进行商业使用。`licenses/PolyForm-Noncommercial-1.0.0.txt` 保存原项目许可证，适用于维持原许可的项目原创硬件/资产及旧版本既有范围；第三方材料按各自条款使用。

项目署名为 2026 云月（GitHub: LighthouseXy）。第三方版权和许可声明须继续保留，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。

Source inventory records support version tracing. Entries marked `recorded_source_available_in_repository=false` identify other authored source versions. Repository blob IDs refer to the commit specified in `license-scope.json`; build inputs follow the distributed source, and MIT usage conditions follow the license text.

2026-11-03开发源码同步的当前文件哈希和字体例外另见 [SOURCE_SYNC_MANIFEST.json](docs/SOURCE_SYNC_MANIFEST.json)；原license-scope.json保留其历史版本记录语义。新增原创软件及文档沿用MIT，字体仍为OFL派生，提醒PCM未分发。
