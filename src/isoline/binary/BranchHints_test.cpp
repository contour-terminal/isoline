// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::binary::BranchHintSectionName;
using isoline::binary::BranchLikelihood;
using isoline::binary::BranchSite;
using isoline::binary::CustomErrorCode;
using isoline::binary::CustomSectionError;
using isoline::binary::decodeBranchHints;
using isoline::binary::decodeModule;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::Module;
using isoline::binary::specMessage;
using isoline::binary::testing::encodeName;
using isoline::binary::testing::ModuleBuilder;

namespace
{

/// Whether the hinted module imports a function, which shifts the defined ones' indices by one.
enum class HintedImport : uint8_t
{
    None,     ///< Function 0 is the dummy, function 1 the hinted one.
    Function, ///< Function 0 is imported, 1 the dummy, 2 the hinted one.
};

/// A module of a dummy function and a hinted one, `(local i32) local.get 0 if local.get 0 br_if 0
/// end`, whose if sits at offset 5 (instruction 1) and br_if at offset 9 (instruction 3) from its
/// locals declaration, followed by a custom section of @p sections' payloads, each after the name
/// `metadata.code.branch_hint`.
/// @param sections The payloads of the branch hint sections, in module order.
/// @param import Whether a function is imported ahead of the defined ones.
std::vector<std::byte> hintedModule(std::vector<std::vector<uint8_t>> const& sections,
                                    HintedImport import = HintedImport::None)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    if (import == HintedImport::Function)
        builder.importFunction("env", "f", 0);
    builder.function(0, {}, {});
    builder.function(0, { I32 }, { 0x20, 0x00, 0x04, 0x40, 0x20, 0x00, 0x0D, 0x00, 0x0B });
    for (auto const& payload: sections)
    {
        auto section = encodeName(BranchHintSectionName);
        section.insert(section.end(), payload.begin(), payload.end());
        builder.rawSection(0, std::move(section));
    }
    return builder.build();
}

/// The features the tests decode with: 3.0, which includes branch hinting.
FeatureSet hintingFeatures()
{
    return FeatureSet::wasm3();
}

/// The module @p bytes decode to, which must also validate; fails the test when either does not.
Module loaded(std::span<std::byte const> bytes, FeatureSet const& features = hintingFeatures())
{
    auto module = decodeModule(bytes, features);
    REQUIRE(module.has_value());
    REQUIRE(isoline::validate::validateModule(*module, features).has_value());
    return std::move(*module);
}

/// The codes of @p module's custom section errors.
std::vector<CustomErrorCode> errorCodesOf(Module const& module)
{
    auto codes = std::vector<CustomErrorCode> {};
    for (auto const& error: module.customErrors)
        codes.push_back(error.code);
    return codes;
}

/// Checks that @p payload, as the module's only hint section, is refused with @p code alone, and
/// that the module decodes and validates without any hint.
void checkRefused(std::vector<uint8_t> payload,
                  CustomErrorCode code,
                  HintedImport import = HintedImport::None)
{
    auto const module = loaded(hintedModule({ std::move(payload) }, import));
    CHECK(errorCodesOf(module) == std::vector { code });
    CHECK(module.branchHints.empty());
}

} // namespace

TEST_CASE("BranchHints.decodes")
{
    // func 1: [offset 5, size 1, likely]
    auto const module = loaded(hintedModule({ { 0x01, 0x01, 0x01, 0x05, 0x01, 0x01 } }));
    CHECK(module.customErrors.empty());
    REQUIRE(module.branchHints.size() == 1);
    CHECK(module.branchHints[0].function == 1);
    CHECK(module.branchHints[0].instruction == 1);
    CHECK(module.branchHints[0].likelihood == BranchLikelihood::Likely);
    // The section stays a custom section too.
    REQUIRE(module.customs.size() == 1);
    CHECK(module.customs[0].name == BranchHintSectionName);
}

TEST_CASE("BranchHints.recordsBranchSites")
{
    auto const module = loaded(hintedModule({}));
    CHECK(module.code[0].branchSites.empty());
    auto const& sites = module.code[1].branchSites;
    REQUIRE(sites.size() == 2);
    CHECK(sites[0].instruction == 1);
    CHECK(sites[0].offset == 5);
    CHECK(sites[1].instruction == 3);
    CHECK(sites[1].offset == 9);
}

