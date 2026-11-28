// SPDX-License-Identifier: Apache-2.0
//
// isoline_encoding_samples: prints every sample of every row of the x86-64 encoding table, in table order,
// as `hex<TAB>intended<TAB>row` lines (compiler/x86_64/EncodingSamples.hpp), for scripts/check-encodings.py.
// Exits 1 if the encoder refuses a sample, naming it on stderr.

#include <cstdio>
#include <print>

#include <isoline/compiler/x86_64/Encoder.hpp>
#include <isoline/compiler/x86_64/EncodingSamples.hpp>

int main()
{
    using namespace isoline::compiler::x86_64;
    auto status = 0;
    for (auto const& row: EncodingTable)
    {
        for (auto const& sample: samplesFor(row.mnemonic))
        {
            auto const line = sampleLine(sample);
            if (!line)
            {
                std::println(
                    stderr, "{}: {} refused: {}", row.name, intendedText(sample), describe(line.error()));
                status = 1;
                continue;
            }
            std::println("{}", *line);
        }
    }
    return status;
}
