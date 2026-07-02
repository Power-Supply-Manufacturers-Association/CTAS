// Catch2 tests for ctas_to_cias — the controller ideal-control-law -> CIAS leaf lowering.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
#include "CtasConverter.hpp"

using json = nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {
json sr_doc(json behavioral) {
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
} // namespace

TEST_CASE("current-sensed SR lowers to a two-comparator leaf with the given params", "[ctas]") {
    const json leaf = CTAS::ctas_to_cias(sr_doc(full_behavioral()), PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SR1");
    CHECK(leaf.at("components").size() == 2);
    CHECK(leaf.at("ports").size() == 4);        // senseP / senseM / gA / gB
    CHECK(atom_param(leaf, 0, "outputHigh") == 5.0);
    CHECK(atom_param(leaf, 0, "hysteresis") == 0.005);
}

TEST_CASE("drainSource sensing lowers to the four-comparator Vds leaf", "[ctas]") {
    json b = full_behavioral();
    b["sensing"] = "drainSource";
    const json leaf = CTAS::ctas_to_cias(sr_doc(b), PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SR2");
    CHECK(leaf.at("components").size() == 4);
    CHECK(leaf.at("ports").size() == 8);
}

TEST_CASE("absent hysteresis means 0, not a fabricated 5 mV — H11b regression", "[ctas]") {
    json b = full_behavioral();
    b.erase("hysteresis");
    const json leaf = CTAS::ctas_to_cias(sr_doc(b), PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SR3");
    CHECK(atom_param(leaf, 0, "hysteresis") == 0.0);
}

TEST_CASE("missing drive rails / sensing / controlScheme throw — no magic defaults", "[ctas]") {
    for (const char* key : {"driveHigh", "driveLow", "sensing", "controlScheme"}) {
        json b = full_behavioral();
        b.erase(key);
        CHECK_THROWS_WITH(CTAS::ctas_to_cias(sr_doc(b), PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SRx"),
                          ContainsSubstring(key));
    }
}

TEST_CASE("negative hysteresis throws", "[ctas]") {
    json b = full_behavioral();
    b["hysteresis"] = -0.001;
    CHECK_THROWS_WITH(CTAS::ctas_to_cias(sr_doc(b), PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SRn"),
                      ContainsSubstring("negative hysteresis"));
}

TEST_CASE("unsupported control scheme and missing behavioral throw", "[ctas]") {
    json b = full_behavioral();
    b["controlScheme"] = "peakCurrentMode";
    CHECK_THROWS_WITH(CTAS::ctas_to_cias(sr_doc(b), PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SRu"),
                      ContainsSubstring("not supported yet"));
    json doc = {{"controller", {{"manufacturerInfo", {{"name", "TI"}}}}}};
    CHECK_THROWS_WITH(CTAS::ctas_to_cias(doc, PEAS::Fidelity(PEAS::Fidelity::Origin::REQUIREMENTS), "SRd"),
                      ContainsSubstring("behavioral"));
}
