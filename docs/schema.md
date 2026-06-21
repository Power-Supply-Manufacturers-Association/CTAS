# CTAS — Controller Agnostic Structure

Vendor-neutral JSON Schema (draft 2020-12) data model for power-electronics **control ICs**, part of the OpenConverters / PEAS family. Every valid CTAS document is also a valid PEAS document (`inputs` + `controller` + `outputs`).

`$id` namespace: `https://psma.com/ctas/...`. Cross-repo `$ref`s resolve by absolute `$id` URI against the sibling repos checked out alongside this one (PEAS in particular). CTAS reuses the PEAS shared primitives (`dimensionWithTolerance`, `manufacturerInfo`, `distributorInfo`, `substituteInfo`, `datasheetInfoPartBase`, `datasheetInfoMechanical`, `datasheetInfoThermal`, `complianceTarget`, `designRequirementsBase`, `topology`, `connectionType`, `outputBase`) and **owns** the controller-family vocabularies — it is **not** self-contained the way MAS is.

## Design principle: one schema, category-gated capability sub-objects

Control ICs span PWM/LLC/PFC switchers, gate drivers, digital/PMBus controllers, references, sense amps, hot-swap/eFuse/load-switch front-ends and supervisors/sequencers. Rather than a `oneOf` explosion (one giant union per category) or a flat field bag (where a TL431 would carry `slopeCompensation`), CTAS keeps:

- a **shared core** — `function` (the category discriminator + topology/modulation/channels) and the **common `electrical` scalars** (supply, frequency, duty, reference, soft-start) that almost every part publishes; plus
- **optional CLOSED capability sub-objects** under `electrical`, each populated **only** for the categories that use it.

This satisfies *fewest fields / no redundancy / cover all cases* simultaneously: a part never sees a field it cannot use, and the schema never duplicates a quantity.

## At a glance

```
CTAS  (https://psma.com/ctas/CTAS.json)   { inputs, controller, outputs }
│
├─ inputs
│   ├─ designRequirements         → designRequirementsBase + {category*, topology†, switchingFrequency†,
│   │                                modulation, conductionMode, channelCount, phaseCount, syncRectification,
│   │                                isolation, isolationRatingRmsMin, cmtiMin, supplyVoltageAvailable,
│   │                                minimumGateDriveCurrent, maximumQuiescentCurrent, maximumStandbyPower,
│   │                                requiredProtections[], digitalInterfaceRequired, requiredTelemetry[],
│   │                                aecQGrade, controlConfigSeed}      († required for switching categories)
│   └─ operatingPoints[]          → {ambientTemperature, supplyVoltage, switchingFrequency, loadFraction}
│
├─ controller
│   ├─ manufacturerInfo            (PEAS shared identity fields)
│   │   └─ datasheetInfo           ◄── the parametric catalog block
│   │       ├─ part        → datasheetInfoPartBase + {deviceType, technology}
│   │       ├─ function*   { category*, intendedTopologies[], modulation, conductionMode, channelCount,
│   │       │                maxPhaseCount, phaseShedding, currentSharing, syncRectification, isolation,
│   │       │                frequencyJitterSupported }
│   │       ├─ electrical  { supplyVoltage, supplyVoltageAbsoluteMax, quiescentCurrent, startupCurrent,
│   │       │                switchingFrequencyMin/Max, maxDutyCycle, maxDutyCycleClampType, minOnTime,
│   │       │                minOffTime, referenceVoltage, referenceTolerance, softStartTime,
│   │       │                leadingEdgeBlanking, slopeCompensation, deadTime, lightLoadMode,
│   │       │                highVoltageStartup, capacitiveModeProtection, dynamicResponseEnhancer,
│   │       │                ── capability sub-objects ──
│   │       │                currentMode, errorAmplifier, gateDrive, uvlo[], isolation, senseAmplifier,
│   │       │                shuntReference, voltageReference, hotSwap, syncRectifier, burstMode,
│   │       │                brownOut, pfc, loadLine, synchronization, integratedPowerStage }
│   │       ├─ protections[] { kind*, threshold, hysteresis, mode, restartDelay, responseTime, rail }
│   │       ├─ pins[]       { number*, name*, function* }      (slim map for stencil wiring)
│   │       ├─ digitalInterface { kind, maxClock, revision, address{}, configurableViaNvm,
│   │       │                     nonvolatileConfig{}, telemetry[], vidDac{}, digitalControl{} }
│   │       ├─ thermal     → datasheetInfoThermal + {maximumJunctionTemperature, thetaJA, thetaJC}
│   │       ├─ mechanical  → datasheetInfoMechanical + {packageType, assemblyType}
│   │       └─ compliance  { aecQGrade, moistureSensitivityLevel, rohsCompliant, reachCompliant,
│   │                        complianceTargets[] }
│   ├─ distributorsInfo[]          (PEAS shared)
│   └─ substitutesInfo[]           (PEAS shared)
│
└─ outputs[]   { biasLoss{}, thermal{}, selection{} }   (each carries the PEAS outputBase provenance shell)
```

