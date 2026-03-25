#include "Annotation/ModRef/ExternalModRefTablePrinter.h"

#include "Annotation/ModRef/ExternalModRefTable.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace annotation {

/**
 * Prints a mod/ref class to the output stream
 *
 * @param os The output stream to write to
 * @param c The class (direct or reachable memory) to print
 *
 * Formats the memory access class in a human-readable form, indicating whether
 * the access is to direct memory or to memory reachable through pointers.
 */
static void printClass(raw_ostream &os, ModRefClass c) {
  switch (c) {
  case ModRefClass::DirectMemory:
    os << "[DIRECT MEMORY]";
    break;
  case ModRefClass::ReachableMemory:
    os << "[REACHABLE MEMORY]";
    break;
  }
}

/**
 * Prints a mod/ref effect to the output stream
 *
 * @param os The output stream to write to
 * @param effect The effect to print
 *
 * Formats a single mod/ref effect, showing the position (return value,
 * argument), and the type of memory access (direct or reachable).
 */
static void printEffect(raw_ostream &os, const ModRefEffect &effect) {
  auto const &pos = effect.getPosition();
  if (pos.isReturnPosition()) {
    os << "    return";
  } else {
    auto const &argPos = pos.getAsArgPosition();
    unsigned idx = argPos.getArgIndex();

    os << "    arg" << idx;
    if (argPos.isAfterArgPosition())
      os << " and after";
  }

  os << ", ";
  printClass(os, effect.getClass());
  os << "\n";
}

/**
 * Prints all MOD effects in a summary to the output stream
 *
 * @param os The output stream to write to
 * @param summary The summary containing effects to print
 *
 * Filters and formats all memory modification effects from the summary.
 */
static void printModEffects(raw_ostream &os,
                            const ModRefEffectSummary &summary) {
  os << "  [MOD]\n";
  for (auto const &effect : summary)
    if (effect.getType() == ModRefType::Mod)
      printEffect(os, effect);
}

/**
 * Prints all REF effects in a summary to the output stream
 *
 * @param os The output stream to write to
 * @param summary The summary containing effects to print
 *
 * Filters and formats all memory reference (read) effects from the summary.
 */
static void printRefEffects(raw_ostream &os,
                            const ModRefEffectSummary &summary) {
  os << "  [REF]\n";
  for (auto const &effect : summary)
    if (effect.getType() == ModRefType::Ref)
      printEffect(os, effect);
}

/**
 * Prints the entire external mod/ref table to the output stream
 *
 * @param table The external mod/ref table to print
 *
 * This method formats and outputs the full contents of the mod/ref table,
 * including each function and its effects. Functions marked as ignored are
 * specially highlighted. The output is color-coded for better readability,
 * with modification effects in magenta and reference effects in yellow.
 */
void ExternalModRefTablePrinter::printTable(const ExternalModRefTable &table) {
  os << "\n----- ExternalModRefTable -----\n";
  for (auto const &mapping : table) {
    os.resetColor();

    os << "Function ";
    os.changeColor(raw_ostream::GREEN);
    os << mapping.first << ":\n";

    if (mapping.second.empty()) {
      os.changeColor(raw_ostream::RED);
      os << "  Ignored\n";
      continue;
    }

    os.changeColor(raw_ostream::MAGENTA);
    printModEffects(os, mapping.second);
    os.changeColor(raw_ostream::YELLOW);
    printRefEffects(os, mapping.second);
  }

  os.resetColor();
  os << "--------- End of Table ---------\n\n";
}

} // namespace annotation