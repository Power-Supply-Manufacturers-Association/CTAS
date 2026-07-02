#include "CtasConverter.hpp"
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace CTAS {
using nlohmann::json;

namespace {
const json& controller_of(const json& peas) {
    if (peas.contains("controller")) return peas.at("controller");
    throw std::runtime_error("ctas_to_cias: document has no 'controller'");
}

// Number -> expression/parameter text (same precision idiom as the CIAS emitter).
std::string num(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

// Field access on the behavioral block. No magic defaults: a missing field the lowering needs
// is an error (the schema documents no default for any of them — driveHigh/driveLow included).
std::string req_str(const json& b, const char* k) {
    if (!b.contains(k))
        throw std::runtime_error("ctas_to_cias: behavioral block is missing required '" +
                                 std::string(k) + "'");
    return b.at(k).get<std::string>();
}
double req_num(const json& b, const char* k) {
    if (!b.contains(k))
        throw std::runtime_error("ctas_to_cias: behavioral block is missing '" +
                                 std::string(k) + "' (no documented default exists)");
    return b.at(k).get<double>();
}
std::optional<double> opt_num(const json& b, const char* k) {
    if (!b.contains(k)) return std::nullopt;
    return b.at(k).get<double>();
}

// Every component atom is a complete PEAS document — the required `inputs` seed included —
// so the emitted brick is CIAS-schema-valid end to end (component.data must validate against
// peas.json; an atom without `inputs` would be a schema-illegal intermediate). The AAS/TBAS
// designRequirements seeds require their family `deviceType` discriminator; the PEAS-native
// behavioral seed carries no required fields.
json seed_inputs(const char* deviceType) {
    return json{{"designRequirements", json{{"deviceType", deviceType}}}};
}
json seed_inputs_behavioral() { return json{{"designRequirements", json::object()}}; }

// ── PEAS/AAS/TBAS component atoms (inline PEAS docs, RFC 0001 §6/§7) ──────────────────────────

// AAS comparator. threshold/hysteresis are OMITTED when not given: the AAS schema documents
// "absent = 0 (ideal)" for both, so absence is the faithful encoding, not a fabricated value.
json comparator_atom(double vHigh, double vLow,
                     std::optional<double> thr = std::nullopt,
                     std::optional<double> hyst = std::nullopt) {
    json atom;
    json& e = atom["analog"]["comparator"]["behavioral"];
    e["outputHigh"] = vHigh;
    e["outputLow"] = vLow;
    if (thr) e["threshold"] = *thr;
    if (hyst) e["hysteresis"] = *hyst;
    atom["inputs"] = seed_inputs("comparator");
    return atom;
}

// TBAS oscillator (fixed or VCO when vcoGain is given).
json oscillator_atom(const char* shape, double f, double amp, double off,
                     std::optional<double> duty = std::nullopt,
                     std::optional<double> vcoGain = std::nullopt) {
    json atom;
    json& b = atom["timeBase"]["oscillator"]["behavioral"];
    b["shape"] = shape;
    b["frequency"] = f;
    b["amplitude"] = amp;
    b["offset"] = off;
    if (duty) b["dutyCycle"] = *duty;
    if (vcoGain) b["frequencyControl"] = json{{"gain", *vcoGain}};
    atom["inputs"] = seed_inputs("oscillator");
    return atom;
}

// TBAS SR latch.
json latch_atom(double setThr, double rstThr, double hi, double lo, const char* dominance) {
    json atom;
    json& b = atom["timeBase"]["latch"]["behavioral"];
    b["setThreshold"] = setThr;
    b["resetThreshold"] = rstThr;
    b["outputHigh"] = hi;
    b["outputLow"] = lo;
    b["dominance"] = dominance;
    atom["inputs"] = seed_inputs("latch");
    return atom;
}

// TBAS monostable timer (one-shot).
json monostable_atom(double hi, double lo, double thr, const char* polarity, double onTime) {
    json atom;
    json& b = atom["timeBase"]["timer"]["behavioral"];
    b["mode"] = "monostable";
    b["outputHigh"] = hi;
    b["outputLow"] = lo;
    b["threshold"] = thr;
    b["polarity"] = polarity;
    b["onTime"] = onTime;
    b["retriggerable"] = false;
    atom["inputs"] = seed_inputs("timer");
    return atom;
}

// PEAS `controlled` nature (behavioral voltage source across own pins out/ref).
json controlled_v_atom(const std::string& expression) {
    json atom;
    json& b = atom["behavioral"];
    b["nature"] = "controlled";
    b["output"] = json{{"quantity", "voltage"},
                       {"across", json::array({"out", "ref"})},
                       {"expression", expression}};
    atom["inputs"] = seed_inputs_behavioral();
    return atom;
}

// AAS multiplier / integrator (single-ended ideal blocks).
json multiplier_atom(double gain) {
    json atom;
    atom["analog"]["multiplier"]["behavioral"] = json{{"gain", gain}};
    atom["inputs"] = seed_inputs("multiplier");
    return atom;
}
json integrator_atom(double gain, double outLow, double outHigh) {
    json atom;
    atom["analog"]["integrator"]["behavioral"] =
        json{{"gain", gain}, {"outputLow", outLow}, {"outputHigh", outHigh}};
    atom["inputs"] = seed_inputs("integrator");
    return atom;
}

json C(const char* nm, const char* pin) { return json{{"component", nm}, {"pin", pin}}; }
json P(const char* nm) { return json{{"port", nm}}; }
json cn(const char* nm, std::vector<json> eps) { return json{{"name", nm}, {"endpoints", eps}}; }
json port(const char* nm) { return json{{"name", nm}}; }
json comp(const char* nm, json data) { return json{{"name", nm}, {"data", std::move(data)}}; }

// A net named "0" is the SPICE global-ground idiom of the CIAS emitter (a net not exposed at a
// port takes its own name as the node name; node 0 is ground). It is used ONLY where an internal
// TBAS atom needs a DC reference (oscillator/latch/timer returns) — every such net still has the
// schema-required >= 2 endpoints.

// ── synchronousRectifier (pre-existing) ───────────────────────────────────────────────────────

// Current-sensed SR: two comparators read the tank-current sign (senseP/senseM) and gate the two
// rectifying diagonals (gA/gB). The robust choice for resonant converters — no per-switch
// self-feedback.
json sync_rect_current(const std::string& name, double vH, double vL, double thr, double hyst) {
    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("senseP"), port("senseM"), port("gA"), port("gB")});
    leaf["components"] = json::array({
        comp("CmpA", comparator_atom(vH, vL, thr, hyst)),
        comp("CmpB", comparator_atom(vH, vL, thr, hyst))});
    leaf["connections"] = json::array({
        cn("senseP", {C("CmpA","inPlus"),  C("CmpB","inMinus"), P("senseP")}),
        cn("senseM", {C("CmpA","inMinus"), C("CmpB","inPlus"),  P("senseM")}),
        cn("gA", {C("CmpA","out"), P("gA")}), cn("gB", {C("CmpB","out"), P("gB")})});
    return leaf;
}

// Vds-emulating SR: one comparator per SR switch senses that switch's drain-source (nodeC/nodeD vs
// vSense/gSense) and gates it like its body diode. Provided for completeness; current sensing is
// preferred for resonant SR (Vds emulation is a tight self-feedback loop prone to mis-commutation).
json sync_rect_vds(const std::string& name, double vH, double vL, double thr, double hyst) {
    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("nodeC"), port("nodeD"), port("vSense"), port("gSense"),
                                 port("gE"), port("gF"), port("gG"), port("gH")});
    leaf["components"] = json::array({
        comp("CmpE", comparator_atom(vH, vL, thr, hyst)),
        comp("CmpF", comparator_atom(vH, vL, thr, hyst)),
        comp("CmpG", comparator_atom(vH, vL, thr, hyst)),
        comp("CmpH", comparator_atom(vH, vL, thr, hyst))});
    leaf["connections"] = json::array({
        cn("nodeC",  {C("CmpE","inPlus"),  C("CmpF","inMinus"), P("nodeC")}),
        cn("nodeD",  {C("CmpG","inPlus"),  C("CmpH","inMinus"), P("nodeD")}),
        cn("vSense", {C("CmpE","inMinus"), C("CmpG","inMinus"), P("vSense")}),
        cn("gSense", {C("CmpF","inPlus"),  C("CmpH","inPlus"),  P("gSense")}),
        cn("gE", {C("CmpE","out"), P("gE")}), cn("gF", {C("CmpF","out"), P("gF")}),
        cn("gG", {C("CmpG","out"), P("gG")}), cn("gH", {C("CmpH","out"), P("gH")})});
    return leaf;
}

