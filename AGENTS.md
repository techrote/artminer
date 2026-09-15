# AGENTS.md — ArtMiner repository instructions

These instructions apply to the entire repository.

## Authority

For implementation work, read and reconcile:

1. the active GitHub issue;
2. `RAG.md`;
3. this file;
4. current repository documentation;
5. current `main` and accepted predecessor implementations/tests.

The active issue defines immediate scope. `RAG.md` defines product and architecture contracts. Existing tested behaviour on `main` is authoritative unless the issue intentionally changes it.

## Product constraints

ArtMiner is a standalone Windows-native deterministic procedural-art laboratory. Preserve these constraints unless an explicit accepted repository decision changes them:

- C++20 native implementation;
- Windows 10/11 x64 first-class target;
- Win32/D3D11/DXGI/Direct2D/DirectWrite/WIC are acceptable Windows platform facilities;
- no Electron/browser shell, Python, Node, JVM, .NET runtime requirement, cloud service, account system, telemetry, AI model, or network runtime dependency;
- no third-party runtime installation requirement;
- portable workspace/state by default;
- deterministic model-owned randomness only;
- canonical/versioned recipe and provenance semantics;
- UI state must not alter recipe semantics without an explicit model command;
- ordinary graph cycles are invalid; state/feedback crosses an explicit deterministic boundary.

## Engineering rules

- Prefer small, explicit contracts over framework-heavy abstractions.
- Keep deterministic core code independent of the GUI.
- Add no third-party dependency without documenting why Windows SDK/project-owned code is insufficient and how licensing, deterministic behaviour, maintenance, binary size, and portability are affected.
- Do not use global `rand()` or time-based seeds in deterministic paths.
- Stable ordering must not depend on worker scheduling, pointer addresses, hash-map iteration order, locale, or UI timing.
- Treat caches as disposable. Authoritative state belongs in recipes/job manifests/settings explicitly owned by the project.
- Malformed/unsupported recipes must fail clearly rather than being partially reinterpreted.
- Intentional semantic changes require versioning/migration consideration and regression tests.
- Avoid broad unrelated refactors in milestone PRs.

## Tests and CI

Every issue must add or update tests that prove its acceptance criteria. Preserve deterministic/golden fixtures unless the issue intentionally changes semantics and documents the change.

Do not obtain green CI by:

- deleting relevant tests;
- weakening assertions/tolerances without technical justification;
- suppressing compiler errors/warnings globally;
- skipping deterministic checks;
- disabling jobs or required checks.

## Branch / PR / merge protocol

For every implementation issue:

1. start from current `main` after inspecting predecessors;
2. implement the complete issue, including tests and documentation reconciliation;
3. run relevant local checks;
4. open a focused PR linked to the issue;
5. repair automated-check failures rather than bypassing them;
6. merge only when all required checks have passed and acceptance criteria are actually met;
7. verify the merge commit/change is present on `main` after merging;
8. close the issue only after that verification.

If external infrastructure prevents a required check from completing, leave the PR/issue open with evidence rather than claiming completion.

## Documentation discipline

`RAG.md` is the architecture/product retrieval document. Update it when an implementation intentionally changes an architectural contract, milestone sequence, or durable product behaviour. Do not duplicate divergent versions of core contracts in ad-hoc documents.

`README.md` is user/developer orientation and may remain shorter.
