#pragma once

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace annotation {

class ExternalModRefTable;

class ExternalModRefTablePrinter {
private:
  llvm::raw_ostream &os;

public:
  ExternalModRefTablePrinter(llvm::raw_ostream &o) : os(o) {}

  void printTable(const ExternalModRefTable &);
};

} // namespace annotation
