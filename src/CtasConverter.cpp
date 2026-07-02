#include "CtasConverter.hpp"
#include <stdexcept>
#include <vector>

namespace CTAS {
using nlohmann::json;

namespace {
const json& controller_of(const json& peas) {
    if (peas.contains("controller")) return peas.at("controller");
    throw std::runtime_error("ctas_to_cias: document has no 'controller'");
}

// A comparator atom carrying the agnostic ideal-behavioural block (rails / threshold / hysteresis).
json comparator_atom(double vHigh, double vLow, double thr, double hyst) {
    json atom; json& e = atom["analog"]["comparator"]["behavioral"];
    e["outputHigh"] = vHigh; e["outputLow"] = vLow; e["threshold"] = thr; e["hysteresis"] = hyst;
    return atom;
}
json C(const char* nm, const char* pin) { return json{{"component", nm}, {"pin", pin}}; }
json P(const char* nm) { return json{{"port", nm}}; }
json cn(const char* nm, std::vector<json> eps) { return json{{"name", nm}, {"endpoints", eps}}; }
json port(const char* nm) { return json{{"name", nm}}; }

// Current-sensed SR: two comparators read the tank-current sign (senseP/senseM) and gate the two
// rectifying diagonals (gA/gB). The robust choice for resonant converters — no per-switch self-feedback.
json sync_rect_current(const std::string& name, double vH, double vL, double thr, double hyst) {
    json leaf; leaf["name"] = name;
    leaf["ports"] = json::array({port("senseP"), port("senseM"), port("gA"), port("gB")});
    leaf["components"] = json::array({
        json{{"name","CmpA"},{"data",comparator_atom(vH,vL,thr,hyst)}},
        json{{"name","CmpB"},{"data",comparator_atom(vH,vL,thr,hyst)}}});
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
        json{{"name","CmpE"},{"data",comparator_atom(vH,vL,thr,hyst)}},
        json{{"name","CmpF"},{"data",comparator_atom(vH,vL,thr,hyst)}},
        json{{"name","CmpG"},{"data",comparator_atom(vH,vL,thr,hyst)}},
        json{{"name","CmpH"},{"data",comparator_atom(vH,vL,thr,hyst)}}});
    leaf["connections"] = json::array({
        cn("nodeC",  {C("CmpE","inPlus"),  C("CmpF","inMinus"), P("nodeC")}),
        cn("nodeD",  {C("CmpG","inPlus"),  C("CmpH","inMinus"), P("nodeD")}),
        cn("vSense", {C("CmpE","inMinus"), C("CmpG","inMinus"), P("vSense")}),
        cn("gSense", {C("CmpF","inPlus"),  C("CmpH","inPlus"),  P("gSense")}),
        cn("gE", {C("CmpE","out"), P("gE")}), cn("gF", {C("CmpF","out"), P("gF")}),
        cn("gG", {C("CmpG","out"), P("gG")}), cn("gH", {C("CmpH","out"), P("gH")})});
    return leaf;
}
} // namespace

json ctas_to_cias(const json& peas, const PEAS::Fidelity& /*fidelity*/, const std::string& name) {
    const json& ctrl = controller_of(peas);
    if (!ctrl.contains("behavioral"))
        throw std::runtime_error("ctas_to_cias: controller has no 'behavioral' block (only the ideal "
                                 "behavioural control law is lowered to CIAS so far)");
    const json& b = ctrl.at("behavioral");
    // No magic defaults: controlScheme is schema-required; sensing and the drive rails have no
    // documented default and no universal value (the old value_or(5.0)/value_or(5e-3) silently
    // fabricated a drive stage that contradicted both the schema and the CIAS realizer).
    auto req_str = [&](const char* k) -> std::string {
        if (!b.contains(k))
            throw std::runtime_error("ctas_to_cias: behavioral block is missing required '" +
                                     std::string(k) + "'");
        return b.at(k).get<std::string>();
    };
    auto req_num = [&](const char* k) -> double {
        if (!b.contains(k))
            throw std::runtime_error("ctas_to_cias: behavioral block is missing '" +
                                     std::string(k) + "' (no default drive rail exists)");
        return b.at(k).get<double>();
    };
    const std::string scheme  = req_str("controlScheme");
    const std::string sensing = req_str("sensing");
    const double vHigh = req_num("driveHigh");
    const double vLow  = req_num("driveLow");
    // threshold: commutation point on the sensed signal; absent = 0 (sign crossing), matching
    // the ideal-commutation semantics. hysteresis: schema documents "0 (or absent) is ideal".
    const double thr   = b.value("threshold", 0.0);
    const double hyst  = b.value("hysteresis", 0.0);
    if (hyst < 0.0)
        throw std::runtime_error("ctas_to_cias: negative hysteresis");
    // behavioral.topology (fullBridge/halfBridge/centerTapped) does not change the leaf: both
    // realizations expose two commutation groups (gA/gB or gE..gH) and the TAS stage wiring maps
    // them onto the actual rectifier switches. It is wiring metadata, not a realization input.

    if (scheme != "synchronousRectifier")
        throw std::runtime_error("ctas_to_cias: controlScheme '" + scheme + "' not supported yet");
    if (sensing == "current")     return sync_rect_current(name, vHigh, vLow, thr, hyst);
    if (sensing == "drainSource") return sync_rect_vds(name, vHigh, vLow, thr, hyst);
    throw std::runtime_error("ctas_to_cias: sensing '" + sensing + "' not supported (current|drainSource)");
}

} // namespace CTAS
