/**
 * @file reflow_tokenize_entities.c
 * @brief XHTML 1.0 named character reference table for the reflow tokenizer.
 *
 * @details
 * Companion translation unit to reflow_tokenize_lex.c, holding the one
 * lookup the lexer needs and nothing else: the 253 named character
 * references XHTML 1.0 defines (the Latin-1, symbols and special entity
 * sets) plus XML's `apos`, mapped to their Unicode code points. The rows
 * are sorted by name under plain byte order, so the lookup is a bounded
 * binary search (NASA P10 Rule 2) with no recursion (Rule 1) and no
 * allocation (Rule 3). Matching is exact and case-sensitive: `&Eacute;`
 * and `&eacute;` are different references and `&AMP;` is not one, which
 * is what the XML specification requires of XHTML content documents.
 *
 * The name field is a fixed `char[9]` array rather than a pointer, so a
 * name longer than the tokenizer's `&...;` scan window can hold
 * (`k_priv_entity_name_max`) fails to compile instead of sitting in the
 * table as a row no input can ever reach. See ADR-0010.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow.h"
#include "reflow_tokenize_internal.h"

/**
 * @struct priv_entity_row_t
 * @brief One named character reference: its name and the code point it denotes.
 *
 * @details `name` holds the reference without its leading `&` and trailing
 * `;`, NUL-terminated. Its capacity is `k_priv_entity_name_cap`, one byte
 * more than the longest name the scan window admits, so an over-long
 * literal is a compile error rather than an unreachable row.
 *
 * @since 0.1.0
 */
typedef struct {
  char     name[k_priv_entity_name_cap]; /**< Reference name, no '&' or ';'. */
  uint32_t cp;                           /**< Code point the name denotes.   */
} priv_entity_row_t;

/**
 * @brief The XHTML 1.0 named character references, sorted by name.
 *
 * @details Byte-order sorted (uppercase before lowercase), which is the
 * order internal_entity_cmp() compares in and the order the binary search
 * in priv_reflow_tok_lookup_entity() depends on. Asserted by
 * tests/src/test_reflow_entities.c over the whole table.
 */
