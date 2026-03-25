#pragma once

#include "Alias/Dynamic/LogProcessor.h"

namespace dynamic {

class LogPrinter : public LogProcessor<LogPrinter> {
private:
  std::ostream &os;

public:
  LogPrinter(const char *, std::ostream &);

  void visitAllocRecord(const AllocRecord &);
  void visitPointerRecord(const PointerRecord &);
  void visitEnterRecord(const EnterRecord &);
  void visitExitRecord(const ExitRecord &);
  void visitCallRecord(const CallRecord &);
};

} // namespace dynamic
