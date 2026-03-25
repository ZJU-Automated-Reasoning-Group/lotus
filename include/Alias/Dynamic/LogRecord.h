#pragma once

// We won't put the following structs into a namespace because of C
// compatibility

struct AllocRecord {
  char type;
  unsigned id;
  void *address;
};

struct PointerRecord {
  unsigned id;
  void *address;
};

struct EnterRecord {
  unsigned id;
};

struct ExitRecord {
  unsigned id;
};

struct CallRecord {
  unsigned id;
};

enum LogRecordType { TAllocRec, TPointerRec, TEnterRec, TExitRec, TCallRec };

struct LogRecord {
  enum LogRecordType type;
  union {
    struct AllocRecord allocRecord;
    struct PointerRecord ptrRecord;
    struct EnterRecord enterRecord;
    struct ExitRecord exitRecord;
    struct CallRecord callRecord;
  };
};