json lower_synchronous_rectifier(const json& b, const std::string& name) {
    const std::string sensing = req_str(b, "sensing");
    const double vHigh = req_num(b, "driveHigh");
    const double vLow  = req_num(b, "driveLow");
    // threshold: commutation point on the sensed signal; absent = 0 (sign crossing), matching
    // the ideal-commutation semantics. hysteresis: schema documents "0 (or absent) is ideal".
    const double thr   = b.value("threshold", 0.0);
    const double hyst  = b.value("hysteresis", 0.0);
    if (hyst < 0.0)
        throw std::runtime_error("ctas_to_cias: negative hysteresis");
    // behavioral.topology (fullBridge/halfBridge/centerTapped) does not change the leaf: both
    // realizations expose two commutation groups (gA/gB or gE..gH) and the TAS stage wiring maps
    // them onto the actual rectifier switches. It is wiring metadata, not a realization input.
    if (sensing == "current")     return sync_rect_current(name, vHigh, vLow, thr, hyst);
    if (sensing == "drainSource") return sync_rect_vds(name, vHigh, vLow, thr, hyst);
    throw std::runtime_error("ctas_to_cias: sensing '" + sensing + "' not supported (current|drainSource)");
}

// ── voltageModePWM (RFC 0001 §6.1) ────────────────────────────────────────────────────────────
//
// Error input pair (errP/errM) vs a TBAS sawtooth ramp at switchingFrequency, amplitude
// rampAmplitude, offset 0 -> AAS comparator -> gate at driveHigh/driveLow. The ramp RIDES on
// errM (its outMinus is wired to the errM net), so the comparator sees the truly differential
// (errP-errM) - ramp(t): gate is high while the error exceeds the ramp, duty = err/rampAmplitude.
// maximumDutyCycle present -> a `controlled` min() block clamps the error seen by the comparator
// at maximumDutyCycle*rampAmplitude (the ramp crossing that corresponds to the duty limit).
json lower_voltage_mode_pwm(const json& b, const std::string& name) {
    const double fsw  = req_num(b, "switchingFrequency");
    const double ramp = req_num(b, "rampAmplitude");
    const double vH   = req_num(b, "driveHigh");
    const double vL   = req_num(b, "driveLow");
    const std::optional<double> dmax = opt_num(b, "maximumDutyCycle");
    if (dmax && (*dmax <= 0.0 || *dmax >= 1.0))
        throw std::runtime_error("ctas_to_cias: maximumDutyCycle must lie in (0, 1)");

    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("errP"), port("errM"), port("gate")});
    json comps = json::array({comp("Ramp", oscillator_atom("sawtooth", fsw, ramp, 0.0)),
                              comp("Cmp",  comparator_atom(vH, vL))});
    json conns = json::array({cn("ramp", {C("Ramp","outPlus"), C("Cmp","inMinus")}),
                              cn("gate", {C("Cmp","out"), P("gate")})});
    if (dmax) {
        comps.push_back(comp("Clamp", controlled_v_atom(
            "min(v(in,ref),(" + num(*dmax * ramp) + "))")));
        conns.push_back(cn("errP", {P("errP"), C("Clamp","in")}));
        conns.push_back(cn("errM", {P("errM"), C("Ramp","outMinus"), C("Clamp","ref")}));
        conns.push_back(cn("ctl",  {C("Clamp","out"), C("Cmp","inPlus")}));
    } else {
        conns.push_back(cn("errP", {P("errP"), C("Cmp","inPlus")}));
        conns.push_back(cn("errM", {P("errM"), C("Ramp","outMinus")}));
    }
    leaf["components"] = comps;
    leaf["connections"] = conns;
    return leaf;
}

