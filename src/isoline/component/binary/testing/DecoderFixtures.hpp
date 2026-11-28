// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string_view>
#include <vector>

#include <isoline/binary/testing/ModuleBuilder.hpp>

namespace isoline::component::binary::testing
{

/// An encoded component, or a part of one.
using Bytes = std::vector<std::byte>;

/// Appends raw bytes.
/// @param out The bytes to extend.
/// @param bytes The bytes to append, as written.
inline void appendBytes(Bytes& out, std::initializer_list<uint8_t> bytes)
{
    out.reserve(out.size() + bytes.size());
    for (auto const byte: bytes)
        out.push_back(std::byte { byte });
}

/// Appends encoded bytes.
/// @param out The bytes to extend.
/// @param bytes The bytes to append.
inline void appendBytes(Bytes& out, Bytes const& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Appends bytes the core module builder's encoders made.
/// @param out The bytes to extend.
/// @param bytes The bytes to append.
inline void appendBytes(Bytes& out, std::vector<uint8_t> const& bytes)
{
    out.reserve(out.size() + bytes.size());
    for (auto const byte: bytes)
        out.push_back(std::byte { byte });
}

/// Appends the shortest unsigned LEB128 encoding of @p value, as the core builder encodes it.
/// @param out The bytes to extend.
/// @param value The value to encode.
inline void appendLeb128(Bytes& out, uint64_t value)
{
    appendBytes(out, isoline::binary::testing::encodeU64(value));
}

/// Appends a name, its byte length and then its bytes, as the core builder encodes it.
/// @param out The bytes to extend.
/// @param name The name to encode.
inline void appendName(Bytes& out, std::string_view name)
{
    appendBytes(out, isoline::binary::testing::encodeName(name));
}

/// A plain name (`nameattributes` of form 0x00) of @p name.
/// @param name The name to encode.
/// @return Its bytes.
[[nodiscard]] inline Bytes plainName(std::string_view name)
{
    auto bytes = Bytes { std::byte { 0x00 } };
    appendName(bytes, name);
    return bytes;
}

/// A component's preamble: the magic number, version 0x0d and layer 1.
/// @return Its eight bytes.
[[nodiscard]] inline Bytes componentPreamble()
{
    auto bytes = Bytes {};
    appendBytes(bytes, { 0x00, 0x61, 0x73, 0x6D, 0x0D, 0x00, 0x01, 0x00 });
    return bytes;
}

/// A core module's preamble, which is also the smallest core module: the magic number and version 1.
/// @return Its eight bytes.
[[nodiscard]] inline Bytes modulePreamble()
{
    auto bytes = Bytes {};
    appendBytes(bytes, { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 });
    return bytes;
}

/// A section: its id, its size and its payload.
/// @param id The section id.
/// @param payload The section's contents.
/// @return Its bytes.
[[nodiscard]] inline Bytes section(uint8_t id, Bytes const& payload)
{
    auto bytes = Bytes { std::byte { id } };
    appendLeb128(bytes, payload.size());
    appendBytes(bytes, payload);
    return bytes;
}

/// A component of @p sections, after its preamble.
/// @param sections The encoded sections, in order.
/// @return Its bytes.
[[nodiscard]] inline Bytes componentOf(std::initializer_list<Bytes> sections)
{
    auto bytes = componentPreamble();
    for (auto const& part: sections)
        appendBytes(bytes, part);
    return bytes;
}

/// A core module whose one custom section, named @p tag, tells it apart from other modules.
/// @param tag The custom section's name.
/// @return Its bytes.
[[nodiscard]] inline Bytes taggedModule(std::string_view tag)
{
    // The custom section is appended in place: GCC 14 at -O3 reads the destruction of a temporary section
    // here as a delete of a pointer into the middle of an allocation (-Wfree-nonheap-object).
    auto name = Bytes {};
    appendName(name, tag);
    auto module = modulePreamble();
    module.push_back(std::byte { 0 });
    appendLeb128(module, name.size());
    appendBytes(module, name);
    return module;
}

/// @p depth components, each but the innermost holding the next in its one component section.
///
/// Built front to back from the sizes, computed innermost first, so the bytes are made in time and
/// memory linear in @p depth.
/// @param depth The number of components, the outermost counted; at least one.
/// @return Their bytes.
[[nodiscard]] inline Bytes nestedComponents(uint32_t depth)
{
    constexpr auto PreambleSize = uint64_t { 8 };
    // sizes[k] is the encoded size of the component at depth k, the outermost at 0.
    auto sizes = std::vector<uint64_t>(depth, PreambleSize);
    for (auto const level: std::views::iota(uint32_t { 1 }, depth) | std::views::reverse)
        sizes[level - 1] =
            PreambleSize + 1 + isoline::binary::testing::encodeU64(sizes[level]).size() + sizes[level];
    auto bytes = Bytes {};
    bytes.reserve(sizes.front());
    for (auto const level: std::views::iota(uint32_t { 1 }, depth))
    {
        appendBytes(bytes, componentPreamble());
        bytes.push_back(std::byte { 0x04 });
        appendLeb128(bytes, sizes[level]);
    }
    appendBytes(bytes, componentPreamble());
    return bytes;
}

/// The core instance section of the fixtures: an instantiation of module 0 with one argument, and
/// a core instance of one inline export.
/// @return The section's bytes.
[[nodiscard]] inline Bytes coreInstanceSection()
{
    auto payload = Bytes {};
    appendBytes(payload, { 0x02, 0x00, 0x00, 0x01 }); // 2 instances; instantiate module 0, 1 argument
    appendName(payload, "m");
    appendBytes(payload, { 0x12, 0x00, 0x01, 0x01 }); // (instance 0); inline exports, 1 export
    appendName(payload, "g");
    appendBytes(payload, { 0x03, 0x02 }); // (global 2)
    return section(2, payload);
}

/// The instance section of the fixtures: an instantiation of component 0 with one argument, and an
/// instance of one inline export.
/// @return The section's bytes.
[[nodiscard]] inline Bytes instanceSection()
{
    auto payload = Bytes {};
    appendBytes(payload, { 0x02, 0x00, 0x00, 0x01 }); // 2 instances; instantiate component 0, 1 argument
    appendName(payload, "a");
    appendBytes(payload, { 0x01, 0x00, 0x01, 0x01 }); // (func 0); inline exports, 1 export
    appendBytes(payload, plainName("x"));
    appendBytes(payload, { 0x00, 0x11, 0x03 }); // (core module 3)
    return section(5, payload);
}

/// The alias section of the fixtures: an export, a core export and an outer alias.
/// @return The section's bytes.
[[nodiscard]] inline Bytes aliasSection()
{
    auto payload = Bytes {};
    appendBytes(payload, { 0x03, 0x01, 0x00, 0x00 }); // 3 aliases; func, export of instance 0
    appendName(payload, "f");
    appendBytes(payload, { 0x00, 0x01, 0x01, 0x04 }); // core table, core export of core instance 4
    appendName(payload, "t");
    appendBytes(payload, { 0x03, 0x02, 0x01, 0x07 }); // type, outer 1 7
    return section(6, payload);
}

/// The import section of the fixtures: a function import "a" of type 0, and a resource import.
/// @return The section's bytes.
[[nodiscard]] inline Bytes importSection()
{
    auto payload = Bytes { std::byte { 0x02 } };
    appendBytes(payload, plainName("a"));
    appendBytes(payload, { 0x01, 0x00 }); // (func (type 0))
    appendBytes(payload, plainName("r"));
    appendBytes(payload, { 0x03, 0x01 }); // (type (sub resource))
    return section(10, payload);
}

/// The export section of the fixtures: one export without a type, one with.
/// @return The section's bytes.
[[nodiscard]] inline Bytes exportSection()
{
    auto payload = Bytes { std::byte { 0x02 } };
    appendBytes(payload, plainName("b"));
    appendBytes(payload, { 0x01, 0x00, 0x00 }); // func 0, no type
    appendBytes(payload, plainName("c"));
    appendBytes(payload, { 0x05, 0x01, 0x01, 0x03, 0x00, 0x02 }); // instance 1, (type (eq 2))
    return section(11, payload);
}

/// The type section of the fixtures: every defvaltype P11 decodes under ComponentFeatureSet::wasi03()
/// (🔧 and 📝 are refused), the function types, a resource type and the scope forms.
/// @return The section's bytes.
[[nodiscard]] inline Bytes typeSection()
{
    auto payload = Bytes { std::byte { 0x15 } }; // 21 types
    appendBytes(payload, { 0x3F, 0x7F, 0x00 });  // (resource (rep i32))
    appendBytes(payload, { 0x73 });              // string
    appendBytes(payload, { 0x72, 0x02 });        // (record (field "a" bool) (field "b" u8))
    appendName(payload, "a");
    appendBytes(payload, { 0x7F });
    appendName(payload, "b");
    appendBytes(payload, { 0x7D });
    appendBytes(payload, { 0x71, 0x02 }); // (variant (case "x" s8) (case "y"))
    appendName(payload, "x");
    appendBytes(payload, { 0x01, 0x7E, 0x00 });
    appendName(payload, "y");
    appendBytes(payload, { 0x00, 0x00 });
    appendBytes(payload, { 0x70, 0x7B });             // (list u16)
    appendBytes(payload, { 0x6F, 0x02, 0x7C, 0x79 }); // (tuple s16 u32)
    appendBytes(payload, { 0x6E, 0x02 });             // (flags "f1" "f2")
    appendName(payload, "f1");
    appendName(payload, "f2");
    appendBytes(payload, { 0x6D, 0x01 }); // (enum "e1")
    appendName(payload, "e1");
    appendBytes(payload, { 0x6B, 0x7A });                   // (option s32)
    appendBytes(payload, { 0x6A, 0x01, 0x76, 0x01, 0x75 }); // (result f32 (error f64))
    appendBytes(payload, { 0x6A, 0x00, 0x00 });             // (result)
    appendBytes(payload, { 0x69, 0x00 });                   // (own 0)
    appendBytes(payload, { 0x68, 0x00 });                   // (borrow 0)
    appendBytes(payload, { 0x66, 0x01, 0x7D });             // (stream u8)
    appendBytes(payload, { 0x65, 0x00 });                   // (future)
    appendBytes(payload, { 0x63, 0x73, 0x79 });             // (map string u32)
    appendBytes(payload, { 0x70, 0x02 });                   // (list 2): a type index
    appendBytes(payload, { 0x40, 0x01 });                   // (func (param "p" bool) (result u32))
    appendName(payload, "p");
    appendBytes(payload, { 0x7F, 0x00, 0x79 });
    appendBytes(payload, { 0x43, 0x00, 0x01, 0x00 }); // (func async)
    appendBytes(payload, { 0x41, 0x03 });             // (component: type, import, export)
    appendBytes(payload, { 0x01, 0x40, 0x00, 0x01, 0x00 });
    appendBytes(payload, { 0x03 });
    appendBytes(payload, plainName("i"));
    appendBytes(payload, { 0x01, 0x00 });
    appendBytes(payload, { 0x04 });
    appendBytes(payload, plainName("e"));
    appendBytes(payload, { 0x01, 0x00 });
    appendBytes(payload, { 0x42, 0x03 }); // (instance: core type, outer alias, export)
    appendBytes(payload, { 0x00, 0x60, 0x00, 0x00 });
    appendBytes(payload, { 0x02, 0x03, 0x02, 0x01, 0x01 });
    appendBytes(payload, { 0x04 });
    appendBytes(payload, plainName("t"));
    appendBytes(payload, { 0x03, 0x00, 0x00 });
    return section(7, payload);
}

/// The core type section of the fixtures: a function type and a module type of every declarator.
/// @return The section's bytes.
[[nodiscard]] inline Bytes coreTypeSection()
{
    auto payload = Bytes {};
    appendBytes(payload, { 0x02, 0x60, 0x01, 0x7F, 0x01, 0x7E }); // 2 types; (func (param i32) (result i64))
    appendBytes(payload, { 0x50, 0x05, 0x01, 0x60, 0x00, 0x00 }); // (module (type (func))
    appendBytes(payload, { 0x00 });                               //   (import "a" "b" (func (type 0)))
    appendName(payload, "a");
    appendName(payload, "b");
    appendBytes(payload, { 0x00, 0x00 });
    appendBytes(payload, { 0x00 }); //   (import "a" "m" (memory 1))
    appendName(payload, "a");
    appendName(payload, "m");
    appendBytes(payload, { 0x02, 0x00, 0x01 });
    appendBytes(payload, { 0x02, 0x10, 0x01, 0x01, 0x00 }); //   (alias outer 1 0 (type))
    appendBytes(payload, { 0x03 });                         //   (export "e" (func (type 0))))
    appendName(payload, "e");
    appendBytes(payload, { 0x00, 0x00 });
    return section(3, payload);
}

/// The canonical section of the fixtures: lift and lower with options, the resource built-ins and an
/// async one.
/// @return The section's bytes.
[[nodiscard]] inline Bytes canonSection()
{
    auto payload = Bytes {};
    appendBytes(payload, { 0x07 }); // 7 canons
    appendBytes(
        payload,
        { 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x04, 0x01, 0x00 }); // lift 0 (memory 0) (realloc 1), type 0
    appendBytes(payload, { 0x01, 0x00, 0x00, 0x01, 0x01 });        // lower 0 utf16
    appendBytes(payload, { 0x02, 0x00 });                          // resource.new 0
    appendBytes(payload, { 0x03, 0x00 });                          // resource.drop 0
    appendBytes(payload, { 0x04, 0x00 });                          // resource.rep 0
    appendBytes(payload, { 0x09, 0x00, 0x79, 0x00 });              // task.return (result u32)
    appendBytes(payload, { 0x0A, 0x7F, 0x00 });                    // context.get i32 0
    return section(8, payload);
}

/// One encoded component the decoder must accept, under ComponentFeatureSet::wasi03().
struct DecoderFixture
{
    std::string_view name; ///< What it exercises.
    Bytes bytes;           ///< Its encoding.
};

/// Every component the decoder tests accept, for the tests that must cover each form (the encoder's
/// round trip).
/// @return The fixtures.
[[nodiscard]] inline std::vector<DecoderFixture> decoderFixtures()
{
    auto custom = Bytes {};
    appendName(custom, "hi");
    appendBytes(custom, { 0xFF, 0xFE });

    auto attributed = Bytes { std::byte { 0x01 } }; // 1 import
    appendBytes(attributed, { 0x02 });
    appendName(attributed, "x");
    appendBytes(attributed, { 0x02, 0x00 }); // 2 attributes; implements
    appendName(attributed, "ns:p/i@1.0.0");
    appendBytes(attributed, { 0x02 }); // external-id
    appendName(attributed, "x");
    appendBytes(attributed, { 0x05, 0x00 }); // (instance (type 0))

    return {
        { .name = "empty", .bytes = componentPreamble() },
        { .name = "custom", .bytes = componentOf({ section(0, custom) }) },
        { .name = "coreModule", .bytes = componentOf({ section(1, modulePreamble()) }) },
        { .name = "coreInstances", .bytes = componentOf({ coreInstanceSection() }) },
        { .name = "instances", .bytes = componentOf({ instanceSection() }) },
        { .name = "aliases", .bytes = componentOf({ aliasSection() }) },
        { .name = "importsExports", .bytes = componentOf({ importSection(), exportSection() }) },
        { .name = "nameAttributes", .bytes = componentOf({ section(10, attributed) }) },
        { .name = "nested",
          .bytes = componentOf({ section(1, taggedModule("a")),
                                 section(4, componentOf({ section(1, taggedModule("b")) })),
                                 section(1, taggedModule("c")) }) },
        { .name = "deep", .bytes = nestedComponents(100) },
        { .name = "types", .bytes = componentOf({ typeSection() }) },
        { .name = "coreTypes", .bytes = componentOf({ coreTypeSection() }) },
        { .name = "canon", .bytes = componentOf({ canonSection() }) },
    };
}

} // namespace isoline::component::binary::testing
