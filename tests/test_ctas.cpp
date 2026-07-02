// Catch2 tests for ctas_to_cias — the controller ideal-control-law -> CIAS brick lowering.
//
// Every lowered brick is round-tripped through the REAL CIAS consumer (CiasCircuitConverter,
// linked from the sibling repo): structural validation + ngspice-dialect netlist emission, plus
// one end-to-end ngspice smoke simulation for the voltage-mode PWM brick.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
#include "CtasConverter.hpp"
#include "CiasCircuitConverter.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using json = nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {
json ctrl_doc(json behavioral) {
    return {{"controller", {{"behavioral", behavioral}}},
            {"inputs", {{"designRequirements", json::object()}}}};
}
json full_behavioral() {
    return {{"controlScheme", "synchronousRectifier"}, {"sensing", "current"},
            {"topology", "fullBridge"}, {"hysteresis", 0.005},
            {"driveHigh", 5.0}, {"driveLow", 0.0}, {"threshold", 0.0}};
}
double atom_param(const json& leaf, int comp, const char* key) {
    return leaf.at("components")[comp].at("data").at("analog").at("comparator")
               .at("behavioral").at(key).get<double>();
}
PEAS::Fidelity fid() { return PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS); }

json lower(json behavioral, const std::string& name) {
    return CTAS::ctas_to_cias(ctrl_doc(std::move(behavioral)), fid(), name);
}

// Round-trip through the CIAS consumer: the brick must pass the structural validator and emit
// an ngspice-dialect subcircuit.
std::string emit_checked(const json& leaf) {
    const auto circuit = CIAS::CiasCircuit::from_json(leaf);
    const auto problems = CIAS::validate_cias_structure(circuit);
    for (const auto& p : problems) FAIL("CIAS structural problem: " + p);
    return CIAS::CiasCircuitConverter(CIAS::CircuitSimulator::Ngspice).to_subckt(circuit);
}
} // namespace

// ── synchronousRectifier (pre-existing behavior, kept green) ──────────────────────────────────

TEST_CASE("current-sensed SR lowers to a two-comparator leaf with the given params", "[ctas]") {
    const json leaf = lower(full_behavioral(), "SR1");
    CHECK(leaf.at("components").size() == 2);
    CHECK(leaf.at("ports").size() == 4);        // senseP / senseM / gA / gB
    CHECK(atom_param(leaf, 0, "outputHigh") == 5.0);
    CHECK(atom_param(leaf, 0, "hysteresis") == 0.005);
    emit_checked(leaf);
}

TEST_CASE("drainSource sensing lowers to the four-comparator Vds leaf", "[ctas]") {
    json b = full_behavioral();
    b["sensing"] = "drainSource";
    const json leaf = lower(b, "SR2");
    CHECK(leaf.at("components").size() == 4);
    CHECK(leaf.at("ports").size() == 8);
    emit_checked(leaf);
}

TEST_CASE("absent hysteresis means 0, not a fabricated 5 mV — H11b regression", "[ctas]") {
    json b = full_behavioral();
    b.erase("hysteresis");
    const json leaf = lower(b, "SR3");
    CHECK(atom_param(leaf, 0, "hysteresis") == 0.0);
}

TEST_CASE("missing drive rails / sensing / controlScheme throw — no magic defaults", "[ctas]") {
    for (const char* key : {"driveHigh", "driveLow", "sensing", "controlScheme"}) {
        json b = full_behavioral();
        b.erase(key);
        CHECK_THROWS_WITH(lower(b, "SRx"), ContainsSubstring(key));
    }
}

TEST_CASE("negative hysteresis throws", "[ctas]") {
    json b = full_behavioral();
    b["hysteresis"] = -0.001;
    CHECK_THROWS_WITH(lower(b, "SRn"), ContainsSubstring("negative hysteresis"));
}

TEST_CASE("unknown control scheme and missing behavioral throw", "[ctas]") {
    json b = full_behavioral();
    b["controlScheme"] = "burstMode";   // not a schema scheme
    CHECK_THROWS_WITH(lower(b, "SRu"), ContainsSubstring("not supported"));
    json doc = {{"controller", {{"manufacturerInfo", {{"name", "TI"}}}}}};
    CHECK_THROWS_WITH(CTAS::ctas_to_cias(doc, fid(), "SRd"), ContainsSubstring("behavioral"));
}