// ── peakCurrentMode (RFC 0001 §6.2) ───────────────────────────────────────────────────────────
//
// TBAS square clock (duty 0.05 — the narrow set pulse of the clock template; amplitude 1/offset 0
// are its internal logic levels, matched by the latch setThreshold 0.5) sets a reset-dominant
// TBAS latch whose output rails are driveHigh/driveLow. A `controlled` block forms the comparator
// signal Ri*v(isenseP,isenseM) (+ the slope-compensation sawtooth when slopeCompensation [V/s] is
// present, emitted as a same-frequency sawtooth of amplitude slopeCompensation*T volts); the
// reset comparator (internal 0/1 logic) trips when that signal reaches the differential error
// (errP/errM) — the peak-current comparison. The sensed current enters as a voltage on
// isenseP/isenseM carrying 1 V/A (the TAS stage wires a current sense there); currentSenseGain
// Ri [V/A] is applied inside. maximumDutyCycle present -> a second comparator on a sawtooth at
// the clock frequency (high past dmax of each period) is OR'd (max()) into the latch reset.
json lower_peak_current_mode(const json& b, const std::string& name) {
    const double fsw = req_num(b, "switchingFrequency");
    const double ri  = req_num(b, "currentSenseGain");
    const double vH  = req_num(b, "driveHigh");
    const double vL  = req_num(b, "driveLow");
    const std::optional<double> slope = opt_num(b, "slopeCompensation");
    const std::optional<double> dmax  = opt_num(b, "maximumDutyCycle");
    if (dmax && (*dmax <= 0.0 || *dmax >= 1.0))
        throw std::runtime_error("ctas_to_cias: maximumDutyCycle must lie in (0, 1)");

    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("errP"), port("errM"),
                                 port("isenseP"), port("isenseM"), port("gate")});
    std::string senseExpr = "(" + num(ri) + ")*v(isP,isM)";
    if (slope) senseExpr += "+v(rampP,rampM)";
    json comps = json::array({
        comp("Clk",   oscillator_atom("square", fsw, 1.0, 0.0, 0.05)),
        comp("Sense", controlled_v_atom(senseExpr)),
        comp("CmpR",  comparator_atom(1.0, 0.0)),
        comp("Latch", latch_atom(0.5, 0.5, vH, vL, "reset"))});
    json conns = json::array({
        cn("clk",     {C("Clk","outPlus"),  C("Latch","setPlus")}),
        cn("isenseP", {P("isenseP"), C("Sense","isP")}),
        cn("isenseM", {P("isenseM"), C("Sense","isM")}),
        // The sense block rides on errM so the reset comparator sees sense - (errP-errM).
        cn("errM",    {P("errM"), C("Sense","ref")}),
        cn("errP",    {P("errP"), C("CmpR","inMinus")}),
        cn("sense",   {C("Sense","out"), C("CmpR","inPlus")}),
        cn("gate",    {C("Latch","outPlus"), P("gate")})});
    // Internal logic reference (see the net-"0" note above): clock return, latch returns, and
    // the OR block's reference all share SPICE ground.
    json gnd = json::array({C("Clk","outMinus"), C("Latch","setMinus"),
                            C("Latch","resetMinus"), C("Latch","outMinus")});
    if (slope) {
        // slopeCompensation [V/s] over one period T = slope/fsw volts of sawtooth amplitude.
        // The sawtooth return and the sense block's rampM pin sit on ground (the sawtooth needs
        // a DC reference — a floating source pair sensed only by a B-source is singular).
        comps.push_back(comp("Slope", oscillator_atom("sawtooth", fsw, *slope / fsw, 0.0)));
        conns.push_back(cn("slopeP", {C("Slope","outPlus"), C("Sense","rampP")}));
        gnd.push_back(C("Slope","outMinus"));
        gnd.push_back(C("Sense","rampM"));
    }
    if (dmax) {
        comps.push_back(comp("RampD", oscillator_atom("sawtooth", fsw, 1.0, 0.0)));
        comps.push_back(comp("CmpD",  comparator_atom(1.0, 0.0, *dmax)));
        comps.push_back(comp("OrR",   controlled_v_atom("max(v(a,ref),v(b,ref))")));
        conns.push_back(cn("rampD",   {C("RampD","outPlus"), C("CmpD","inPlus")}));
        conns.push_back(cn("rstPwm",  {C("CmpR","out"), C("OrR","a")}));
        conns.push_back(cn("rstDuty", {C("CmpD","out"), C("OrR","b")}));
        conns.push_back(cn("reset",   {C("OrR","out"), C("Latch","resetPlus")}));
        gnd.push_back(C("RampD","outMinus"));
        gnd.push_back(C("CmpD","inMinus"));
        gnd.push_back(C("OrR","ref"));
    } else {
        conns.push_back(cn("reset", {C("CmpR","out"), C("Latch","resetPlus")}));
    }
    conns.push_back(json{{"name", "0"}, {"endpoints", gnd}});
    leaf["components"] = comps;
    leaf["connections"] = conns;
    return leaf;
}

