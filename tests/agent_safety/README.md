# Offline Agent safety checks

Run `python3 tests/agent_safety/run_checks.py` from the repository root.
Use `--only reliability|runtime|transport|semantic|storage` for a targeted group.
Requires the existing C compiler and installed cJSON component. All devices,
models, storage faults and SDK operations use synthetic fixtures. No network,
paid model, hardware or firmware flash is used. Temporary binaries are removed.