RA8_INTERNAL
static const priv_entity_row_t k_priv_entities[] = {
  {"AElig", 0x00C6U},   {"Aacute", 0x00C1U},  {"Acirc", 0x00C2U},    {"Agrave", 0x00C0U},
  {"Alpha", 0x0391U},   {"Aring", 0x00C5U},   {"Atilde", 0x00C3U},   {"Auml", 0x00C4U},
  {"Beta", 0x0392U},    {"Ccedil", 0x00C7U},  {"Chi", 0x03A7U},      {"Dagger", 0x2021U},
  {"Delta", 0x0394U},   {"ETH", 0x00D0U},     {"Eacute", 0x00C9U},   {"Ecirc", 0x00CAU},
  {"Egrave", 0x00C8U},  {"Epsilon", 0x0395U}, {"Eta", 0x0397U},      {"Euml", 0x00CBU},
  {"Gamma", 0x0393U},   {"Iacute", 0x00CDU},  {"Icirc", 0x00CEU},    {"Igrave", 0x00CCU},
  {"Iota", 0x0399U},    {"Iuml", 0x00CFU},    {"Kappa", 0x039AU},    {"Lambda", 0x039BU},
  {"Mu", 0x039CU},      {"Ntilde", 0x00D1U},  {"Nu", 0x039DU},       {"OElig", 0x0152U},
  {"Oacute", 0x00D3U},  {"Ocirc", 0x00D4U},   {"Ograve", 0x00D2U},   {"Omega", 0x03A9U},
  {"Omicron", 0x039FU}, {"Oslash", 0x00D8U},  {"Otilde", 0x00D5U},   {"Ouml", 0x00D6U},
  {"Phi", 0x03A6U},     {"Pi", 0x03A0U},      {"Prime", 0x2033U},    {"Psi", 0x03A8U},
  {"Rho", 0x03A1U},     {"Scaron", 0x0160U},  {"Sigma", 0x03A3U},    {"THORN", 0x00DEU},
  {"Tau", 0x03A4U},     {"Theta", 0x0398U},   {"Uacute", 0x00DAU},   {"Ucirc", 0x00DBU},
  {"Ugrave", 0x00D9U},  {"Upsilon", 0x03A5U}, {"Uuml", 0x00DCU},     {"Xi", 0x039EU},
  {"Yacute", 0x00DDU},  {"Yuml", 0x0178U},    {"Zeta", 0x0396U},     {"aacute", 0x00E1U},
  {"acirc", 0x00E2U},   {"acute", 0x00B4U},   {"aelig", 0x00E6U},    {"agrave", 0x00E0U},
  {"alefsym", 0x2135U}, {"alpha", 0x03B1U},   {"amp", 0x0026U},      {"and", 0x2227U},
  {"ang", 0x2220U},     {"apos", 0x0027U},    {"aring", 0x00E5U},    {"asymp", 0x2248U},
  {"atilde", 0x00E3U},  {"auml", 0x00E4U},    {"bdquo", 0x201EU},    {"beta", 0x03B2U},
  {"brvbar", 0x00A6U},  {"bull", 0x2022U},    {"cap", 0x2229U},      {"ccedil", 0x00E7U},
  {"cedil", 0x00B8U},   {"cent", 0x00A2U},    {"chi", 0x03C7U},      {"circ", 0x02C6U},
  {"clubs", 0x2663U},   {"cong", 0x2245U},    {"copy", 0x00A9U},     {"crarr", 0x21B5U},
  {"cup", 0x222AU},     {"curren", 0x00A4U},  {"dArr", 0x21D3U},     {"dagger", 0x2020U},
  {"darr", 0x2193U},    {"deg", 0x00B0U},     {"delta", 0x03B4U},    {"diams", 0x2666U},
  {"divide", 0x00F7U},  {"eacute", 0x00E9U},  {"ecirc", 0x00EAU},    {"egrave", 0x00E8U},
  {"empty", 0x2205U},   {"emsp", 0x2003U},    {"ensp", 0x2002U},     {"epsilon", 0x03B5U},
  {"equiv", 0x2261U},   {"eta", 0x03B7U},     {"eth", 0x00F0U},      {"euml", 0x00EBU},
  {"euro", 0x20ACU},    {"exist", 0x2203U},   {"fnof", 0x0192U},     {"forall", 0x2200U},
  {"frac12", 0x00BDU},  {"frac14", 0x00BCU},  {"frac34", 0x00BEU},   {"frasl", 0x2044U},
  {"gamma", 0x03B3U},   {"ge", 0x2265U},      {"gt", 0x003EU},       {"hArr", 0x21D4U},
  {"harr", 0x2194U},    {"hearts", 0x2665U},  {"hellip", 0x2026U},   {"iacute", 0x00EDU},
  {"icirc", 0x00EEU},   {"iexcl", 0x00A1U},   {"igrave", 0x00ECU},   {"image", 0x2111U},
  {"infin", 0x221EU},   {"int", 0x222BU},     {"iota", 0x03B9U},     {"iquest", 0x00BFU},
  {"isin", 0x2208U},    {"iuml", 0x00EFU},    {"kappa", 0x03BAU},    {"lArr", 0x21D0U},
  {"lambda", 0x03BBU},  {"lang", 0x2329U},    {"laquo", 0x00ABU},    {"larr", 0x2190U},
  {"lceil", 0x2308U},   {"ldquo", 0x201CU},   {"le", 0x2264U},       {"lfloor", 0x230AU},
  {"lowast", 0x2217U},  {"loz", 0x25CAU},     {"lrm", 0x200EU},      {"lsaquo", 0x2039U},
  {"lsquo", 0x2018U},   {"lt", 0x003CU},      {"macr", 0x00AFU},     {"mdash", 0x2014U},
  {"micro", 0x00B5U},   {"middot", 0x00B7U},  {"minus", 0x2212U},    {"mu", 0x03BCU},
  {"nabla", 0x2207U},   {"nbsp", 0x00A0U},    {"ndash", 0x2013U},    {"ne", 0x2260U},
  {"ni", 0x220BU},      {"not", 0x00ACU},     {"notin", 0x2209U},    {"nsub", 0x2284U},
  {"ntilde", 0x00F1U},  {"nu", 0x03BDU},      {"oacute", 0x00F3U},   {"ocirc", 0x00F4U},
  {"oelig", 0x0153U},   {"ograve", 0x00F2U},  {"oline", 0x203EU},    {"omega", 0x03C9U},
  {"omicron", 0x03BFU}, {"oplus", 0x2295U},   {"or", 0x2228U},       {"ordf", 0x00AAU},
  {"ordm", 0x00BAU},    {"oslash", 0x00F8U},  {"otilde", 0x00F5U},   {"otimes", 0x2297U},
  {"ouml", 0x00F6U},    {"para", 0x00B6U},    {"part", 0x2202U},     {"permil", 0x2030U},
  {"perp", 0x22A5U},    {"phi", 0x03C6U},     {"pi", 0x03C0U},       {"piv", 0x03D6U},
  {"plusmn", 0x00B1U},  {"pound", 0x00A3U},   {"prime", 0x2032U},    {"prod", 0x220FU},
  {"prop", 0x221DU},    {"psi", 0x03C8U},     {"quot", 0x0022U},     {"rArr", 0x21D2U},
  {"radic", 0x221AU},   {"rang", 0x232AU},    {"raquo", 0x00BBU},    {"rarr", 0x2192U},
  {"rceil", 0x2309U},   {"rdquo", 0x201DU},   {"real", 0x211CU},     {"reg", 0x00AEU},
  {"rfloor", 0x230BU},  {"rho", 0x03C1U},     {"rlm", 0x200FU},      {"rsaquo", 0x203AU},
  {"rsquo", 0x2019U},   {"sbquo", 0x201AU},   {"scaron", 0x0161U},   {"sdot", 0x22C5U},
  {"sect", 0x00A7U},    {"shy", 0x00ADU},     {"sigma", 0x03C3U},    {"sigmaf", 0x03C2U},
  {"sim", 0x223CU},     {"spades", 0x2660U},  {"sub", 0x2282U},      {"sube", 0x2286U},
  {"sum", 0x2211U},     {"sup", 0x2283U},     {"sup1", 0x00B9U},     {"sup2", 0x00B2U},
  {"sup3", 0x00B3U},    {"supe", 0x2287U},    {"szlig", 0x00DFU},    {"tau", 0x03C4U},
  {"there4", 0x2234U},  {"theta", 0x03B8U},   {"thetasym", 0x03D1U}, {"thinsp", 0x2009U},
  {"thorn", 0x00FEU},   {"tilde", 0x02DCU},   {"times", 0x00D7U},    {"trade", 0x2122U},
  {"uArr", 0x21D1U},    {"uacute", 0x00FAU},  {"uarr", 0x2191U},     {"ucirc", 0x00FBU},
  {"ugrave", 0x00F9U},  {"uml", 0x00A8U},     {"upsih", 0x03D2U},    {"upsilon", 0x03C5U},
  {"uuml", 0x00FCU},    {"weierp", 0x2118U},  {"xi", 0x03BEU},       {"yacute", 0x00FDU},
  {"yen", 0x00A5U},     {"yuml", 0x00FFU},    {"zeta", 0x03B6U},     {"zwj", 0x200DU},
  {"zwnj", 0x200CU},
};