// ── constantOnTime (RFC 0001 §6.3) ────────────────────────────────────────────────────────────
//
// Feedback comparator (differential errP/errM, internal 0/1 logic) triggers a TBAS monostable
// (onTime, retriggerable false, risingEdge, threshold 0.5 = half the comparator swing) whose
// output rails are driveHigh/driveLow -> gate. minimumOffTime present -> a second monostable
// ("Blank", internal 0/1 logic) fires on the gate's FALLING edge (threshold at the midpoint of
// the drive swing) for minimumOffTime, and a `controlled` AND block (product of 0/1 signals)
// gates the comparator's trigger so the one-shot cannot refire during the blanking window.
json lower_constant_on_time(const json& b, const std::string& name) {
    const double onTime = req_num(b, "onTime");
    const double vH = req_num(b, "driveHigh");
    const double vL = req_num(b, "driveLow");
    const std::optional<double> minOff = opt_num(b, "minimumOffTime");
    if (minOff && vH <= vL)
        throw std::runtime_error(
            "ctas_to_cias: constantOnTime with minimumOffTime needs driveHigh > driveLow — the "
            "blanking one-shot detects the gate's turn-off edge at the midpoint of the drive swing");

    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("errP"), port("errM"), port("gate")});
    json comps = json::array({
        comp("CmpT",  comparator_atom(1.0, 0.0)),
        comp("Pulse", monostable_atom(vH, vL, 0.5, "risingEdge", onTime))});
    json conns = json::array({
        cn("errP", {P("errP"), C("CmpT","inPlus")}),
        cn("errM", {P("errM"), C("CmpT","inMinus")})});
    if (minOff) {
        comps.push_back(comp("Blank",
            monostable_atom(1.0, 0.0, (vH + vL) / 2.0, "fallingEdge", *minOff)));
        comps.push_back(comp("Arm", controlled_v_atom("v(trg,ref)*(1-v(blk,ref))")));
        conns.push_back(cn("req",   {C("CmpT","out"), C("Arm","trg")}));
        conns.push_back(cn("blank", {C("Blank","outPlus"), C("Arm","blk")}));
        conns.push_back(cn("trg",   {C("Arm","out"), C("Pulse","trgPlus")}));
        conns.push_back(cn("gate",  {C("Pulse","outPlus"), P("gate"), C("Blank","trgPlus")}));
        conns.push_back(cn("0",     {C("Pulse","trgMinus"), C("Pulse","outMinus"),
                                     C("Blank","trgMinus"), C("Blank","outMinus"),
                                     C("Arm","ref")}));
    } else {
        conns.push_back(cn("trg",  {C("CmpT","out"), C("Pulse","trgPlus")}));
        conns.push_back(cn("gate", {C("Pulse","outPlus"), P("gate")}));
        conns.push_back(cn("0",    {C("Pulse","trgMinus"), C("Pulse","outMinus")}));
    }
    leaf["components"] = comps;
    leaf["connections"] = conns;
    return leaf;
}

