#pragma once

#include "Alias/Dynamic/LogRecord.h"

namespace dynamic {

template <typename SubClass, typename RetType = void> class LogConstVisitor {
public:
  LogConstVisitor() = default;

  RetType visit(const LogRecord &rec) {
    switch (rec.type) {
    case LogRecordType::TAllocRec:
      return static_cast<SubClass *>(this)->visitAllocRecord(rec.allocRecord);
    case LogRecordType::TPointerRec:
      return static_cast<SubClass *>(this)->visitPointerRecord(rec.ptrRecord);
    case LogRecordType::TEnterRec:
      return static_cast<SubClass *>(this)->visitEnterRecord(rec.enterRecord);
    case LogRecordType::TExitRec:
      return static_cast<SubClass *>(this)->visitExitRecord(rec.exitRecord);
    case LogRecordType::TCallRec:
      return static_cast<SubClass *>(this)->visitCallRecord(rec.callRecord);
    default:
      std::abort();
    }
  }
};

} // namespace dynamic