TEST_CASE("BranchHints.decodesEveryHintOfEveryFunction")
{
    // func 2 (after an import): [5 unlikely, 9 likely]
    auto const module = loaded(
        hintedModule({ { 0x01, 0x02, 0x02, 0x05, 0x01, 0x00, 0x09, 0x01, 0x01 } }, HintedImport::Function));
    CHECK(module.customErrors.empty());
    REQUIRE(module.branchHints.size() == 2);
    CHECK(module.branchHints[0].function == 2);
    CHECK(module.branchHints[0].instruction == 1);
    CHECK(module.branchHints[0].likelihood == BranchLikelihood::Unlikely);
    CHECK(module.branchHints[1].function == 2);
    CHECK(module.branchHints[1].instruction == 3);
    CHECK(module.branchHints[1].likelihood == BranchLikelihood::Likely);

    // A function may list no hints at all.
    auto const empty = loaded(hintedModule({ { 0x02, 0x00, 0x00, 0x01, 0x00 } }));
    CHECK(empty.customErrors.empty());
    CHECK(empty.branchHints.empty());
}

TEST_CASE("BranchHints.invalidTarget")
{
    // The local.get at offset 3, and the middle of it at offset 4.
    checkRefused({ 0x01, 0x01, 0x01, 0x03, 0x01, 0x01 }, CustomErrorCode::BranchHintInvalidTarget);
    checkRefused({ 0x01, 0x01, 0x01, 0x04, 0x01, 0x01 }, CustomErrorCode::BranchHintInvalidTarget);
    // Past the function's end.
    checkRefused({ 0x01, 0x01, 0x01, 0x40, 0x01, 0x01 }, CustomErrorCode::BranchHintInvalidTarget);
    // A function index past the defined functions, and an imported function.
    checkRefused({ 0x01, 0x02, 0x01, 0x05, 0x01, 0x01 }, CustomErrorCode::BranchHintInvalidTarget);
    checkRefused({ 0x01, 0x00, 0x01, 0x05, 0x01, 0x01 },
                 CustomErrorCode::BranchHintInvalidTarget,
                 HintedImport::Function);
    // A valid function's hints are dropped with the later invalid one's.
    checkRefused({ 0x02, 0x01, 0x01, 0x05, 0x01, 0x01, 0x02, 0x01, 0x05, 0x01, 0x01 },
                 CustomErrorCode::BranchHintInvalidTarget);
}

TEST_CASE("BranchHints.outOfOrder")
{
    checkRefused({ 0x01, 0x01, 0x02, 0x09, 0x01, 0x01, 0x05, 0x01, 0x01 },
                 CustomErrorCode::BranchHintOutOfOrder);
    // Offsets and function indices increase strictly: a repeat is out of order too.
    checkRefused({ 0x01, 0x01, 0x02, 0x05, 0x01, 0x01, 0x05, 0x01, 0x01 },
                 CustomErrorCode::BranchHintOutOfOrder);
    checkRefused({ 0x02, 0x01, 0x00, 0x01, 0x00 }, CustomErrorCode::BranchHintOutOfOrder);
    checkRefused({ 0x02, 0x01, 0x00, 0x00, 0x00 }, CustomErrorCode::BranchHintOutOfOrder);
}

TEST_CASE("BranchHints.truncated")
{
    auto const full = std::vector<uint8_t> { 0x01, 0x01, 0x01, 0x05, 0x01, 0x01 };
    for (auto const length: { 0UZ, 1UZ, 2UZ, 3UZ, 4UZ, 5UZ })
        checkRefused({ full.begin(), full.begin() + static_cast<std::ptrdiff_t>(length) },
                     CustomErrorCode::BranchHintMalformed);
}

