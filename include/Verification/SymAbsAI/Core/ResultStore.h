/**
 * @file ResultStore.h
 * @brief Persistent storage for abstract analysis results using BerkeleyDB.
 *
 * This header defines the ResultStore class which provides persistent storage
 * for abstract values associated with program locations. It uses BerkeleyDB for
 * key-value storage and Cereal for serialization of abstract domains.
 *
 * Key features:
 * - Persistent abstract values across analysis runs
 * - Stores results indexed by program location (function, basic block)
 * - Supports serialization/deserialization of AbstractValue objects
 * - Enables dynamic analysis and result caching
 *
 * The store is typically used to:
 * - Cache analysis results for reuse
 * - Enable incremental/dynamic analysis
 * - Store computed abstractions for later verification
 *
 * @note Requires ENABLE_DYNAMIC compile flag
 * @see AbstractValue
 * @see FunctionContext
 */
#pragma once

#include "Verification/SymAbsAI/Utils/Utils.h"
#ifdef ENABLE_DYNAMIC

#define CEREAL_FUTURE_EXPERIMENTAL 1
#include <cereal/archives/adapters.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/archives/xml.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp>

extern "C" {
#include <db.h>
}

#include "Verification/SymAbsAI/Core/FunctionContext.h"

#include <llvm/IR/Value.h>
#include <llvm/IR/ValueSymbolTable.h>

namespace symabs_ai {
namespace testing {
class SerializationTests;
}

/**
 * @brief A persistent mapping from program locations to abstract values.
 *
 * When constructed with a file name argument, it will open a BerkeleyDB
 * database inside this file or create one if the file doesn't exist. The
 * database stores abstract values that can be accessed using get() and placed
 * using put().
 *
 * Class ResultStore::Key represents a key in the database and is intended to
 * map 1-to-1 to program locations. Internally it uses function and basic block
 * indices which are only unique within the scope of a single module so
 * different modules should never use the same database.
 */
class ResultStore {
  friend class testing::SerializationTests;

private:
  DB *DBP_;
  std::vector<unique_ptr<AbstractValue>> Cache_;

  void initDB(const std::string &file) {
    DBP_ = nullptr;

    int ret = 0;

    // BerkeleyDB hangs if given an empty string as the file name
    if (!(file.size() > 0))
      goto fail;

    ret = db_create(&DBP_, NULL, 0);
    if (!(ret == 0))
      goto fail;

    ret = DBP_->open(DBP_, NULL, file.c_str(), NULL, DB_BTREE, DB_CREATE, 0);
    if (!(ret == 0))
      goto fail;

    return;
  fail:
    panic("Failed to initialized database.");
  }

public:
  /***
   * Represents a key in the key-value store implemented by `ResultStore`.
   */
  class Key {
  private:
    uint32_t ID_;
    DBT DBT_;

    void prepareDBT();

  public:
    Key(uint32_t id);
    Key(const llvm::Function &function, llvm::BasicBlock *location,
        bool sound = false);
    Key(llvm::BasicBlock &, bool sound = false);

    uint32_t getId() const { return ID_; }
    operator DBT *() { return &DBT_; }
  };

  /**
   * Constructs a ResultStore backed with a persistent storage in a given
   * file.
   *
   * If the file doesn't exist, it will be created.
   */
  ResultStore(const std::string &filename);

  ResultStore &operator=(ResultStore &&other);
  ResultStore(ResultStore &&other);

  ResultStore &operator=(const ResultStore &other) = delete;
  ResultStore(const ResultStore &other) = delete;

  /**
   * Writes a binary representation of the given abstract value to a stream.
   *
   * Will fail at runtime if the abstract domain doesn't support
   * serialization.
   */
  void serialize(const AbstractValue &avalue, std::ostream &out);

  /**
   * Reads a binary representation of an abstract value from a stream.
   *
   * The `fctx` argument will be passed to the abstract value during
   * its reconstruction and must be compatible with a FunctionContext object
   *used
   * originally.
   */
  unique_ptr<AbstractValue> deserialize(std::istream &in,
                                        const FunctionContext &fctx);

  /**
   * Returns the abstract value stored under a given key. Will return a null
   * pointer if no value with this key is present.
   */
  unique_ptr<AbstractValue> get(const Key &key, const FunctionContext &fctx);

  /**
   * Stores a given abstract value under a specified key.
   */
  void put(const Key &key, const AbstractValue &avalue);

  /**
   * A wrapper class to support customized serialization of llvm::Value using
   * the Cereal serialization library.
   */
  class ValueWrapper {
  private:
    llvm::Value *value;

  public:
    ValueWrapper() : value(nullptr) {}
    ValueWrapper(llvm::Value *x) : value(x) {}

    operator llvm::Value *() { return value; }

    template <class Archive> void save(Archive &archive) const {
      if (value == nullptr) {
        archive(std::string(""));
      } else if (llvm::isa<llvm::GlobalVariable>(value)) {
        archive("@" + value->getName().str());
      } else {
        archive("%" + value->getName().str());
      }
    }

    template <class Archive> void load(Archive &archive) {
      auto &fctx = cereal::get_user_data<FunctionContext>(archive);
      auto *func = fctx.getFunction();

      std::string value_code;
      archive(value_code);

      if (value_code.size() != 0) {
        std::string value_name = value_code.substr(1);

        if (value_code[0] == '%') {
          value = func->getValueSymbolTable().lookup(value_name);
          assert(value != nullptr);
        } else if (value_code[0] == '@') {
          value = func->getParent()->getGlobalVariable(value_name, true);
          assert(value != nullptr);
        } else {
          assert(false && "malformed input");
        }
      }
    }
  };

  ~ResultStore();
};
} // namespace symabs_ai
#else
#include "Verification/SymAbsAI/Core/FunctionContext.h"

namespace symabs_ai {
class ResultStore {
private:
  [[noreturn]] static void fail() {
    llvm_unreachable("serialization and dynamic analysis not available");
  }

public:
  class Key {
  public:
    Key(uint32_t) { fail(); }
    Key(const llvm::Function &, llvm::BasicBlock *) { fail(); }
    Key(llvm::BasicBlock &) { fail(); }
  };

  ResultStore(const std::string &filename) { fail(); }

  ResultStore &operator=(ResultStore &&other) = default;
  ResultStore(ResultStore &&other) = default;

  ResultStore &operator=(const ResultStore &other) = delete;
  ResultStore(const ResultStore &other) = delete;

  void serialize(const AbstractValue &avalue, std::ostream &out) { fail(); }
  unique_ptr<AbstractValue> deserialize(std::istream &, FunctionContext *) {
    fail();
  }
  unique_ptr<AbstractValue> get(const Key &, FunctionContext *) { fail(); }
  void put(const Key &key, const AbstractValue &avalue) { fail(); }
};
} // namespace symabs_ai
#endif /* ENABLE_DYNAMIC */
