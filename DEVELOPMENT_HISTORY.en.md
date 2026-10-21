# VoxConductor Development and Maintenance History

[简体中文](DEVELOPMENT_HISTORY.md) | [English](DEVELOPMENT_HISTORY.en.md) · [Project README](README.en.md)

This record is grounded in accessible Git commits and their substantive diffs. Dates use the author's timestamp displayed in UTC+8, not file modification times or dates embedded in imported assets. Verifiable records extend from the V1 import on 2026-08-26 to 2026-10-21, a span of 56 days. This does not imply daily commits or establish when development first began.

Source maintenance, hardware records, and documentation/licensing work are separated below. Adding documentation does not establish the first implementation or release date of a firmware feature. A committed check report is not a new physical-device test.

## Software snapshot and maintenance

| Date | Category | Substantive diff evidence | Commit |
| --- | --- | --- | --- |
| 2026-08-26 | Initial import snapshot | Imported the then-named AmbientDesk firmware, FastAPI service, and documentation. The source includes push-to-talk audio upload, cloud ASR/model/TTS, streamed audio output, SQLite conversation state, and SD text records. This is an import of an existing implementation, not a reconstructed earlier development schedule. | [c2bb0e0](https://github.com/LighthouseXy/VoxConductor/commit/c2bb0e0745e420011c078f6c5a983c9f031a6dda) |
| 2026-08-26 | Rename and compatibility | Renamed project paths and identifiers to VoxConductor. `server/conversation_store.py` added reuse of the legacy database file and migration of the old session ID, preserving access to existing context across the rename. | [c65367e](https://github.com/LighthouseXy/VoxConductor/commit/c65367ea2e0d3ced2b286247d0fab4520c277ca4) |
| 2026-08-27 | Service diagnostics and error handling | `server/app.py` added consistent stage-failure reporting and first-audio, byte, and chunk metrics. `server/tts_client.py` distinguishes normal `session.finished` from an early close rather than treating an interrupted stream as successful completion. No latency or accuracy improvement is inferred from these code changes. | [e07bbb5](https://github.com/LighthouseXy/VoxConductor/commit/e07bbb5f4df8e3bc694e24ab32b2b5db5acbf4f8) |

These entries describe the source in their respective commits. See [device-direct setup](docs/DEVICE_CLOUD_VOICE.md) for the current voice route; the V1 server remains a historical reference, not a required PC service for the current device.

## Hardware records and static checks

| Date | Category | Substantive diff evidence | Commit |
| --- | --- | --- | --- |
| 2026-10-11 | Hardware files under version control | Added KiCad schematic/PCB files, project libraries, existing fabrication records, a board BOM, V3-SLA mechanical files, and rebuild/package scripts. Older dates in imported fabrication directories are not the fabrication date of this commit; the diff establishes that these records entered version control. | [459a489](https://github.com/LighthouseXy/VoxConductor/commit/459a4894a30c6dbee7cd5b3852c938331d62f2e8) |
| 2026-10-11 | Fix for skipped CAD checks | `complete_assembly.py` selects six structural parts by FreeCAD Label rather than internal object name, requires all 18 combinations of three reference envelopes and six parts, and adds `--check-only`. That mode updates the report without saving or exporting assembly geometry, preventing an empty check set from passing. | [cf8629e](https://github.com/LighthouseXy/VoxConductor/commit/cf8629e4ca9df97db9782ac3b10f217fb149fdf1) |
| 2026-10-11 | BOM and assembly documentation | Added `hardware/BOM.md` and the per-reference BOM CSV, recording 61 board positions, 59 intended fitted positions, and two DNPs, alongside assembly/technical clarification. This remains a board BOM, not a complete procurement list for the assembled device. | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |

Hardware boundaries remain explicit: the physical J2 repair has not been written back to the current KiCad design. V3 CAD reports do not establish resin-print fit, cable bends, or thread strength. See [hardware notes](hardware/README.md).

## Documentation, presentation, and licensing

| Date | Category | Substantive diff evidence | Commit |
| --- | --- | --- | --- |
| 2026-08-26 | Historical license | Added the PolyForm Noncommercial text used at that time. This records a past state; see the later MIT change and root LICENSE for current software terms. | [c59e8e9](https://github.com/LighthouseXy/VoxConductor/commit/c59e8e9608ce9f85f75948d442c5938802dba4e9) |
| 2026-08-26 / 2026-08-27 | Documentation maintenance | Clarified verification links, described V1 as push-to-talk half-duplex interaction between the device and FastAPI, and separated functional-chain verification from unmeasured statistical performance. The service configuration example was also clarified; these changes do not establish a new feature. | [4487e42](https://github.com/LighthouseXy/VoxConductor/commit/4487e42dff0f33f0b40053f9783d3aff8c86471b), [ebc3d11](https://github.com/LighthouseXy/VoxConductor/commit/ebc3d11accf5ef8ae338b5c84c5f65d06284d8ef) |
| 2026-10-10 | Device-direct stage and demonstrations | Added device-direct setup, voice/memory design material, a showcase, and demonstration links. The corresponding changes are documentation/assets; they do not establish the first source commit of those firmware capabilities. | [ab63267](https://github.com/LighthouseXy/VoxConductor/commit/ab63267161b4ed16bc769a9328ca33c9b7d6d61e) |
| 2026-10-11 | Reading structure and management UI documentation | Reorganized README hardware/feature/architecture navigation and revised management-web layout, interface, and boundary documentation. | [abbfed8](https://github.com/LighthouseXy/VoxConductor/commit/abbfed8c563733c2f535d854972c2323eaebcdc8) |
| 2026-10-11 | Technical and demonstration notes | Added technical architecture documentation and notes on pitch-shifted demonstration audio and presentation limits. These records are not wake-rate, latency, or endurance statistics. | [8992aac](https://github.com/LighthouseXy/VoxConductor/commit/8992aacab4e86378d38f2bb8363008d237ca8480) |
| 2026-10-11 | Speech preference documentation | Updated feature/web documentation for speech rate, voice selection, and persistence, and added desktop/mobile settings screenshots. This diff adds no firmware source and is not evidence of same-day implementation or a passed listening test. | [f182f20](https://github.com/LighthouseXy/VoxConductor/commit/f182f200d775a78dfe27ec116d7aac062993cbb2) |
| 2026-10-21 | English README and navigation | Added an English README with language links in both READMEs. It explicitly notes that the device UI is Chinese-only; no UI language switch was implemented. | [ad5a675](https://github.com/LighthouseXy/VoxConductor/commit/ad5a67575b3ceac12e64bcd3b04ebc5117de6228) |
| 2026-10-21 | Software license and boundaries | Adopted standard MIT for project-authored software and technical documentation, with path/version scope and third-party notices. Hardware, fonts, media, and dependencies retain their own terms. Firmware and audio resources did not change. | [00684a0](https://github.com/LighthouseXy/VoxConductor/commit/00684a050d0d051534f9f48b29ee5039acd37da2) |

## Reading this history

- The first entry is an import snapshot; it cannot establish an earlier day-by-day development history.
- Dates and commit links identify actual changes. Work absent from a commit is not counted as a released feature on that date.
- Documentation, photographs, and individual demonstrations do not substitute for source diffs, statistical tests, or physical validation.
- This page makes no claim of a new release, user population, or community contribution. See [license scope](LICENSE_SCOPE.md) for software and third-party boundaries.
