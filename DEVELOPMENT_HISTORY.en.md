# VoxConductor Development and Maintenance History

[简体中文](DEVELOPMENT_HISTORY.md) | [English](DEVELOPMENT_HISTORY.en.md) · [Project README](README.en.md)

This record follows accessible Git commits and their substantive diffs. Dates use the author's timestamp in UTC+8. The verifiable interval runs from the V1 import on 2026-08-26 to 2026-10-21, spanning 56 days; the original start date and daily activity require earlier records.

The sections cover source maintenance, hardware records, and documentation/licensing work. Feature implementation dates come from source diffs; release dates and physical-device acceptance require their corresponding release and test records.

## Software snapshot and maintenance

| Date | Category | Substantive diff evidence | Commit |
| --- | --- | --- | --- |
| 2026-08-26 | Initial import snapshot | Imported the then-named AmbientDesk firmware, FastAPI service, and documentation. The source includes push-to-talk audio upload, cloud ASR/model/TTS, streamed audio output, SQLite conversation state, and SD text records. The import captures an existing implementation; earlier development dates require earlier records. | [c2bb0e0](https://github.com/LighthouseXy/VoxConductor/commit/c2bb0e0745e420011c078f6c5a983c9f031a6dda) |
| 2026-08-26 | Rename and compatibility | Renamed project paths and identifiers to VoxConductor. `server/conversation_store.py` added reuse of the legacy database file and migration of the old session ID, preserving access to existing context across the rename. | [c65367e](https://github.com/LighthouseXy/VoxConductor/commit/c65367ea2e0d3ced2b286247d0fab4520c277ca4) |
| 2026-08-27 | Service diagnostics and error handling | `server/app.py` added consistent stage-failure reporting and first-audio, byte, and chunk metrics. `server/tts_client.py` classifies normal `session.finished` and early closure separately. Latency and accuracy improvements await dedicated measurements. | [e07bbb5](https://github.com/LighthouseXy/VoxConductor/commit/e07bbb5f4df8e3bc694e24ab32b2b5db5acbf4f8) |

The entries describe the source in their respective commits. The current voice route connects the device directly to cloud services; see [device-direct setup](docs/DEVICE_CLOUD_VOICE.md). V1 server records document the earlier architecture.

## Hardware records and static checks

| Date | Category | Substantive diff evidence | Commit |
| --- | --- | --- | --- |
| 2026-10-11 | Hardware files under version control | Added KiCad schematic/PCB files, project libraries, existing fabrication records, a board BOM, V3-SLA mechanical files, and rebuild/package scripts. The diff dates the entry of these records into version control; fabrication dates follow the original production records. | [459a489](https://github.com/LighthouseXy/VoxConductor/commit/459a4894a30c6dbee7cd5b3852c938331d62f2e8) |
| 2026-10-11 | Fix for skipped CAD checks | `complete_assembly.py` replaces internal-object-name selection with FreeCAD Label selection for six structural parts, requires all 18 combinations of three reference envelopes and six parts, and adds `--check-only`. That mode updates the report and preserves assembly geometry; a complete check set is required for acceptance. | [cf8629e](https://github.com/LighthouseXy/VoxConductor/commit/cf8629e4ca9df97db9782ac3b10f217fb149fdf1) |
| 2026-10-11 | BOM and assembly documentation | Added `hardware/BOM.md` and the per-reference BOM CSV, recording 61 board positions, 59 intended fitted positions, and two DNPs, alongside assembly/technical clarification. The assembled-device procurement list requires additional items beyond this board BOM. | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |

KiCad retains the pre-repair J2 design, pending the physical correction. Resin-print fit, cable bends, and thread strength await physical validation alongside the V3 CAD checks. See [hardware notes](hardware/README.md).

## Documentation, presentation, and licensing

| Date | Category | Substantive diff evidence | Commit |
| --- | --- | --- | --- |
| 2026-08-26 | Historical license | Added the PolyForm Noncommercial text used at that time. This records a past state; see the later MIT change and root LICENSE for current software terms. | [c59e8e9](https://github.com/LighthouseXy/VoxConductor/commit/c59e8e9608ce9f85f75948d442c5938802dba4e9) |
| 2026-08-26 / 2026-08-27 | Documentation maintenance | Clarified verification links, described V1 as push-to-talk half-duplex interaction between the device and FastAPI, and separated functional-chain verification from unmeasured statistical performance. The service configuration example was also clarified; the diff records documentation and configuration maintenance. | [4487e42](https://github.com/LighthouseXy/VoxConductor/commit/4487e42dff0f33f0b40053f9783d3aff8c86471b), [ebc3d11](https://github.com/LighthouseXy/VoxConductor/commit/ebc3d11accf5ef8ae338b5c84c5f65d06284d8ef) |
| 2026-10-10 | Device-direct stage and demonstrations | Added device-direct setup, voice/memory design material, a showcase, and demonstration links. The corresponding diff dates these documents and assets; first implementation dates require their firmware source commits. | [ab63267](https://github.com/LighthouseXy/VoxConductor/commit/ab63267161b4ed16bc769a9328ca33c9b7d6d61e) |
| 2026-10-11 | Reading structure and management UI documentation | Reorganized README hardware/feature/architecture navigation and revised management-web layout, interface, and boundary documentation. | [abbfed8](https://github.com/LighthouseXy/VoxConductor/commit/abbfed8c563733c2f535d854972c2323eaebcdc8) |
| 2026-10-11 | Technical and demonstration notes | Added technical architecture documentation and notes on pitch-shifted demonstration audio and presentation limits. Wake-rate, latency, and endurance statistics await dedicated measurements. | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |
| 2026-10-11 | Speech preference documentation | Updated feature/web documentation for speech rate, voice selection, and persistence, and added desktop/mobile settings screenshots. The diff covers documentation and screenshots; implementation dates and listening-test results require their corresponding source and test records. | [f182f20](https://github.com/LighthouseXy/VoxConductor/commit/f182f200d775a78dfe27ec116d7aac062993cbb2) |
| 2026-10-21 | English README and navigation | Added an English README with language links in both READMEs. The device UI remains Chinese; language links navigate the repository documentation. | [ad5a675](https://github.com/LighthouseXy/VoxConductor/commit/ad5a67575b3ceac12e64bcd3b04ebc5117de6228) |
| 2026-10-21 | Software license and boundaries | Adopted standard MIT for project-authored software and technical documentation, with path/version scope and third-party notices. Hardware, fonts, media, and dependencies retain their own terms. Firmware and audio resources retain their preceding versions. | [00684a0](https://github.com/LighthouseXy/VoxConductor/commit/00684a050d0d051534f9f48b29ee5039acd37da2) |

## Reading this history

- The first entry records an existing implementation at import time; earlier development dates require earlier records.
- Dates and commit links identify the changes included in each commit; additional work is dated by its own source and release records.
- Source diffs, statistical tests, and physical validation each supply their own evidence alongside documentation, photographs, and demonstrations.
- Release history, user metrics, and community contributions require their own records. See [license scope](LICENSE_SCOPE.md) for software and third-party terms.