// ── frequencyControl (RFC 0001 §6.4) ──────────────────────────────────────────────────────────
//
// Error input (errP/errM) steers a TBAS triangle VCO (frequency = centerFrequency, gain =
// vcoGain, amplitude 1, offset 0 — span 0..1). Two comparators on the triangle derive the
// complementary gates: gateA while tri > 0.5+delta, gateB while tri < 0.5-delta. With the CIAS
// triangle span convention (offset..offset+amplitude over T/2) the slope is 2*amplitude*f, so
// deadTime = 2*delta/slope  =>  delta = deadTime*amplitude*centerFrequency (exact at the center
// frequency; the ideal FM template's dead time scales inversely with instantaneous frequency).
// deadTime ABSENT -> delta = 0 exactly: both comparators sit at the 0.5 midpoint, the ideal
// complementary pair — the deadTime->0 limit of the same construction (the schema's stated
// minimum), not a fabricated numeric default.
// minimum/maximumFrequency present -> the control voltage is clamped through a `controlled`
// min()/max() block BEFORE the VCO ctrl pins, at v_bound = (f_bound - centerFrequency)/vcoGain.
json lower_frequency_control(const json& b, const std::string& name) {
    const double f0 = req_num(b, "centerFrequency");
    const double k  = req_num(b, "vcoGain");
    const double vH = req_num(b, "driveHigh");
    const double vL = req_num(b, "driveLow");
    const std::optional<double> fMin = opt_num(b, "minimumFrequency");
    const std::optional<double> fMax = opt_num(b, "maximumFrequency");
    const std::optional<double> dead = opt_num(b, "deadTime");
    if (fMin && fMax && *fMin >= *fMax)
        throw std::runtime_error("ctas_to_cias: minimumFrequency >= maximumFrequency");
    if ((fMin || fMax) && k == 0.0)
        throw std::runtime_error(
            "ctas_to_cias: frequency clamps cannot be mapped onto the VCO control pin with "
            "vcoGain 0");
    double delta = 0.0;
    if (dead) {
        if (*dead < 0.0)
            throw std::runtime_error("ctas_to_cias: negative deadTime");
        delta = *dead * f0;  // deadTime * slope/2, slope = 2*amplitude*f0, amplitude = 1
        if (delta >= 0.5)
            throw std::runtime_error(
                "ctas_to_cias: deadTime >= half the period at centerFrequency — no gate "
                "pulses remain");
    }

    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("errP"), port("errM"), port("gateA"), port("gateB")});
    json comps = json::array({
        comp("Vco",  oscillator_atom("triangle", f0, 1.0, 0.0, std::nullopt, k)),
        comp("CmpA", comparator_atom(vH, vL, 0.5 + delta)),
        // gateB trips while tri < 0.5-delta: V(ref)-V(tri) > delta-0.5.
        comp("CmpB", comparator_atom(vH, vL, delta - 0.5))});
    json conns = json::array({
        cn("tri",   {C("Vco","outPlus"), C("CmpA","inPlus"), C("CmpB","inMinus")}),
        // VCO return + comparator references share SPICE ground (net-"0" idiom, note above).
        cn("0",     {C("Vco","outMinus"), C("CmpA","inMinus"), C("CmpB","inPlus")}),
        cn("gateA", {C("CmpA","out"), P("gateA")}),
        cn("gateB", {C("CmpB","out"), P("gateB")})});
    if (fMin || fMax) {
        // Bound voltages on the ctrl pin; a negative vcoGain swaps which frequency bound is the
        // lower/upper control-voltage bound.
        std::optional<double> vLo, vHi;
        auto vOf = [&](double f) { return (f - f0) / k; };
        if (k > 0.0) {
            if (fMin) vLo = vOf(*fMin);
            if (fMax) vHi = vOf(*fMax);
        } else {
            if (fMin) vHi = vOf(*fMin);
            if (fMax) vLo = vOf(*fMax);
        }
        std::string expr = "v(in,ref)";
        if (vHi) expr = "min(" + expr + ",(" + num(*vHi) + "))";
        if (vLo) expr = "max(" + expr + ",(" + num(*vLo) + "))";
        comps.push_back(comp("Clamp", controlled_v_atom(expr)));
        conns.push_back(cn("errP", {P("errP"), C("Clamp","in")}));
        conns.push_back(cn("errM", {P("errM"), C("Clamp","ref"), C("Vco","ctrlMinus")}));
        conns.push_back(cn("vctl", {C("Clamp","out"), C("Vco","ctrlPlus")}));
    } else {
        conns.push_back(cn("errP", {P("errP"), C("Vco","ctrlPlus")}));
        conns.push_back(cn("errM", {P("errM"), C("Vco","ctrlMinus")}));
    }
    leaf["components"] = comps;
    leaf["connections"] = conns;
    return leaf;
}

