#include "Annotation/Pointer/ExternalPointerTable.h"

#include "Utils/Formats/pcomb/pcomb.h"
#include "Utils/LLVM/IO/ReadFile.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pcomb;

namespace annotation {

/**
 * Finds a pointer effect summary for a given function name
 *
 * @param name The name of the function to look up
 * @return A pointer to the summary, or nullptr if not found
 */
const PointerEffectSummary *
ExternalPointerTable::lookup(const StringRef &name) const {
  auto itr = table.find(name.str());
  if (itr == table.end())
    return nullptr;
  else
    return &itr->second;
}

/**
 * Builds a pointer effect table from a configuration file's content.
 *
 * Returns a (table, success) pair.  On parse error the error is reported via
 * llvm::errs() and success is set to false.
 *
 * Changes from the original:
 *  - assert(num < 256) replaced by a runtime range check.
 *  - assert() on duplicate IGNORE/DEALLOC entries replaced by a runtime
 *    warning; the duplicate is skipped rather than corrupting the table.
 *  - std::exit(-1) replaced by returning a failure indicator.
 *  - Comment regex changed from "#.*\\n" to "#[^\\n]*" so that a comment on
 *    the last line of a file (with no trailing newline) is still recognised.
 *  - AfterArg added to the copy-destination position rule (ppos) so that
 *    AfterArg<N> can be used as a COPY destination, matching the ModRef parser.
 *
 * Note: this is a static member function so it has access to the private
 * `table` field of ExternalPointerTable.  The bool return value is passed
 * back via an out-parameter to stay compatible with C++14 (no structured
 * bindings).
 */
ExternalPointerTable
ExternalPointerTable::buildTableImpl(const StringRef &fileContent, bool &ok) {
  ExternalPointerTable extTable;
  ok = true;

  // Parse a decimal integer in [0, 255].  On out-of-range input the rule
  // sets ok=false and returns 0 so that parsing can continue.
  auto idx = rule(regex("\\d+"), [&ok](auto const &digits) -> uint8_t {
    auto num = std::stoul(digits.str());
    if (num > 255) {
      llvm::errs() << "[ExternalPointerTable] Error: argument index " << num
                   << " exceeds maximum of 255\n";
      ok = false;
      return 0;
    }
    return static_cast<uint8_t>(num);
  });

  auto id = regex("[\\w\\.]+");

  auto pret = rule(str("Ret"),
                   [](auto const &) { return APosition::getReturnPosition(); });

  auto parg = rule(seq(str("Arg"), idx), [](auto const &pair) {
    return APosition::getArgPosition(std::get<1>(pair));
  });

  // AfterArg<N> as a position — needed for copy destinations such as
  // "COPY AfterArg0 V Arg1 V".  Previously absent, causing a parse failure
  // for any spec line that used AfterArg as a destination.
  auto pafterarg = rule(seq(str("AfterArg"), idx), [](auto const &pair) {
    return APosition::getAfterArgPosition(std::get<1>(pair));
  });

  // ppos is used for copy destinations; include AfterArg so it is symmetric
  // with the ModRef parser.
  auto ppos = alt(parg, pafterarg, pret);

  auto argsrc = rule(
      seq(parg, token(alt(ch('V'), ch('D'), ch('R')))), [](auto const &pair) {
        auto type = std::get<1>(pair);
        switch (type) {
        case 'V':
          return CopySource::getValue(std::get<0>(pair));
        case 'D':
          return CopySource::getDirectMemory(std::get<0>(pair));
        case 'R':
          return CopySource::getReachableMemory(std::get<0>(pair));
        default:
          llvm_unreachable("Only VDR could possibly be here");
        }
      });

  auto nullsrc = rule(
      str("NULL"), [](auto const &) { return CopySource::getNullPointer(); });

  auto unknowsrc = rule(str("UNKNOWN"), [](auto const &) {
    return CopySource::getUniversalPointer();
  });

  auto staticsrc = rule(str("STATIC"), [](auto const &) {
    return CopySource::getStaticPointer();
  });

  auto copysrc = alt(nullsrc, unknowsrc, staticsrc, argsrc);

  auto copydest = rule(
      seq(ppos, token(alt(ch('V'), ch('D'), ch('R')))), [](auto const &pair) {
        auto type = std::get<1>(pair);
        switch (type) {
        case 'V':
          return CopyDest::getValue(std::get<0>(pair));
        case 'D':
          return CopyDest::getDirectMemory(std::get<0>(pair));
        case 'R':
          return CopyDest::getReachableMemory(std::get<0>(pair));
        default:
          llvm_unreachable("Only VDR could possibly be here");
        }
      });

  // Match a comment: '#' followed by any characters up to (but not including)
  // the newline.  The original "#.*\\n" required a trailing newline, which
  // caused a parse failure on the last line of a file without a trailing
  // newline.
  auto commentEntry =
      rule(token(regex("#[^\n]*")), [](auto const &) { return false; });

  auto ignoreEntry =
      rule(seq(token(str("IGNORE")), token(id)), [&extTable](auto const &pair) {
        const std::string name = std::get<1>(pair).str();
        if (extTable.lookup(name) != nullptr) {
          // Duplicate IGNORE: warn and skip rather than asserting.
          llvm::errs()
              << "[ExternalPointerTable] Warning: duplicate entry for '" << name
              << "' — second IGNORE ignored\n";
        } else {
          extTable.table.insert(std::make_pair(name, PointerEffectSummary()));
        }
        return false;
      });

  auto deallocEntry =
      rule(seq(token(str("DEALLOC")), token(id)),
           [&extTable](auto const &pair) {
             // TPA currently does not model deallocation effects; treat as
             // no-op.
             const std::string name = std::get<1>(pair).str();
             if (extTable.lookup(name) != nullptr) {
               // Duplicate DEALLOC: warn and skip rather than asserting.
               llvm::errs()
                   << "[ExternalPointerTable] Warning: duplicate entry for '"
                   << name << "' — second DEALLOC ignored\n";
             } else {
               extTable.table.insert(
                   std::make_pair(name, PointerEffectSummary()));
             }
             return false;
           });

  auto allocWithSize =
      rule(seq(str("ALLOC"), token(parg)), [](auto const &pair) {
        return PointerEffect::getAllocEffect(std::get<1>(pair));
      });

  auto allocWithoutSize = rule(str("ALLOC"), [](auto const &) {
    return PointerEffect::getAllocEffect();
  });

  auto allocEntry =
      rule(seq(token(id), token(alt(allocWithSize, allocWithoutSize))),
           [&extTable](auto &&pair) {
             extTable.table[std::get<0>(pair).str()].addEffect(
                 std::move(std::get<1>(pair)));
             return true;
           });

  auto copyEntry = rule(
      seq(token(id), token(str("COPY")), token(copydest), token(copysrc)),
      [&extTable](auto const &tuple) {
        auto entry = PointerEffect::getCopyEffect(std::get<2>(tuple),
                                                  std::get<3>(tuple));
        extTable.table[std::get<0>(tuple).str()].addEffect(std::move(entry));
        return true;
      });

  auto exitEntry =
      rule(seq(token(id), token(str("EXIT"))), [&extTable](auto const &tuple) {
        auto entry = PointerEffect::getExitEffect();
        extTable.table[std::get<0>(tuple).str()].addEffect(std::move(entry));
        return true;
      });

  auto pentry = alt(commentEntry, ignoreEntry, deallocEntry, allocEntry,
                    copyEntry, exitEntry);
  auto ptable = many(pentry);

  auto parseResult = ptable.parse(fileContent);
  if (parseResult.hasError() ||
      !StringRef(parseResult.getInputStream().getRawBuffer()).ltrim().empty()) {
    auto &stream = parseResult.getInputStream();
    llvm::errs() << "[ExternalPointerTable] Error: parsing pointer config file "
                    "failed at line "
                 << stream.getLineNumber() << ", column "
                 << stream.getColumnNumber() << "\n";
    ok = false;
  }

  return extTable;
}

/**
 * Builds a pointer effect table from a configuration file's content.
 * Aborts the process on parse error (preserves the original public API).
 */
ExternalPointerTable
ExternalPointerTable::buildTable(const StringRef &fileContent) {
  bool ok = false;
  ExternalPointerTable tbl = buildTableImpl(fileContent, ok);
  if (!ok)
    std::exit(-1);
  return tbl;
}

/**
 * Loads an external pointer table from a file.
 *
 * @param fileName The path to the configuration file
 * @return A fully populated ExternalPointerTable
 */
ExternalPointerTable ExternalPointerTable::loadFromFile(const char *fileName) {
  auto memBuf = util::io::readFileIntoBuffer(fileName);
  return buildTable(memBuf->getBuffer());
}

/**
 * Loads an external pointer table from a file without calling std::exit().
 * Returns false (and reports the error via llvm::errs()) on parse failure,
 * giving the caller a chance to recover.
 *
 * @param fileName  The path to the configuration file
 * @param outTable  Populated on success
 * @return true on success, false on parse error
 */
bool ExternalPointerTable::loadFromFile(const char *fileName,
                                        ExternalPointerTable &outTable) {
  auto memBuf = util::io::readFileIntoBuffer(fileName);
  bool ok = false;
  ExternalPointerTable tbl = buildTableImpl(memBuf->getBuffer(), ok);
  if (ok)
    outTable = std::move(tbl);
  return ok;
}

/**
 * Adds a pointer effect to the table for a specific function.
 * This method is primarily used for testing purposes.
 */
void ExternalPointerTable::addEffect(const llvm::StringRef &name,
                                     PointerEffect &&e) {
  table[name.str()].addEffect(std::move(e));
}

} // namespace annotation