`*` = required. An empty `controller: {}` is a valid pre-librarian seed (carry the spec in `inputs.designRequirements`); a resolved part carries `manufacturerInfo` (which requires `name` + `datasheetInfo`, and `datasheetInfo` requires `function`, and `function` requires `category`).

## Which capability sub-object belongs to which category

| Sub-object (`electrical.*`) | Populated for | Carries (headline) |
| --- | --- | --- |
| `currentMode` | peak/avg current-mode PWM, PFC | `maxThresholdVoltage` (CS comparator clamp — UC384x 1.0 V) |
| `errorAmplifier` | all switchers | `type`, `transconductance`/`openLoopGain`, `gainBandwidth`, `secondInstance` (PFC 2nd loop) |
| `gateDrive` | drivers + switchers with on-die output | source/sink peak, `driveVoltage`, `negativeBiasMin`, prop-delay + matching, `deadTimes[]`, Miller clamp |
| `uvlo[]` | nearly universal | per-rail `startThreshold`/`stopThreshold`/`side` (array: input + output + floating) |
| `isolation` | isolated drivers / amps / digital isolators / opto | `isolationType`, `withstandVoltageRms`, `workingVoltage`, `surgeVoltage`, `cmti` |
| `senseAmplifier` | current-sense / isolated amps / iso-modulators | `gain`/`gainOptions`, `cmrr`, `commonModeRange`, `outputFormat` (analog vs bitstream) |
| `shuntReference` | TL431-class shunt regulators | cathode-current window, `dynamicImpedance`, `adjustableRange` |
| `voltageReference` | series/shunt references | `tempco`, `outputNoise`, `longTermDrift`, line/load regulation |
| `hotSwap` | hot-swap / eFuse | `currentLimit`, `circuitBreakerThreshold`, `powerLimit` (SOA), `faultResponse` |
| `syncRectifier` | SR controllers | negative `turnOnThreshold`, `turnOffThreshold`, `proportionalGateDrive`, `ccmCycleLimit` |
| `burstMode` | LLC / flyback light-load | entry/exit thresholds, `standbyPower` |
| `brownOut` | offline PWM/LLC/PFC | bulk-node `threshold`/`hysteresis` (NOT VCC UVLO units) |
| `pfc` | PFC controllers | `multiplierInput`, `lineFeedforward`, `frequencyFoldbackFloor`, `valleySwitching` |
| `loadLine` | multiphase VR | droop `slope`, `dcrSenseSupported` |
| `synchronization` | parallelable / interleaved | `role` (master/slave), `frequencyMultiplier`, `phaseShift` (inter-chip) |
| `integratedPowerStage` | GaN/Si power-stage ICs | `semiconductorRef` → a SAS device (CTAS owns only the driver/protection half; FET physics live in SAS) |

## Modelling rules worth knowing

- **UVLO is not a protection row.** It is an operating boundary carried by `electrical.uvlo[]` (start/stop/side), never as `protections[].kind = "uvlo"` — so it is never double-modelled. `uvlo` is an array because isolated drivers and integrated-half-bridge LLC parts have more than one domain.
- **Switching frequency is a min/max pair** (`switchingFrequencyMin`/`Max`), not a scalar. Fixed-frequency parts set `min == max`; LLC sets the regulation band (max = soft-start sweep top).
- **`maxDutyCycle` carries the number; `maxDutyCycleClampType` carries the mechanism** (`toggleFlipFlop` structural 50 % vs `programmable` vs `none`). They are not redundant.
- **Multi-edge dead times** use `gateDrive.deadTimes[]` (named edges `legAB`/`legCD`/`primaryToSyncRectifier`…); single-edge parts use the scalar `electrical.deadTime`.
- **Digital-ness is implied** by `digitalInterface.digitalControl`, not by a `modulation = "digital"` value — a digital controller still names its underlying law (e.g. `peakCurrentMode`).
- **PMBus `READ_*` commands map 1:1 onto `telemetry[]` flags**, so there is no free-string supported-command list.
- **GaN/Si power-stage FET physics are not duplicated** — `integratedPowerStage.semiconductorRef` points at the SAS record.
- **`compliance` replaces the former (dangling) `business` block** the inline PEAS schema referenced; it reuses the shared `complianceTarget` primitive.

## Validation

```bash
pip install jsonschema referencing
python3 scripts/validate.py
```

Three gates: schema meta-validation + **full `$ref` resolution** (catches dangling refs); `examples/*.json` vs `CTAS.json`; PEAS citizenship. In the TAS workspace, `TAS/data/controllers.ndjson` records (each a `{ "controller": { … } }` wrap) are validated against `controller.json` by `TAS/tests/test_data.py`.
