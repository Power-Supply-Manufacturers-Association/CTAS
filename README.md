# CTAS — Controller Agnostic Structure

Vendor-neutral JSON Schema (draft 2020-12) data model for power-electronics **control ICs**, part of the OpenConverters / PEAS family. A CTAS document is `{ inputs, controller, outputs }` and is also a valid PEAS document.

CTAS covers the whole control-IC space with **one** agnostic schema, discriminated internally on `controller.manufacturerInfo.datasheetInfo.function.category`:

| Category | Examples |
| --- | --- |
| `pwmController` | UC384x, TPS40210, NCP1250, ICE5 |
| `multiphaseController` | TPS53679, ISL68137 |
| `llcController` | UCC256301, NCP1399, L6599A |
| `pfcController` | UCC28180, NCP1612, L6562A |
| `dualPwmController` / `phaseShiftController` | UCC28950, LM5045 |
| `syncRectifierController` | UCC24612, NCP4305 |
| `gateDriver` | UCC27517, UCC21520, UCC5320, ADuM4135 |
| `digitalController` | ADP1055, UCD3138, XDPP1100 |
| `secondaryFeedbackController` / `optocouplerFeedback` | UCC24650, opto loops |
| `shuntRegulator` | TL431, TLV431 |
| `voltageReference` | LM4040, REF5025 |
| `linearRegulator` | LT3045, TPS7A47, TLV757P, NCP1117 |
| `currentSenseAmplifier` / `isolatedAmplifier` | INA240, AMC1301, AMC1306 |
| `hotSwapController` / `eFuse` / `loadSwitch` | LM5066, TPS25940, TPS22918 |
| `supervisor` | TPS3700, UCD9090, ADM1266 (sequencer / voltage-monitor / watchdog) |

The trick that keeps the field count low without losing coverage: the parametric core (`function` + the common `electrical` scalars) is shared across every category, and the category-specific data lives in **optional, closed capability sub-objects** under `electrical` (`currentMode`, `gateDrive`, `uvlo[]`, `isolation`, `senseAmplifier`, `shuntReference`, `voltageReference`, `linearRegulator`, `hotSwap`, `syncRectifier`, `burstMode`, `brownOut`, `pfc`, `loadLine`, `synchronization`, `integratedPowerStage`). A TL431 carries `shuntReference` only; a UCC21520 carries `gateDrive` + `isolation` + `uvlo[]`; neither ever sees a field it cannot use.

See [`docs/schema.md`](docs/schema.md) for the at-a-glance structure diagram and the full field-by-field reference.

## Repository layout

```
schemas/
  CTAS.json                top-level container: { inputs, controller, outputs }
  controller.json          the controller component (manufacturerInfo.datasheetInfo + capability sub-objects)
  utils.json               CTAS-owned enums: controllerCategory / Modulation / ConductionMode /
                           SyncRectification / Isolation / PinFunction / ProtectionKind,
                           protectionMode, telemetryChannel, packageType
  inputs.json              operatingPoints[] + designRequirements
  inputs/
    designRequirements.json the pre-librarian seed (built on PEAS designRequirementsBase)
  outputs.json             per-operating-point results (bias loss, junction temperature, selection margins)
examples/
  ucc256301-llc-resonant.json       hybrid-hysteretic LLC controller, both tiers
  ucc21520-isolated-gate-driver.json reinforced dual gate driver (isolation + dual UVLO)
  tlv75733p-ldo.json                 1 A fixed-output LDO (the linearRegulator capability sub-object)
docs/
  schema.md                structure diagram + field reference
scripts/
  validate.py              meta-validates schemas, resolves EVERY cross-ref, validates examples + PEAS citizenship
```

## Relationship to PEAS and the other Agnostic Structures

CTAS reuses the PEAS shared primitives (`dimensionWithTolerance`, `manufacturerInfo`, `distributorInfo`, `substituteInfo`, `datasheetInfoPartBase`, `datasheetInfoMechanical`, `datasheetInfoThermal`, `complianceTarget`, `designRequirementsBase`, `topology`, `connectionType`, `market`, `outputBase`) by absolute `$id` URI — it is **not** self-contained the way MAS is. It **owns** the controller-family vocabularies in its own `utils.json`, because no other component family uses them (the CONAS pattern). Cross-repo `$ref`s resolve by absolute `$id` against the sibling repos checked out alongside this one (PEAS in particular).

The `controller` branch used to live inline in PEAS (`PEAS/schemas/controller.json`); it was extracted into this repo. PEAS now references `https://psma.com/ctas/controller.json` for the `controller` discriminator and `https://psma.com/ctas/inputs/designRequirements.json` for its design-requirements seed. The controller-family enums and `packageType`/`protectionMode`/`telemetryChannel` moved from `PEAS/schemas/utils.json` into `CTAS/schemas/utils.json` (no other family referenced them).

## Validate

```bash
pip install jsonschema referencing
python3 scripts/validate.py
```

Runs three gates: every schema meta-validates and **every** `$ref` resolves (this is stricter than a plain instance check — it catches dangling refs); each `examples/*.json` validates against `CTAS.json`; and each example is confirmed to be a valid **PEAS** document (the `controller` branch).

## License

Apache-2.0 (`$comment: SPDX-License-Identifier: Apache-2.0` on every schema), matching the OpenConverters family.
