#pragma once

// CtasConverter — "generate a CIAS element (leaf) from a CTAS controller".
//
// ctas_to_cias(peas, fidelity) lowers a CTAS `controller` to a CIAS brick built from AAS/TBAS/PEAS
// behavioral atoms — the "controller as one part" route (the topology places a single controller
// component; this expands it). Driven by the agnostic ideal control law in `controller.behavioral`
// (a complete oneOf keyed by controlScheme, PEAS-RFC 0001 §5.3; recipes per §6).
//
// Supported controlScheme values and brick interfaces:
//   - synchronousRectifier : AAS comparators commutate the SR gates. Two sensing modes:
//       "current"     — two comparators on the tank-current sign; ports senseP/senseM/gA/gB.
//       "drainSource" — four comparators, per-switch body-diode emulation; ports
//                       nodeC/nodeD/vSense/gSense + gE/gF/gG/gH.
//   - voltageModePWM       : TBAS sawtooth ramp + AAS comparator; ports errP/errM/gate.
//                            maximumDutyCycle -> controlled min() clamp on the error.
//   - peakCurrentMode      : TBAS square clock -> reset-dominant TBAS latch; comparator of
//                            Ri*isense (+ slope-comp sawtooth) vs error resets it; ports
//                            errP/errM/isenseP/isenseM/gate (isense carries 1 V/A).
//                            maximumDutyCycle -> second comparator OR'd into reset.
//   - constantOnTime       : feedback comparator -> TBAS monostable (onTime, non-retriggerable);
//                            ports errP/errM/gate. minimumOffTime -> blanking one-shot gating
//                            the trigger.
//   - frequencyControl     : TBAS triangle VCO (centerFrequency, vcoGain) + two comparators at
//                            0.5±delta for complementary gates with dead time; ports
//                            errP/errM/gateA/gateB. minimum/maximumFrequency -> controlled
//                            min()/max() clamp on the control voltage.
//   - pfcAverageCurrentMode: AAS multiplier (line shape × error) -> current reference; controlled
//                            current-error block -> AAS integrator -> PWM comparator on a unit
//                            sawtooth at switchingFrequency; ports line/err/isenseP/isenseM/gate
//                            (line/err are ground-referenced single-ended).
//
// Drive rails: driveHigh/driveLow have no documented schema default — every scheme REQUIRES them
// and throws when absent (no fabricated rails), consistent with the synchronousRectifier lowering.
//
// Input `peas` is a PEAS/CTAS controller document — {"controller": {...}} (optionally under no wrapper).
// Output is a CIAS leaf as JSON, consumed by the CIAS converter; never persisted.

#include <nlohmann/json.hpp>
#include <string>
#include "Fidelity.hpp"

namespace CTAS {

// peas: a PEAS/CTAS controller document. Returns a CIAS leaf brick as JSON.
nlohmann::json ctas_to_cias(const nlohmann::json& peas,
                            const PEAS::Fidelity& fidelity,
                            const std::string& name = "controller");

} // namespace CTAS
