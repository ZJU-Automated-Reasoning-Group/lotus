#include "Annotation/ModRef/ExternalModRefTable.h"

#include "Utils/Formats/pcomb/pcomb.h"
#include "Utils/LLVM/IO/ReadFile.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pcomb;

namespace annotation {

/**
 * Finds a mod/ref effect summary for a given function name
 *
 * @param name The name of the function to look up
 * @return A pointer to the summary, or nullptr if not found
 */
const ModRefEffectSummary *
ExternalModRefTable::lookup(const StringRef &name) const {
  auto itr = table.find(name.str());
  if (itr == table.end())
    return nullptr;
  else
    return &itr->second;
}

/**
 * Internal helper that builds the table and reports success via the ok
 * out-parameter.  Declared as a static member so it can access the private
 * `table` field.  Uses an out-parameter instead of a structured binding to
 * remain compatible with C++14.
 *
 * Changes from the original buildTable():
 *  - assert(num < 256) replaced by a runtime range check.
 *  - assert() on duplicate IGNORE entries replaced by a runtime warning.
 *  - std::exit(-1) replaced by setting ok=false and returning.
 *  - Comment regex "#.*\\n" → "#[^\\n]*" (last-line-without-newline fix).
 */
ExternalModRefTable
ExternalModRefTable::buildTableImpl(const StringRef &fileContent, bool &ok) {
  ExternalModRefTable table;
  ok = true;

  // Parse a decimal integer in [0, 255].  On out-of-range input the rule
  // sets ok=false and returns 0 so that parsing can continue and report the
  // position of the bad token.
  auto idx = rule(regex("\\d+"), [&ok](auto const &digits) -> uint8_t {
    auto num = std::stoul(digits.str());
    if (num > 255) {
      llvm::errs() << "[ExternalModRefTable] Error: argument index " << num
                   << " exceeds maximum of 255\n";
      ok = false;
      return 0;
    }
    return static_cast<uint8_t>(num);
  });

  auto id = regex("[\\w\\.]+");

  auto marg = rule(seq(str("Arg"), idx), [](auto const &pair) {
    return APosition::getArgPosition(std::get<1>(pair));
  });

  auto mafterarg = rule(seq(str("AfterArg"), idx), [](auto const &pair) {
    return APosition::getAfterArgPosition(std::get<1>(pair));
  });

  auto mret = rule(str("Ret"),
                   [](auto const &) { return APosition::getReturnPosition(); });

  auto mpos = alt(mret, marg, mafterarg);

  auto modtype = rule(str("MOD"), [](auto const &) { return ModRefType::Mod; });

  auto reftype = rule(str("REF"), [](auto const &) { return ModRefType::Ref; });

  auto mtype = alt(modtype, reftype);

  auto dclass = rule(ch('D'), [](char) { return ModRefClass::DirectMemory; });
  auto rclass =
      rule(ch('R'), [](char) { return ModRefClass::ReachableMemory; });
  auto mclass = alt(dclass, rclass);

  auto regularEntry =
      rule(seq(token(id), token(mtype), token(mpos), token(mclass)),
           [&table](auto const &tuple) {
             auto entry = ModRefEffect(std::get<1>(tuple), std::get<3>(tuple),
                                       std::get<2>(tuple));
             table.table[std::get<0>(tuple).str()].addEffect(std::move(entry));
             return true;
           });

  auto ignoreEntry =
      rule(seq(token(str("IGNORE")), token(id)), [&table](auto const &pair) {
        const std::string name = std::get<1>(pair).str();
        if (table.lookup(name) != nullptr) {
          // Duplicate IGNORE (or IGNORE after a regular entry): warn and skip
          // rather than asserting or silently overwriting.
          llvm::errs() << "[ExternalModRefTable] Warning: duplicate entry for '"
                       << name << "' — second IGNORE ignored\n";
        } else {
          table.table.insert(std::make_pair(name, ModRefEffectSummary()));
        }
        return false;
      });

  // Match a comment: '#' followed by any characters up to (but not including)
  // the newline.  The original "#.*\\n" required a trailing newline, which
  // caused a parse failure on the last line of a file that has no trailing
  // newline.
  auto commentEntry =
      rule(token(regex("#[^\n]*")), [](auto const &) { return false; });

  auto entry = alt(commentEntry, ignoreEntry, regularEntry);
  auto ptable = many(entry);

  auto parseResult = ptable.parse(fileContent);
  if (parseResult.hasError() ||
      !StringRef(parseResult.getInputStream().getRawBuffer()).ltrim().empty()) {
    auto &stream = parseResult.getInputStream();
    llvm::errs() << "[ExternalModRefTable] Error: parsing mod/ref config file "
                    "failed at line "
                 << stream.getLineNumber() << ", column "
                 << stream.getColumnNumber() << "\n";
    ok = false;
  }

  return table;
}

/**
 * Builds a mod/ref effect table from a configuration file's content.
 * Aborts the process on parse error (preserves the original public API).
 */
ExternalModRefTable
ExternalModRefTable::buildTable(const StringRef &fileContent) {
  bool ok = false;
  ExternalModRefTable tbl = buildTableImpl(fileContent, ok);
  if (!ok)
    std::exit(-1);
  return tbl;
}

/**
 * Loads an external mod/ref table from a file.
 *
 * @param fileName The path to the configuration file
 * @return A fully populated ExternalModRefTable
 */
ExternalModRefTable ExternalModRefTable::loadFromFile(const char *fileName) {
  auto memBuf = util::io::readFileIntoBuffer(fileName);
  return buildTable(memBuf->getBuffer());
}

/**
 * Loads an external mod/ref table from a file without calling std::exit().
 * Returns false (and reports the error via llvm::errs()) on parse failure,
 * giving the caller a chance to recover.
 *
 * @param fileName  The path to the configuration file
 * @param outTable  Populated on success
 * @return true on success, false on parse error
 */
bool ExternalModRefTable::loadFromFile(const char *fileName,
                                       ExternalModRefTable &outTable) {
  auto memBuf = util::io::readFileIntoBuffer(fileName);
  bool ok = false;
  ExternalModRefTable tbl = buildTableImpl(memBuf->getBuffer(), ok);
  if (ok)
    outTable = std::move(tbl);
  return ok;
}

} // namespace annotation
