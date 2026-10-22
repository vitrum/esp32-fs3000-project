---
name: esp32-testing
description: 'Use when adding or changing features, fixing bugs, writing or running tests in this ESP32 project. Covers pytest-embedded, Docker, ESP-IDF Unity/QEMU, GPS/BLE/Wi-Fi/SD concurrency, wind-speed and cycling-power accuracy, CSV recording and FIT export validation. New functionality must include appropriate regression tests and executed validation.'
---

# ESP32 Feature Development And Testing

## Purpose

Implement changes together with tests that demonstrate the requested behavior.
The highest-priority data is wind speed and cycling power: preserve its value,
units, validity, ordering and source from acquisition through CSV and FIT.

This is a project-local workflow. Run commands from the workspace root, not
from this skill directory. Do not create user-level copies of this skill.

Read [Testing Reference](./references/testing.md) for commands, test ownership,
measurement rules, artifact validation and platform limitations. The current
executable test entry point and dependency files take precedence over stale
counts or versions in documentation.

## Procedure

### 1. Establish The Contract

1. Read the requested feature or failure and locate the production code that
   computes or controls it. Preserve existing unrelated worktree changes.
2. Inspect its nearest test, caller and relevant repository instructions.
3. State the expected inputs, outputs, units, error behavior and acceptance
   criteria. Identify one local hypothesis and a cheap test that can disprove it.
4. For a new feature, include at least its meaningful success and failure paths.
   For a bug fix, add a regression that reproduces the original failure when
   practical. Do not broaden the work into unrelated refactoring.

### 2. Select The Test Layer

- Use native C++ tests for deterministic driver parsing, numerical boundaries,
  state transitions, fault injection and multi-module integration.
- Use the separate ESP-IDF Unity application for code that must exercise real
  FreeRTOS behavior on QEMU or an explicitly selected physical DUT.
- Use an independent format parser for persisted artifacts. A successful export
  return value or a valid filename is not proof that its contents are correct.
- Require physical testing for RF coexistence, electrical behavior, real SD-card
  reliability, sensor calibration and actual power-latch operation. Never label
  mocks or QEMU as proof of those properties.

### 3. Add Tests With The Feature

1. Extend the closest existing test and reuse its helpers and mocks. Create a
   new test file only when the behavior has no suitable existing home.
2. Compile the real production implementation into tests. Mock only hardware,
   platform services or controlled failures; do not implement the feature again
   in the test and then test that copy.
3. Add assertions for applicable boundaries: malformed input, missing data,
   resource exhaustion, cancellation, retries, reconnect, time wrap, bounded
   queues and clean completion. Scale coverage to the actual change and risk.
4. For concurrent workflows, overlap the work rather than sequentially running
   separate module tests. Use completion signals and bounded deadlines, not
   arbitrary sleeps as evidence of completion.
5. Register new native scenarios, production sources, arguments and expected
   completion markers in `test/host/test_regression.py`. Shared C++ test files
   must link all production modules referenced by any function in the file.
6. When adding Unity cases, update the expected registration inventory in
   `test/embedded/pytest_runtime.py`. Do not silently allow an empty test menu.
7. Keep C++ assertions enabled. Never turn failures into skips, weaken expected
   values or suppress diagnostics merely to obtain a passing result.

### 4. Verify Measurements And Artifacts

- Wind changes must check calibration/interpolation, units, checksum and length
  handling, range limits, freshness and the invalid-data policy at the caller.
- Power changes must check CPS/FTMS payload layout, endianness, signed values,
  zero versus missing power, connection state and packet truncation.
- Recording or export changes must exercise acquisition/decoding, the actual
  CSV writer and FIT exporter, then independently decode the retained FIT.
- Check actual values against known raw-input expectations, not only CSV/FIT
  agreement: two identically wrong outputs must not pass.
- Validate every relevant record, developer field and unit, plus CRC, timestamps,
  sample count, distance, summaries and events. Include corruption rejection.
- Preserve artifacts under `test/results/` and identify the current run. A
  historical passing file is not evidence for a new implementation.

### 5. Execute In Small Steps

1. After the first substantive edit, immediately run the cheapest relevant
   executable check. Repair that same slice and rerun it before expanding scope.
2. Run `sh test/run.sh measurements` for changes affecting wind or power.
3. Run the other touched native scenarios, then `sh test/run.sh all` for changes
   to shared mocks, runtime behavior, recording, export or cross-module contracts.
4. If production firmware changed, also compile it with the repository's
   PlatformIO environment, `pio run -e esp32s3`. Do not migrate or upgrade its
   framework just to make a test easier to run.
5. Verify JUnit failures/errors and the expected test inventory, not just the
   final shell line. Inspect the current artifact report when testing FIT.
6. Check relevant editor diagnostics and patch formatting. Distinguish missing
   host-editor dependencies or incorrect include paths from actual container
   compiler failures; do not claim either has been fixed without evidence.

Do not flash a board, format storage, expose credentials, install global host
packages or enable privileged containers without explicit authorization. Docker
test images and disposable simulated storage are the default test environment.

### 6. Document And Report

- Update `test/README.md` when commands, scenarios, coverage boundaries or saved
  artifacts change. Keep the reference linked from this skill consistent.
- Report the feature change, commands actually executed, observed pass/fail
  results, current artifact paths and any unverified hardware requirements.
- Distinguish native pytest groups from individual Unity cases and assertions.
  Do not equate a group count with full coverage or claim 100% coverage without
  measurement and a clearly stated scope.
- If blocked by tooling or hardware, state the blocker and what remains
  unverified. Never substitute an invented test result.