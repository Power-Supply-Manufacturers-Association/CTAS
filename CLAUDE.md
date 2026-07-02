# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository nature

CTAS is primarily a **schema repository**, plus a small C++ lowering library (`src/CtasConverter.cpp`, `ctas_to_cias`) with Catch2 tests (`tests/`, build via CMake; run `./build/ctas_tests` directly, never ctest). It defines the **Controller Agnostic Structure**: a JSON Schema 2020-12 specification for power-electronics control ICs (PWM / multiphase / LLC / PFC / phase-shift / sync-rectifier controllers, isolated & non-isolated gate drivers, digital/PMBus controllers, shunt regulators, linear regulators / LDOs, voltage references, current-sense & isolated amplifiers, hot-swap / eFuse controllers). Work here is editing JSON Schema files and Markdown docs.

The only executable is `scripts/validate.py` (the validation gate). Run it after every schema change:

```bash
pip install jsonschema referencing
python3 scripts/validate.py
```

## Layout

- `schemas/CTAS.json` — top-level wrapper: `{ inputs, controller, outputs }`. Every valid CTAS document is also a valid PEAS document (the `controller` branch).
- `schemas/controller.json` — the controller payload. `manufacturerInfo.datasheetInfo` carries the parametric catalog block (`part`, `function*`, `electrical`, `protections[]`, `pins[]`, `digitalInterface`, `thermal`, `mechanical`, `compliance`). `function.category` is the required discriminator.
- `schemas/utils.json` — **CTAS-owned** controlled vocabularies: `controllerCategory`, `controllerModulation`, `controllerConductionMode`, `controllerSyncRectification`, `controllerIsolation`, `controllerPinFunction`, `controllerProtectionKind`, `protectionMode`, `telemetryChannel`, `packageType`. These moved out of `PEAS/schemas/utils.json` during the extraction (no other family used them).
- `schemas/inputs.json` + `schemas/inputs/designRequirements.json` — the design-requirements seed (built on the PEAS `designRequirementsBase` mixin) plus optional `operatingPoints[]`.
- `schemas/outputs.json` — per-operating-point computed results (`biasLoss`, `thermal`, `selection`), each wrapping the PEAS `outputBase` provenance shell.
- `docs/schema.md` — field-by-field reference. **Keep in sync with `schemas/controller.json`.**
- `examples/*.json` — worked whole-document examples. **Keep in sync** and re-run `validate.py`.

There is **no** `data/` directory: CTAS is a building-block / type repo. Finished orderable controller parts live in **`TAS/data/controllers.ndjson`** (validated against `controller.json` by `TAS/tests/test_data.py`).

## Sibling-repo layout and PEAS relationship

CTAS is a sibling of MAS / CAS / SAS / RAS / CONAS under the PEAS umbrella, checked out alongside them at `/home/alf/PSMA/`:

```
PSMA/
  PEAS/    schemas/{peas.json, utils.json, outputs/outputBase.json, ...}   -- parent + shared primitives
  CTAS/    schemas/{CTAS.json, controller.json, utils.json, inputs.json, inputs/, outputs.json}
  CONAS/   schemas/{CONAS.json, connector.json, utils.json, ...}           -- the structural template CTAS follows
  MAS / CAS / SAS / RAS                                                    -- the other families
  TAS/     data/controllers.ndjson                                         -- the orderable-parts catalog
```

- **Reuse PEAS shared primitives by absolute `$id` URI**, never by relative path: `https://psma.com/peas/utils.json#/$defs/{dimensionWithTolerance, manufacturerInfo, distributorInfo, substituteInfo, datasheetInfoPartBase, datasheetInfoMechanical, datasheetInfoThermal, complianceTarget, designRequirementsBase, topology, connectionType, market}` and `https://psma.com/peas/outputs/outputBase.json`. CTAS is **not** self-contained the way MAS is.
- **Own the controller-family enums here** (the CONAS pattern). If you need a new vocabulary that only controllers use, add it to `CTAS/schemas/utils.json`, not PEAS.
- PEAS pins the `controller` branch to `https://psma.com/ctas/controller.json` and the design-requirements seed to `https://psma.com/ctas/inputs/designRequirements.json`. Don't reintroduce an inline controller schema in PEAS.

## Schema editing rules (the strictness philosophy)

- **Closed objects everywhere.** Every object is `additionalProperties: false` (or `unevaluatedProperties: false` when it extends a shared base via `allOf`). Never leave an object open.
- **Capability sub-objects, not a flat bag and not a per-category `oneOf`.** Category-specific electricals live in optional closed sub-objects under `electrical` (e.g. `gateDrive`, `shuntReference`, `hotSwap`) that appear only for the categories that use them. When you add a parameter, put it in the sub-object for its category — do not add a top-level scalar that is null for every other category.
- **No redundancy / no double-modelling.** Before adding a field, check it is not derivable from an existing one (e.g. `pinCount` is derived from `pins[]`; load-line `360/N` offset is derived from `maxPhaseCount`). UVLO lives in `electrical.uvlo[]`, never also as a `protections[]` row.
- **Enums are complete unions of fixed human-readable strings.** When a new real part needs a value the enum lacks, add the value (don't shoehorn into a near-match). Verify against the datasheet.
- **Keep `function.category` required** — it is the discriminator the whole schema and the TAS validator key off.
- **When you add or rename a field in `schemas/controller.json`, update the matching entry in `docs/schema.md` and any `examples/*.json` in the same change, then re-run `validate.py`.**

## Provenance of the field set

The field set was derived from a datasheet survey of mainstream parts across every controller family (TI UC384x / UCC256xx / UCC28xxx / UCC2152x / UCC24xxx; Analog Devices ADP105x / LTC / ADuM4135 / AMC130x / INA240; onsemi NCP12xx / NCP13xx / NCP43xx / NCP16xx; Infineon ICE5 / XDPP1100 / EiceDRIVER; ST L6562 / L6599; Renesas/Intersil ISL68137; Power Integrations; TI TL431 / LM4040 / REF5025 / LM5066 / TPS25940). The provenance and the per-category field map are documented in `docs/schema.md`.
