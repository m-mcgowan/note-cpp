# Release Checklist

End-to-end sequence for cutting a release. Most gates are automated by
[`validate-release.sh`](../../validate-release.sh) and
[`release-validate.yml`](../../.github/workflows/release-validate.yml).

## 1. Prepare

- [ ] Bump version in `library.json`, `library.properties`, `CMakeLists.txt`
- [ ] Rename `## [Unreleased]` in `CHANGELOG.md` to `## [X.Y.Z] - YYYY-MM-DD`
- [ ] Re-add an empty `## [Unreleased]` section above it
- [ ] Commit the above on `main` (or a release branch)

## 2. Validate locally (host)

Each step is scripted. Run the whole sequence with `./validate-release.sh X.Y.Z --host-only`,
or call individual steps with `--step <name>`.

- [ ] `version-check` — `library.json` / `library.properties` match the tag
- [ ] `changelog-check` — `## [X.Y.Z]` entry exists
- [ ] `codegen` — generated headers / tests are up to date
- [ ] `host-tests` — `./ci.sh` (Catch2 suite, both normal + NOTE_MINIMAL)
- [ ] `full-checks` — `./ci.sh --full` (public headers, examples, version gating, target filtering)
- [ ] `integrations` — cjson / nlohmann / buffer JSON backend tests
- [ ] `coverage` — lines ≥ 90 %, functions ≥ 90 %, branches ≥ 95 %
- [ ] `docs` — Doxygen build, snippet verification, link check
- [ ] `pio-build` — PlatformIO firmware builds (ESP32-S3 serial + I2C)
- [ ] `arduino-build` — arduino-cli compiles `quickstart`, `serial_basic`, `i2c_basic` on ESP32-S3 and Blues Swan

## 3. Validate on hardware

Run on a workstation with a Notecard attached. Each step uploads and asserts
a success marker over the serial console.

- [ ] `hw-esp32-serial` — integration tests, serial wire
- [ ] `hw-esp32-i2c` — integration tests, I2C wire
- [ ] `hw-esp32-arduino-serial` — `serial_basic.ino` end-to-end
- [ ] `hw-esp32-arduino-i2c` — `i2c_basic.ino` end-to-end
- [ ] `hw-swan-arduino-serial` — `serial_basic.ino` on Blues Swan

## 4. Validate in CI

- [ ] Trigger `release-validate.yml` workflow with the version input
- [ ] All steps in the workflow pass

## 5. Tag and publish

- [ ] `git tag vX.Y.Z` on the release commit
- [ ] `git push --tags` (only after local + CI validation)
- [ ] Create a GitHub Release; tick **Pre-release** for 0.x releases
- [ ] Release notes: paste the `## [X.Y.Z]` CHANGELOG block

## 6. Registries

- [ ] Arduino Library Manager picks up new tags automatically — confirm
      on the next index cycle
- [ ] Publish to PlatformIO Registry

## 7. Announce

- [ ] Blues community forum thread with migration guide link
- [ ] GitHub Discussions post for feedback

## Release gates that don't fit the pipeline

These are human-judgement items — not automated:

- [ ] README "BETA" banner matches the 0.x reality
- [ ] No in-progress experimental code in `src/note/**` that shouldn't ship
- [ ] Working tree is clean of dev artifacts (`logs/`, `PLAN-*.md`, `HANDOFF-*.md`, etc.)
