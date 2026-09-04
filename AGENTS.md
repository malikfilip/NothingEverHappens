# picoTorrent Development Guidelines

## Project

picoTorrent is a C++20 discrete-event simulator of the BitTorrent protocol.

## Development rules

- Use C++20.
- Use CMake as the build system.
- Keep the simulation engine independent from the GUI and CLI.
- Prefer small, incremental changes.
- Do not introduce external dependencies without explicit approval.
- Do not redesign existing architecture without explicit approval.
- Do not implement features that were not requested.
- Preserve existing public APIs unless a change has been discussed.
- Build the project after implementation changes.
- Add tests for non-trivial simulation behavior when appropriate.
- Keep simulation time separate from real wall-clock time.
- Do not use sleep() to model simulated delays.
- The core simulation should remain single-threaded unless explicitly changed later.

## Workflow

Before implementing a substantial feature:

1. Read `ARCHITECTURE.md`.
2. Inspect the relevant existing code.
3. State the intended changes.
4. Implement only the requested scope.
5. Build the project.
6. Report changed files, important design decisions, and build/test results.

When architecture is ambiguous, do not invent a major design decision. Ask for clarification.