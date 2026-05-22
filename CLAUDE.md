# ra8d2-firmware (Cortex-M85 bare-metal HAL)

See @.claude/CLAUDE.md for full project conventions, environment setup, and coding rules. <!-- AI-OK: reference to CLAUDE.md file -->

## Subagents & Swarms
- **Code Style compliance**: `@style-reviewer` (checks C23, Doxygen, and header guards)
- **Safety & MC/DC compliance**: `@safety-reviewer` (checks MC/DC tests and NASA P10)
- **HUM Citations validation**: `@citation-reviewer` (checks register accesses and HUM citations)
