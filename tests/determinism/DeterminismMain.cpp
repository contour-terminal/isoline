// SPDX-License-Identifier: Apache-2.0
// Prints the digest of each module's compiled code (compiler::testing::digestOf()), for every execution mode
// and host ABI, one `file<TAB>mode<TAB>abi<TAB>digest` line each: what the two-process determinism check runs
// twice and compares (CompareRuns.cmake). A second process has another heap and another address-space layout,
// so an ordering that leaks a pointer into the code shows as a different digest.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/Compiler.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/testing/CodeDigest.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace
{

using isoline::compiler::machine::OperatingSystemAbi;
using isoline::runtime::ExecutionMode;

/// The digests of the module in @p path, printed; false when it does not load or compile.
[[nodiscard]] bool printDigests(std::string const& path)
{
    auto file = std::ifstream { path, std::ios::binary };
    auto const text =
        std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    auto const bytes = isoline::text::compileText(text, isoline::text::TextLimits {});
    if (!bytes)
        return false;
    auto const module = isoline::validate::ValidatedModule::load(
        *bytes, isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
    if (!module)
        return false;
    for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
        for (auto const abi: { OperatingSystemAbi::SystemV, OperatingSystemAbi::Windows })
        {
            auto const config =
                isoline::compiler::CompileConfig { isoline::compiler::machine::TargetDescription {
                                                       .architecture =
                                                           isoline::compiler::machine::Architecture::X64,
                                                       .abi = abi,
                                                       .features = {} },
                                                   mode };
            auto const target = isoline::compiler::x86_64::X64Target::create(config.target);
            if (!target)
                return false;
            auto arena =
                isoline::runtime::CountingArena { uint64_t { 1 } << 32, *std::pmr::new_delete_resource() };
            auto const cancellation = isoline::compiler::CompileCancellation {};
            auto const compiled =
                isoline::compiler::Compiler { config, *target, arena, cancellation }.compile(**module);
            if (!compiled)
                return false;
            std::println("{}\t{}\t{}\t{}",
                         path,
                         mode == ExecutionMode::Production ? "production" : "deterministic",
                         abi == OperatingSystemAbi::SystemV ? "systemv" : "windows",
                         isoline::compiler::testing::hexOf(isoline::compiler::testing::digestOf(*compiled)));
        }
    return true;
}

} // namespace

int main(int argc, char const* argv[])
{
    for (auto const* argument: std::span { argv, static_cast<std::size_t>(argc) }.subspan(1))
        if (!printDigests(argument))
        {
            std::println(stderr, "{}: does not load or compile", argument);
            return 1;
        }
    return 0;
}
