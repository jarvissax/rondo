// Text folding for search and sorting, as today's Rondo does it (LocalLibrary.fold(), the dashboard's
// fold()): lower case, accents removed (NFD without combining marks), ı -> i and ə -> e. Covers Latin,
// Greek and Cyrillic; other scripts pass through unchanged.
#pragma once
#include <string>
#include <string_view>

namespace rondo {

std::string fold(std::string_view utf8);   // for matching
std::string lower(std::string_view utf8);  // for sorting (String.lowercase())
std::string upper(std::string_view utf8);  // for headers (String.uppercase(): ı -> I, ə -> Ə, ß -> SS)
// Char.uppercaseChar() of one code point, and whether it is a letter (Char.isLetter(), for the scripts
// titles come in): a list's Next/Prev letter jumps
uint32_t upperChar(uint32_t cp);
bool isLetter(uint32_t cp);

}  // namespace rondo