// ── voltageModePWM ────────────────────────────────────────────────────────────────────────────

TEST_CASE("voltageModePWM lowers to ramp + comparator; the ramp rides on errM", "[ctas][vmp]") {
    const json leaf = lower({{"controlScheme", "voltageModePWM"},
                             {"switchingFrequency", 100000.0}, {"rampAmplitude", 1.0},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "vmp");
    CHECK(leaf.at("components").size() == 2);
    CHECK(leaf.at("ports").size() == 3);        // errP / errM / gate
    const std::string net = emit_checked(leaf);
    // Sawtooth at fsw with rampAmplitude span, returned to the errM net (floating ramp).
    CHECK_THAT(net, ContainsSubstring("VRamp ramp errM PULSE(0 1 0 9.99e-06 1e-08 0 1e-05)"));
    // Comparator: errP vs ramp, drive rails 12/0, ideal (Vt=0 Vh=0).
    CHECK_THAT(net, ContainsSubstring("SCmp gate Cmp__vh errP ramp CMP_Cmp"));
    CHECK_THAT(net, ContainsSubstring(".model CMP_Cmp SW(Vt=0 Vh=0 Ron=1 Roff=1e9)"));
    CHECK_THAT(net, ContainsSubstring("VCmp_vh Cmp__vh 0 12"));
}

TEST_CASE("voltageModePWM maximumDutyCycle inserts the controlled min() clamp", "[ctas][vmp]") {
    const json leaf = lower({{"controlScheme", "voltageModePWM"},
                             {"switchingFrequency", 100000.0}, {"rampAmplitude", 1.0},
                             {"maximumDutyCycle", 0.6},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "vmpc");
    CHECK(leaf.at("components").size() == 3);
    const std::string net = emit_checked(leaf);
    // min(err, dmax*rampAmplitude) feeds the comparator instead of the raw error.
    CHECK_THAT(net, ContainsSubstring("BClamp ctl errM V=min(V(errP,errM),(0.6))"));
    CHECK_THAT(net, ContainsSubstring("SCmp gate Cmp__vh ctl ramp CMP_Cmp"));
}

TEST_CASE("voltageModePWM missing/invalid fields throw — no magic defaults", "[ctas][vmp]") {
    const json base = {{"controlScheme", "voltageModePWM"},
                       {"switchingFrequency", 100000.0}, {"rampAmplitude", 1.0},
                       {"driveHigh", 12.0}, {"driveLow", 0.0}};
    for (const char* key : {"switchingFrequency", "rampAmplitude", "driveHigh", "driveLow"}) {
        json b = base;
        b.erase(key);
        CHECK_THROWS_WITH(lower(b, "vmx"), ContainsSubstring(key));
    }
    json b = base;
    b["maximumDutyCycle"] = 1.2;
    CHECK_THROWS_WITH(lower(b, "vmx"), ContainsSubstring("maximumDutyCycle"));
}

// ── peakCurrentMode ───────────────────────────────────────────────────────────────────────────

TEST_CASE("peakCurrentMode lowers to clock + sense gain + comparator + reset-dominant latch",
          "[ctas][pcm]") {
    const json leaf = lower({{"controlScheme", "peakCurrentMode"},
                             {"switchingFrequency", 100000.0}, {"currentSenseGain", 0.05},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "pcm");
    CHECK(leaf.at("components").size() == 4);   // Clk / Sense / CmpR / Latch
    CHECK(leaf.at("ports").size() == 5);        // errP / errM / isenseP / isenseM / gate
    const std::string net = emit_checked(leaf);
    // Square clock, duty 0.05, grounded return, sets the latch.
    CHECK_THAT(net, ContainsSubstring("VClk clk 0 PULSE(0 1 0 1e-08 1e-08 5e-07 1e-05)"));
    // Ri gain block: linear VCVS shortcut -> native E element (sense rides on errM).
    CHECK_THAT(net, ContainsSubstring("ESense sense errM isenseP isenseM 0.05"));
    // Reset-dominant latch: the RESET comparison is the OUTER ternary; rails 12/0 on the gate.
    CHECK_THAT(net, ContainsSubstring(
        "BLatch_st Latch__d 0 V=(V(reset,0)>(0.5))?(0):((V(clk,0)>(0.5))?(1):(V(Latch__q)))"));
    CHECK_THAT(net, ContainsSubstring("BLatch gate 0 V=(0)+((12)-(0))*V(Latch__q)"));
}

TEST_CASE("peakCurrentMode slope compensation and max-duty clamp add their sub-networks",
          "[ctas][pcm]") {
    const json leaf = lower({{"controlScheme", "peakCurrentMode"},
                             {"switchingFrequency", 100000.0}, {"currentSenseGain", 0.05},
                             {"slopeCompensation", 500000.0}, {"maximumDutyCycle", 0.8},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "pcmx");
    CHECK(leaf.at("components").size() == 8);   // + Slope / RampD / CmpD / OrR
    const std::string net = emit_checked(leaf);
    // Slope ramp amplitude = slopeCompensation * T = 5e5 / 1e5 = 5 V, grounded return.
    CHECK_THAT(net, ContainsSubstring("VSlope slopeP 0 PULSE(0 5 0 9.99e-06 1e-08 0 1e-05)"));
    // The sense signal becomes Ri*isense + slope ramp (behavioral B, no linear shortcut).
    CHECK_THAT(net, ContainsSubstring(
        "BSense sense errM V=(0.05)*V(isenseP,isenseM)+V(slopeP,0)"));
    // Max-duty comparator on its own sawtooth (Vt = dmax), OR'd (max) into the latch reset.
    CHECK_THAT(net, ContainsSubstring(".model CMP_CmpD SW(Vt=0.8 Vh=0 Ron=1 Roff=1e9)"));
    CHECK_THAT(net, ContainsSubstring("BOrR reset 0 V=max(V(rstPwm,0),V(rstDuty,0))"));
}

TEST_CASE("peakCurrentMode missing fields throw", "[ctas][pcm]") {
    const json base = {{"controlScheme", "peakCurrentMode"},
                       {"switchingFrequency", 100000.0}, {"currentSenseGain", 0.05},
                       {"driveHigh", 12.0}, {"driveLow", 0.0}};
    for (const char* key : {"switchingFrequency", "currentSenseGain", "driveHigh", "driveLow"}) {
        json b = base;
        b.erase(key);
        CHECK_THROWS_WITH(lower(b, "pcx"), ContainsSubstring(key));
    }
}

// ── constantOnTime ────────────────────────────────────────────────────────────────────────────

TEST_CASE("constantOnTime lowers to feedback comparator + non-retriggerable one-shot",
          "[ctas][cot]") {
    const json leaf = lower({{"controlScheme", "constantOnTime"}, {"onTime", 5e-6},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "cot");
    CHECK(leaf.at("components").size() == 2);   // CmpT / Pulse
    CHECK(leaf.at("ports").size() == 3);        // errP / errM / gate
    const std::string net = emit_checked(leaf);
    // One-shot triggers at half the 0/1 comparator swing, rising edge, width = onTime.
    CHECK_THAT(net, ContainsSubstring("BPulse_lvl Pulse__lvl 0 V=(V(trg,0)>(0.5))?(1):(0)"));
    CHECK_THAT(net, ContainsSubstring("(V(Pulse__tr)>=(5e-06))"));
    CHECK_THAT(net, ContainsSubstring("BPulse gate 0 V=(0)+((12)-(0))*V(Pulse__q)"));
}

TEST_CASE("constantOnTime minimumOffTime adds the blanking one-shot gating the trigger",
          "[ctas][cot]") {
    const json leaf = lower({{"controlScheme", "constantOnTime"}, {"onTime", 5e-6},
                             {"minimumOffTime", 2e-6},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "cotb");
    CHECK(leaf.at("components").size() == 4);   // + Blank / Arm
    const std::string net = emit_checked(leaf);
    // Blanking one-shot fires on the gate FALLING edge at the drive-swing midpoint (6 V).
    CHECK_THAT(net, ContainsSubstring("BBlank_lvl Blank__lvl 0 V=(V(gate,0)<(6))?(1):(0)"));
    CHECK_THAT(net, ContainsSubstring("(V(Blank__tr)>=(2e-06))"));
    // AND gate (product of 0/1 signals): trigger passes only while not blanking.
    CHECK_THAT(net, ContainsSubstring("BArm trg 0 V=V(req,0)*(1-V(blank,0))"));
}

TEST_CASE("constantOnTime throws on missing fields and unusable drive swing", "[ctas][cot]") {
    const json base = {{"controlScheme", "constantOnTime"}, {"onTime", 5e-6},
                       {"driveHigh", 12.0}, {"driveLow", 0.0}};
    for (const char* key : {"onTime", "driveHigh", "driveLow"}) {
        json b = base;
        b.erase(key);
        CHECK_THROWS_WITH(lower(b, "ctx"), ContainsSubstring(key));
    }
    // The blanking one-shot needs an ordered gate swing to detect the turn-off edge.
    json b = base;
    b["minimumOffTime"] = 2e-6;
    b["driveHigh"] = 0.0;
    CHECK_THROWS_WITH(lower(b, "ctx"), ContainsSubstring("driveHigh > driveLow"));
}

// ── frequencyControl ──────────────────────────────────────────────────────────────────────────

TEST_CASE("frequencyControl lowers to a triangle VCO + complementary comparator pair",
          "[ctas][fm]") {
    const json base = {{"controlScheme", "frequencyControl"},
                       {"centerFrequency", 100000.0}, {"vcoGain", 50000.0},
                       {"driveHigh", 12.0}, {"driveLow", 0.0}};
    const json leaf = lower(base, "fm");
    CHECK(leaf.at("components").size() == 3);   // Vco / CmpA / CmpB
    CHECK(leaf.at("ports").size() == 4);        // errP / errM / gateA / gateB
    const std::string net = emit_checked(leaf);
    // Phase-accumulator VCO steered directly by the error pair.
    CHECK_THAT(net, ContainsSubstring("BVco_f 0 Vco__ph I=max(0,(100000)+(50000)*V(errP,errM))"));
    // deadTime absent -> delta = 0: both comparators at exactly the 0.5 midpoint — the ideal
    // complementary pair (the deadTime->0 limit of the construction, per the lowering contract).
    CHECK_THAT(net, ContainsSubstring(".model CMP_CmpA SW(Vt=0.5 Vh=0 Ron=1 Roff=1e9)"));
    CHECK_THAT(net, ContainsSubstring(".model CMP_CmpB SW(Vt=-0.5 Vh=0 Ron=1 Roff=1e9)"));

    // deadTime 1 us at 100 kHz, triangle slope 2*amp*f0 -> delta = deadTime*f0 = 0.1.
    json bd = base;
    bd["deadTime"] = 1e-6;
    const std::string netd = emit_checked(lower(bd, "fmd"));
    CHECK_THAT(netd, ContainsSubstring(".model CMP_CmpA SW(Vt=0.6 Vh=0 Ron=1 Roff=1e9)"));
    CHECK_THAT(netd, ContainsSubstring(".model CMP_CmpB SW(Vt=-0.4 Vh=0 Ron=1 Roff=1e9)"));
}

TEST_CASE("frequencyControl clamps map onto control-voltage min()/max() before the ctrl pins",
          "[ctas][fm]") {
    const json leaf = lower({{"controlScheme", "frequencyControl"},
                             {"centerFrequency", 100000.0}, {"vcoGain", 50000.0},
                             {"minimumFrequency", 80000.0}, {"maximumFrequency", 120000.0},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "fmc");
    CHECK(leaf.at("components").size() == 4);   // + Clamp
    const std::string net = emit_checked(leaf);
    // v_bound = (f_bound - f0)/k: (80k-100k)/50k = -0.4, (120k-100k)/50k = 0.4.
    CHECK_THAT(net, ContainsSubstring("BClamp vctl errM V=max(min(V(errP,errM),(0.4)),(-0.4))"));
    CHECK_THAT(net, ContainsSubstring("I=max(0,(100000)+(50000)*V(vctl,errM))"));
}

TEST_CASE("frequencyControl invalid parameter combinations throw", "[ctas][fm]") {
    const json base = {{"controlScheme", "frequencyControl"},
                       {"centerFrequency", 100000.0}, {"vcoGain", 50000.0},
                       {"driveHigh", 12.0}, {"driveLow", 0.0}};
    for (const char* key : {"centerFrequency", "vcoGain", "driveHigh", "driveLow"}) {
        json b = base;
        b.erase(key);
        CHECK_THROWS_WITH(lower(b, "fmx"), ContainsSubstring(key));
    }
    json b = base;                              // clamp with zero VCO gain is unmappable
    b["vcoGain"] = 0.0;
    b["maximumFrequency"] = 120000.0;
    CHECK_THROWS_WITH(lower(b, "fmx"), ContainsSubstring("vcoGain 0"));
    b = base;                                   // inverted clamp window
    b["minimumFrequency"] = 120000.0;
    b["maximumFrequency"] = 80000.0;
    CHECK_THROWS_WITH(lower(b, "fmx"), ContainsSubstring("minimumFrequency >= maximumFrequency"));
    b = base;                                   // dead time swallowing the whole half period
    b["deadTime"] = 6e-6;
    CHECK_THROWS_WITH(lower(b, "fmx"), ContainsSubstring("deadTime"));
}

// ── pfcAverageCurrentMode ─────────────────────────────────────────────────────────────────────

TEST_CASE("pfcAverageCurrentMode lowers to multiplier -> current error -> integrator -> PWM",
          "[ctas][pfc]") {
    const json leaf = lower({{"controlScheme", "pfcAverageCurrentMode"},
                             {"switchingFrequency", 100000.0}, {"currentSenseGain", 0.05},
                             {"driveHigh", 12.0}, {"driveLow", 0.0}}, "pfc");
    CHECK(leaf.at("components").size() == 5);   // Mult / Err / Ea / Ramp / Cmp
    CHECK(leaf.at("ports").size() == 5);        // line / err / isenseP / isenseM / gate
    const std::string net = emit_checked(leaf);
    // Unity multiplier of line shape and voltage-loop error -> current reference.
    CHECK_THAT(net, ContainsSubstring("BMult iref 0 V=1*V(line)*V(err)"));
    // Current error: iref - Ri*isense.
    CHECK_THAT(net, ContainsSubstring("BErr ierr 0 V=V(iref,0)-(0.05)*V(isenseP,isenseM)"));
    // Integrator current error amp: gain 2*pi*fsw/10 (crossover a decade below fsw).
    CHECK_THAT(net, ContainsSubstring("I=62831.85307*(V(ierr)-(0))"));
    // PWM comparator of the error-amp output against the unit sawtooth.
    CHECK_THAT(net, ContainsSubstring("VRamp ramp 0 PULSE(0 1 0 9.99e-06 1e-08 0 1e-05)"));
    CHECK_THAT(net, ContainsSubstring("SCmp gate Cmp__vh vca ramp CMP_Cmp"));
}

TEST_CASE("pfcAverageCurrentMode missing fields throw", "[ctas][pfc]") {
    const json base = {{"controlScheme", "pfcAverageCurrentMode"},
                       {"switchingFrequency", 100000.0}, {"currentSenseGain", 0.05},
                       {"driveHigh", 12.0}, {"driveLow", 0.0}};
    for (const char* key : {"switchingFrequency", "currentSenseGain", "driveHigh", "driveLow"}) {
        json b = base;
        b.erase(key);
        CHECK_THROWS_WITH(lower(b, "pfx"), ContainsSubstring(key));
    }
}

// ── cross-scheme invariants ───────────────────────────────────────────────────────────────────

TEST_CASE("every scheme's component atoms are complete PEAS documents (inputs seed present)",
          "[ctas]") {
    const json bricks[] = {
        lower(full_behavioral(), "a"),
        lower({{"controlScheme", "voltageModePWM"}, {"switchingFrequency", 1e5},
               {"rampAmplitude", 1.0}, {"maximumDutyCycle", 0.5},
               {"driveHigh", 12.0}, {"driveLow", 0.0}}, "b"),
        lower({{"controlScheme", "peakCurrentMode"}, {"switchingFrequency", 1e5},
               {"currentSenseGain", 0.05}, {"slopeCompensation", 5e5},
               {"maximumDutyCycle", 0.8}, {"driveHigh", 12.0}, {"driveLow", 0.0}}, "c"),
        lower({{"controlScheme", "constantOnTime"}, {"onTime", 5e-6},
               {"minimumOffTime", 2e-6}, {"driveHigh", 12.0}, {"driveLow", 0.0}}, "d"),
        lower({{"controlScheme", "frequencyControl"}, {"centerFrequency", 1e5},
               {"vcoGain", 5e4}, {"minimumFrequency", 8e4}, {"maximumFrequency", 1.2e5},
               {"deadTime", 1e-6}, {"driveHigh", 12.0}, {"driveLow", 0.0}}, "e"),
        lower({{"controlScheme", "pfcAverageCurrentMode"}, {"switchingFrequency", 1e5},
               {"currentSenseGain", 0.05}, {"driveHigh", 12.0}, {"driveLow", 0.0}}, "f")};
    for (const json& leaf : bricks) {
        for (const json& c : leaf.at("components"))
            CHECK(c.at("data").contains("inputs"));   // PEAS requires `inputs` on every document
        emit_checked(leaf);                           // structural validity + emission round-trip
    }
}

// ngspice smoke test — ngspice was on PATH when this test was written; if it has since
// disappeared the test FAILS loudly (never silently skips) per the house rule.
TEST_CASE("ngspice smoke: voltageModePWM duty tracks the error input (and honors the clamp)",
          "[ctas][vmp][ngspice]") {
    if (std::system("which ngspice > /dev/null 2>&1") != 0)
        FAIL("ngspice not found on PATH — it was present when this smoke test was added");

    namespace fs = std::filesystem;
    const std::string tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path cir = fs::temp_directory_path() / ("ctas_pwm_smoke_" + tag + ".cir");
    const fs::path log = fs::temp_directory_path() / ("ctas_pwm_smoke_" + tag + ".log");

    // Lower the brick, emit its .subckt via the CIAS converter, instantiate it with errM
    // grounded and a DC error voltage, and measure the gate's average = the duty cycle
    // (driveHigh 1 / driveLow 0 makes the average read duty directly).
    auto run_duty = [&](const json& behavioral, const std::string& brick,
                        double verr) -> double {
        const std::string subckt = CIAS::CiasCircuitConverter(CIAS::CircuitSimulator::Ngspice)
                                       .to_subckt_json(lower(behavioral, brick));
        {
            std::ofstream f(cir);
            REQUIRE(f.good());
            f << "* CTAS voltageModePWM smoke (lowered by ctas_to_cias)\n"
              << subckt
              << "Verr errP 0 DC " << verr << "\n"
              << "X1 errP 0 gate " << brick << "\n"
              << ".tran 0.01u 200u 100u\n"
              << ".meas tran davg AVG V(gate) from=100u to=200u\n"
              << ".end\n";
        }
        const std::string cmd = "ngspice -b " + cir.string() + " > " + log.string() + " 2>&1";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::ifstream lf(log);
        std::string line;
        while (std::getline(lf, line)) {
            const auto pos = line.find("davg");
            if (pos != std::string::npos) {
                const auto eq = line.find('=', pos);
                REQUIRE(eq != std::string::npos);
                return std::stod(line.substr(eq + 1));
            }
        }
        FAIL("ngspice produced no 'davg' measurement — deck: " + cir.string());
        return 0.0;
    };

    const json pwm = {{"controlScheme", "voltageModePWM"}, {"switchingFrequency", 100000.0},
                      {"rampAmplitude", 1.0}, {"driveHigh", 1.0}, {"driveLow", 0.0}};
    // Duty = err/rampAmplitude. Tolerance 5% (same as the CIAS-side smoke).
    CHECK(std::abs(run_duty(pwm, "ctas_pwm", 0.25) - 0.25) < 0.05);
    CHECK(std::abs(run_duty(pwm, "ctas_pwm", 0.70) - 0.70) < 0.05);
    // With maximumDutyCycle 0.5, an error demanding 0.9 saturates at the clamp.
    json clamped = pwm;
    clamped["maximumDutyCycle"] = 0.5;
    CHECK(std::abs(run_duty(clamped, "ctas_pwm_clamped", 0.90) - 0.50) < 0.05);
    fs::remove(cir);
    fs::remove(log);
}