TEST_CASE("BranchHints.malformed")
{
    // A size other than 1, a hint byte other than 0 or 1, and a byte after the vector.
    checkRefused({ 0x01, 0x01, 0x01, 0x05, 0x02, 0x01, 0x01 }, CustomErrorCode::BranchHintMalformed);
    checkRefused({ 0x01, 0x01, 0x01, 0x05, 0x01, 0x02 }, CustomErrorCode::BranchHintMalformed);
    checkRefused({ 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x00 }, CustomErrorCode::BranchHintMalformed);
    // Counts far beyond the bytes left are refused before anything is read or allocated.
    checkRefused({ 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, CustomErrorCode::BranchHintMalformed);
    checkRefused({ 0x01, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, CustomErrorCode::BranchHintMalformed);
    // An over-long LEB128.
    checkRefused({ 0x01, 0x81, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 }, CustomErrorCode::BranchHintMalformed);
}

TEST_CASE("BranchHints.reportsTheRefusedOffset")
{
    // The hint section is the last section: its payload is the module's last 9 bytes, and the
    // second offset, the refused one, sits 3 bytes before their end.
    auto const bytes = hintedModule({ { 0x01, 0x01, 0x02, 0x09, 0x01, 0x01, 0x05, 0x01, 0x01 } });
    auto const module = loaded(bytes);
    CHECK(module.customErrors
          == std::vector { CustomSectionError { CustomErrorCode::BranchHintOutOfOrder, bytes.size() - 3 } });
}

TEST_CASE("BranchHints.refusesASecondSection")
{
    // The first section's hints are kept; the second is refused at its payload, whatever it holds,
    // and stays a plain custom section.
    auto const bytes =
        hintedModule({ { 0x01, 0x01, 0x01, 0x05, 0x01, 0x01 }, { 0x01, 0x01, 0x01, 0x09, 0x01, 0x01 } });
    auto const module = loaded(bytes);
    CHECK(module.customErrors
          == std::vector {
              CustomSectionError { CustomErrorCode::BranchHintDuplicateSection, bytes.size() - 6 } });
    REQUIRE(module.branchHints.size() == 1);
    CHECK(module.branchHints[0].instruction == 1);
    CHECK(module.customs.size() == 2);
}

TEST_CASE("BranchHints.decodesASectionBeforeTheCode")
{
    // The section's place in real modules: before the code section, whose branch sites it names.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    builder.function(0, { I32 }, { 0x20, 0x00, 0x04, 0x40, 0x20, 0x00, 0x0D, 0x00, 0x0B });
    auto section = encodeName(BranchHintSectionName);
    for (auto const byte: { 0x01, 0x00, 0x02, 0x05, 0x01, 0x01, 0x09, 0x01, 0x00 })
        section.push_back(static_cast<uint8_t>(byte));
    builder.rawSectionBeforeCode(0, std::move(section));
    auto const module = loaded(builder.build());
    CHECK(module.customErrors.empty());
    REQUIRE(module.branchHints.size() == 2);
    CHECK(module.branchHints[0].instruction == 1);
    CHECK(module.branchHints[0].likelihood == BranchLikelihood::Likely);
    CHECK(module.branchHints[1].instruction == 3);
    CHECK(module.branchHints[1].likelihood == BranchLikelihood::Unlikely);
}

TEST_CASE("BranchHints.needsTheProposal")
{
    auto const module =
        loaded(hintedModule({ { 0xFF } }), FeatureSet::wasm3().without(Feature::BranchHinting));
    CHECK(module.customErrors.empty());
    CHECK(module.branchHints.empty());
    CHECK(module.customs.size() == 1);
}

TEST_CASE("BranchHints.decodeAgainstAModule")
{
    // Called directly, an error offset is the payload's offset plus the refused byte's index.
    auto module = Module {};
    module.code.resize(1);
    module.code[0].branchSites.push_back(BranchSite { .instruction = 2, .offset = 7 });
    auto const payload =
        std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x00 }, std::byte { 0x01 },
                                 std::byte { 0x07 }, std::byte { 0x01 }, std::byte { 0x00 } };
    decodeBranchHints(module, payload, 100);
    CHECK(module.customErrors.empty());
    REQUIRE(module.branchHints.size() == 1);
    CHECK(module.branchHints[0].instruction == 2);

    // A second call appends nothing once it fails, and keeps what the first call decoded.
    auto truncated = std::span { payload }.first(4);
    decodeBranchHints(module, truncated, 100);
    CHECK(module.branchHints.size() == 1);
    CHECK(module.customErrors
          == std::vector { CustomSectionError { CustomErrorCode::BranchHintMalformed, 104 } });
}

TEST_CASE("BranchHints.messages")
{
    CHECK(specMessage(CustomErrorCode::BranchHintMalformed)
          == "@metadata.code.branch_hint annotation: malformed");
    CHECK(specMessage(CustomErrorCode::BranchHintOutOfOrder)
          == "@metadata.code.branch_hint annotation: invalid order");
    CHECK(specMessage(CustomErrorCode::BranchHintInvalidTarget)
          == "@metadata.code.branch_hint annotation: invalid target");
    CHECK(specMessage(CustomErrorCode::BranchHintDuplicateSection)
          == "@metadata.code.branch_hint annotation: duplicate section");
}