// ── pfcAverageCurrentMode (RFC 0001 §6.5) ─────────────────────────────────────────────────────
//
// AAS multiplier (unity gain — the ideal reference former) of the rectified-line shape input
// (`line`) and the voltage-loop error (`err`) forms the current reference. A `controlled` block
// computes the current error iref - Ri*v(isenseP,isenseM) (isense carries the sensed current as
// 1 V/A, like peakCurrentMode); the AAS integrator is the current error amplifier, and its
// output is compared against a unit sawtooth at switchingFrequency — the average-current-mode
// PWM modulator — to drive the gate at driveHigh/driveLow.
// Realization constants of the ideal template (documented, not data): multiplier gain 1; ramp
// amplitude 1 (unity modulator gain); integrator gain 2*pi*fsw/10 (current-loop crossover a
// decade below the switching frequency, the classic average-CM placement) with anti-windup
// clamps 0..1 (the ramp span). `line` and `err` are ground-referenced single-ended inputs —
// the AAS multiplier atom is single-ended by definition.
json lower_pfc_average_current_mode(const json& b, const std::string& name) {
    const double fsw = req_num(b, "switchingFrequency");
    const double ri  = req_num(b, "currentSenseGain");
    const double vH  = req_num(b, "driveHigh");
    const double vL  = req_num(b, "driveLow");
    const double kTwoPi = 2.0 * std::acos(-1.0);

    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("line"), port("err"),
                                 port("isenseP"), port("isenseM"), port("gate")});
    leaf["components"] = json::array({
        comp("Mult", multiplier_atom(1.0)),
        comp("Err",  controlled_v_atom("v(ir,ref)-(" + num(ri) + ")*v(isP,isM)")),
        comp("Ea",   integrator_atom(kTwoPi * fsw / 10.0, 0.0, 1.0)),
        comp("Ramp", oscillator_atom("sawtooth", fsw, 1.0, 0.0)),
        comp("Cmp",  comparator_atom(vH, vL))});
    leaf["connections"] = json::array({
        cn("line",    {P("line"), C("Mult","inA")}),
        cn("err",     {P("err"),  C("Mult","inB")}),
        cn("iref",    {C("Mult","out"), C("Err","ir")}),
        cn("isenseP", {P("isenseP"), C("Err","isP")}),
        cn("isenseM", {P("isenseM"), C("Err","isM")}),
        cn("ierr",    {C("Err","out"), C("Ea","in")}),
        cn("vca",     {C("Ea","out"), C("Cmp","inPlus")}),
        cn("ramp",    {C("Ramp","outPlus"), C("Cmp","inMinus")}),
        // Current-error reference + ramp return on SPICE ground (net-"0" idiom, note above).
        cn("0",       {C("Err","ref"), C("Ramp","outMinus")}),
        cn("gate",    {C("Cmp","out"), P("gate")})});
    return leaf;
}

} // namespace

