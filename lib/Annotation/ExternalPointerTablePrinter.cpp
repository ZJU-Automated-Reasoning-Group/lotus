#include "Annotation/Pointer/ExternalPointerTablePrinter.h"

#include "Annotation/Pointer/ExternalPointerTable.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace annotation {

/**
 * Prints an annotation position to the output stream
 *
 * @param os The output stream to write to
 * @param pos The position to print
 *
 * Formats the position either as "(return)" for return positions or as
 * "(arg N)" for argument positions, where N is the argument index.
 */
static void printPosition(raw_ostream &os, const APosition &pos) {
  if (pos.isReturnPosition())
    os << "(return)";
  else {
    auto const &argPos = pos.getAsArgPosition();
    os << "(arg " << static_cast<unsigned>(argPos.getArgIndex()) << ")";
  }
}

/**
 * Prints a memory allocation effect to the output stream
 *
 * @param os The output stream to write to
 * @param allocEffect The allocation effect to print
 *
 * Formats allocation effects showing whether the size is known (and from which
 * position) or unknown.
 */
static void printAllocEffect(raw_ostream &os,
                             const PointerAllocEffect &allocEffect) {
  os << "  Memory allocation with ";
  if (allocEffect.hasSizePosition()) {
    os << "size given by ";
    printPosition(os, allocEffect.getSizePosition());
  } else {
    os << "unknown size";
  }
}

/**
 * Prints a copy source to the output stream
 *
 * @param os The output stream to write to
 * @param src The copy source to print
 *
 * Formats the copy source based on its type:
 * - Value: The position directly
 * - DirectMemory: '*' followed by the position
 * - ReachableMemory: '*[position + x]'
 * - Special sources like null, universal, or static pointers
 */
static void printCopySource(raw_ostream &os, const CopySource &src) {
  switch (src.getType()) {
  case CopySource::SourceType::Value:
    printPosition(os, src.getPosition());
    break;
  case CopySource::SourceType::DirectMemory:
    os << "*";
    printPosition(os, src.getPosition());
    break;
  case CopySource::SourceType::ReachableMemory:
    os << "*[";
    printPosition(os, src.getPosition());
    os << " + x]";
    break;
  case CopySource::SourceType::Null:
    os << "[Null]";
    break;
  case CopySource::SourceType::Universal:
    os << "[Unknown]";
    break;
  case CopySource::SourceType::Static:
    os << "[Static]";
    break;
  }
}

/**
 * Prints a copy destination to the output stream
 *
 * @param os The output stream to write to
 * @param dest The copy destination to print
 *
 * Formats the copy destination based on its type:
 * - Value: The position directly
 * - DirectMemory: '*' followed by the position
 * - ReachableMemory: '*[position + x]'
 */
static void printCopyDest(raw_ostream &os, const CopyDest &dest) {
  switch (dest.getType()) {
  case CopyDest::DestType::Value:
    printPosition(os, dest.getPosition());
    break;
  case CopyDest::DestType::DirectMemory:
    os << "*";
    printPosition(os, dest.getPosition());
    break;
  case CopyDest::DestType::ReachableMemory:
    os << "*[";
    printPosition(os, dest.getPosition());
    os << " + x]";
    break;
  }
}

/**
 * Prints a pointer copy effect to the output stream
 *
 * @param os The output stream to write to
 * @param copyEffect The copy effect to print
 *
 * Formats the copy effect showing the data flow from source to destination.
 */
static void printCopyEffect(raw_ostream &os,
                            const PointerCopyEffect &copyEffect) {
  os << "  Copy points-to set from ";
  printCopySource(os, copyEffect.getSource());
  os << " to ";
  printCopyDest(os, copyEffect.getDest());
}

/**
 * Prints a pointer effect to the output stream
 *
 * @param os The output stream to write to
 * @param effect The pointer effect to print
 *
 * Dispatches to the appropriate printer based on the effect type.
 */
static void printPointerEffect(raw_ostream &os, const PointerEffect &effect) {
  switch (effect.getType()) {
  case PointerEffectType::Alloc:
    printAllocEffect(os, effect.getAsAllocEffect());
    break;
  case PointerEffectType::Copy:
    printCopyEffect(os, effect.getAsCopyEffect());
    break;
  case PointerEffectType::Exit:
    os << "  Exit";
    break;
  }
  os << "\n";
}

/**
 * Prints the entire external pointer table to the output stream
 *
 * @param table The external pointer table to print
 *
 * This method formats and outputs the full contents of the pointer table,
 * including each function and its effects. Functions marked as ignored are
 * specially highlighted. The output is color-coded for better readability.
 */
void ExternalPointerTablePrinter::printTable(
    const ExternalPointerTable &table) {
  os << "\n----- ExternalPointerTable -----\n";
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

    os.changeColor(raw_ostream::YELLOW);
    for (auto const &effect : mapping.second)
      printPointerEffect(os, effect);
  }

  os.resetColor();
  os << "--------- End of Table ---------\n\n";
}

} // namespace annotation