// SPDX-License-Identifier: Apache-2.0
// The names in a schema file have to be good identifiers in C++ and in GDScript, and the names made from them
// (the column structs, the constants, the functions) must not meet each other. All of that is decided here.
#pragma once

#include "tessera/schema/schema_file.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::schema {

/// The longest name of a type or a property.
inline constexpr size_t kMaxNameLength = 48;

/// Empty when `name` is a good name for a type or a property. Otherwise what is wrong with it, and in `fix`
/// (when given) how to correct it.
std::string CheckName(std::string_view name, std::string* fix = nullptr);

/// Empty when `name` is a good name for the class of the GDScript output: a capital letter, then letters and
/// digits, with at least one lowercase letter, at most `kMaxNameLength` characters, and not a name that Godot
/// already uses (a class, a built-in type, a keyword). Otherwise what is wrong, and in `fix` (when given) how
/// to correct it. A class of the project of the user that has the same name is for Godot to report when it
/// loads.
std::string CheckGdScriptClassName(std::string_view name, std::string* fix = nullptr);

/// The names of the classes, the built-in types and the singletons in the extension API of Godot 4.7.2, and
/// the classes of this extension, that have the shape of a class name. In order of bytes, each one once.
std::span<const std::string_view> GodotNames();

/// The words that cannot be a name, for the tests. C++ keywords, GDScript keywords, and names that headers
/// of Windows or of GCC in its GNU mode turn into macros.
const std::vector<std::string>& CppReservedWords();
const std::vector<std::string>& GdScriptReservedWords();
const std::vector<std::string>& MacroNames();

/// The names the generated C++ uses, made from the names in the file. Each one is made in one place only, so
/// that the generator and the check for collisions cannot disagree.
struct GeneratedNames {
    /// `player_one` gives `PlayerOne`. Different valid names never give the same result.
    static std::string PascalCase(std::string_view name);
    static std::string ColumnsStruct(std::string_view type_name); ///< `PlayerColumns`
    static std::string TypeConstant(std::string_view type_name);  ///< `kPlayerType`
    static std::string AddFunction(std::string_view type_name);   ///< `AddPlayerType`
    static std::string MakeFunction(std::string_view type_name);  ///< `MakePlayerTable`
    /// `kPlayerOwnerMax`, for the suffix `Max`.
    static std::string PropertyConstant(std::string_view type_name, std::string_view property_name,
                                        std::string_view suffix);
    /// The columns of a property: `position_x`, `position_y` and `position_z` for a position, the name itself
    /// for the other kinds.
    static std::vector<std::string> ColumnMembers(const PropertyDecl& property);
    /// The suffixes of the constants a property gets: `Step` and `Range` for a position, `Bits` for an angle
    /// or a mask, `Min` and `Max` for an integer, `Count` for a choice, none for a flag.
    static std::vector<std::string> ConstantSuffixes(Kind kind);
};

/// The names that would be made twice: two names in the same C++ scope that come out the same, whatever the
/// file says. One diagnostic for each place, so that both can be found. Only names that are good by
/// themselves are looked at.
std::vector<Diagnostic> FindCollisions(const SchemaFile& file);

} // namespace tessera::schema