/**
 * @brief Compare a non-terminated needle against a table row's name.
 *
 * @details Byte comparison over `len` bytes, then a length tie-break, so
 * the ordering matches the table's sort exactly. A needle that is a strict
 * prefix of the row sorts before it; a row that is a strict prefix of the
 * needle sorts before the needle.
 *
 * @param[in] needle Candidate name, not NUL-terminated.
 * @param[in] len    Length of `needle` in bytes.
 * @param[in] row    NUL-terminated table name.
 * @return Negative, zero or positive as `needle` sorts before, equal to or
 * after `row`.
 * @retval 0 The names are byte-identical and the same length.
 * @pre `needle` and `row` are non-null.
 * @pre `len` is the exact byte length of `needle`.
 * @post No state is modified (pure).
 * @post Neither buffer is written.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_entity_cmp(const char* needle, size_t len, const char* row)
{
  for (size_t i = 0U; i < len; ++i) {
    const uint8_t a = (uint8_t)needle[i];
    const uint8_t b = (uint8_t)row[i];
    if (b == (uint8_t)'\0') {
      return 1; /* row ended first: needle sorts after it */
    }
    if (a != b) {
      return (a < b) ? -1 : 1;
    }
  }
  return (row[len] == '\0') ? 0 : -1;
}

size_t priv_reflow_tok_entity_count(void)
{
  return sizeof(k_priv_entities) / sizeof(k_priv_entities[0]);
}

bool priv_reflow_tok_entity_at(size_t idx, const char** out_name, uint32_t* out_cp)
{
  if (idx >= priv_reflow_tok_entity_count()) {
    return false;
  }
  *out_name = k_priv_entities[idx].name;
  *out_cp   = k_priv_entities[idx].cp;
  return true;
}

bool priv_reflow_tok_lookup_entity(const char* name, size_t len, uint32_t* out_cp)
{
  if ((len == 0U) || (len > (size_t)k_priv_entity_name_max)) {
    return false;
  }
  size_t lo = 0U;
  size_t hi = priv_reflow_tok_entity_count();
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) / 2U);
    const int    rel = internal_entity_cmp(name, len, k_priv_entities[mid].name);
    if (rel == 0) {
      *out_cp = k_priv_entities[mid].cp;
      return true;
    }
    if (rel < 0) {
      hi = mid;
    } else {
      lo = mid + 1U;
    }
  }
  return false;
}
