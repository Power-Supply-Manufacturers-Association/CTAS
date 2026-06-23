#pragma once

// CtasConverter — "generate a CIAS element (leaf) from a CTAS controller".
//
// ctas_to_cias(peas, fidelity) lowers a CTAS `controller` to a CIAS brick built from AAS comparators —
// the "controller as one part" route (the topology places a single controller component; this expands
// it). Driven by the agnostic ideal control law in `controller.behavioral` (controlScheme + sensing +
// rails/threshold/hysteresis).
//
// Supported controlScheme: `synchronousRectifier`. Two sensing modes:
//   - "current"     : two comparators read the secondary/tank-current sign across a sense element and
//                     gate the two rectifying diagonals (gA/gB). No per-switch self-feedback — the
//                     robust choice for resonant converters. 4-pin interface: senseP/senseM/gA/gB.
//   - "drainSource" : four comparators, one per SR switch, emulate each body diode from its Vds.
//                     8-pin interface: nodeC/nodeD/vSense/gSense + gE/gF/gG/gH. (Self-feedback; provided
//                     for completeness — current sensing is preferred for resonant SR.)
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