json ctas_to_cias(const json& peas, const PEAS::Fidelity& /*fidelity*/, const std::string& name) {
    const json& ctrl = controller_of(peas);
    if (!ctrl.contains("behavioral"))
        throw std::runtime_error("ctas_to_cias: controller has no 'behavioral' block (only the ideal "
                                 "behavioural control law is lowered to CIAS so far)");
    const json& b = ctrl.at("behavioral");
    // controlScheme is the schema's oneOf discriminator; each scheme's lowering pulls exactly the
    // fields its branch carries. No magic defaults anywhere: a field the lowering needs (the
    // drive rails included — the schema documents no default for them) must be present or we
    // throw; optionals whose ABSENCE has a documented meaning (no clamp, no slope compensation,
    // no blanking, zero dead time) simply omit the corresponding sub-network.
    const std::string scheme = req_str(b, "controlScheme");
    if (scheme == "synchronousRectifier")   return lower_synchronous_rectifier(b, name);
    if (scheme == "voltageModePWM")         return lower_voltage_mode_pwm(b, name);
    if (scheme == "peakCurrentMode")        return lower_peak_current_mode(b, name);
    if (scheme == "constantOnTime")         return lower_constant_on_time(b, name);
    if (scheme == "frequencyControl")       return lower_frequency_control(b, name);
    if (scheme == "pfcAverageCurrentMode")  return lower_pfc_average_current_mode(b, name);
    throw std::runtime_error(
        "ctas_to_cias: controlScheme '" + scheme + "' not supported "
        "(synchronousRectifier|voltageModePWM|peakCurrentMode|constantOnTime|frequencyControl|"
        "pfcAverageCurrentMode)");
}

} // namespace CTAS
