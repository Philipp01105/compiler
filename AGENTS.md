# Validation

For Linux validation, run only the checks necessary for the current change.
Leave the full Linux test suite to CI unless the user explicitly requests it.

# DMM formatting

Follow the agreed rules in [tools/README.md](tools/README.md). Use
`python tools/format_dmm.py` for tracked DMM sources and `--check` to validate.
Preserve diagnostic fixture errors, comments and literal contents.
