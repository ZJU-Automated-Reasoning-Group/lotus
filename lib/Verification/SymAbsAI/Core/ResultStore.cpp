/**
 * @file ResultStore.cpp
 * @brief Implementation of ResultStore for persisting and retrieving abstract
 * analysis results.
 *
 * ResultStore provides a database-backed storage mechanism for abstract
 * interpretation results. It allows storing and retrieving AbstractValue
 * instances associated with specific function/location keys, enabling reuse of
 * analysis results across runs or integration with dynamic analysis results.
 * Uses Berkeley DB when ENABLE_DYNAMIC is defined.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Core/ResultStore.h"

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/repr.h"

#include <sstream>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#ifdef ENABLE_DYNAMIC
using namespace cereal;

#define ID_WIDTH 32

namespace symabs_ai {
/**
 * @brief Prepare the Berkeley DB DBT structure for key operations.
 *
 * Initializes the DBT (database tuple) structure used by Berkeley DB
 * to point to the key's ID data.
 */
void ResultStore::Key::prepareDBT() {
  memset(&DBT_, 0, sizeof(DBT));
  DBT_.data = &ID_;
  DBT_.size = sizeof(ID_);
}

/**
 * @brief Construct a ResultStore::Key from a raw ID.
 *
 * @param id The raw identifier (must not exceed ID_WIDTH-1 bits)
 */
ResultStore::Key::Key(uint32_t id) : ID_(id) {
  assert(!(id & (1 << (ID_WIDTH - 1))) && "ID range exceeded!");
  prepareDBT();
}

/**
 * @brief Construct a ResultStore::Key from a function and location.
 *
 * Computes a unique ID by:
 * 1. Counting all basic blocks in functions before the target function
 * 2. Adding 1 for Fragment::EXIT slot
 * 3. Counting basic blocks in the target function up to the location
 * 4. Setting the high bit if sound=true
 *
 * @param function The function containing the location
 * @param location The basic block location (nullptr for Fragment::EXIT)
 * @param sound Whether this is a sound (true) or unsound (false) result
 */
ResultStore::Key::Key(const llvm::Function &function,
                      llvm::BasicBlock *location, bool sound) {
  const llvm::Module *module = function.getParent();

  ID_ = 0;
  for (auto &f : *module) {
    if (&f == &function)
      break;
    ID_ += f.size() + 1;
  }

  if (location != nullptr) {
    ID_ += 1; // slot 0 is reserved for Fragment::EXIT
    for (auto &bb : function) {
      if (&bb == location)
        break;
      ID_++;
    }
  }

  assert(!(ID_ & (1 << (ID_WIDTH - 1))) && "ID range exceeded!");

  ID_ |= (sound << (ID_WIDTH - 1));

  prepareDBT();
}

ResultStore::Key::Key(llvm::BasicBlock &bb, bool sound)
    : Key(*bb.getParent(), &bb, sound) {}

ResultStore::ResultStore(const std::string &filename) { initDB(filename); }

ResultStore::ResultStore(ResultStore &&other) { *this = std::move(other); }

ResultStore &ResultStore::operator=(ResultStore &&other) {
  DBP_ = other.DBP_;
  Cache_ = std::move(other.Cache_);
  return *this;
}

void ResultStore::serialize(const AbstractValue &avalue, std::ostream &out) {
  UserDataAdapter<ResultStore, BinaryOutputArchive> archive(*this, out);
  unique_ptr<AbstractValue> avalue_copy(avalue.clone());
  archive(avalue_copy);
}

unique_ptr<AbstractValue>
ResultStore::deserialize(std::istream &in, const FunctionContext &fctx) {
  UserDataAdapter<FunctionContext, BinaryInputArchive> archive(
      const_cast<FunctionContext &>(fctx), in);

  unique_ptr<AbstractValue> result;
  archive(result);
  return result;
}

unique_ptr<AbstractValue> ResultStore::get(const Key &key_arg,
                                           const FunctionContext &fctx) {
  Key key = key_arg; // copy since DBP_->get() needs a non-const pointer

  DBT value;
  memset(&value, 0, sizeof(DBT));
  value.flags = DB_DBT_MALLOC;
  int ret = DBP_->get(DBP_, nullptr, key, &value, 0);

  if (ret == DB_NOTFOUND)
    return nullptr;

  std::istringstream in_stream(std::string((char *)value.data, value.size));
  free(value.data);
  return deserialize(in_stream, fctx);
}

void ResultStore::put(const Key &key_arg, const AbstractValue &avalue) {
  Key key = key_arg; // copy since DBP_->put needs a non-const pointer

  DBT value;
  std::ostringstream out;
  serialize(avalue, out);
  std::string str_value = out.str();
  memset(&value, 0, sizeof(DBT));
  value.data = (void *)str_value.c_str();
  value.size = str_value.length();
  int ret = DBP_->put(DBP_, nullptr, key, &value, 0);
  assert(ret == 0);
}

ResultStore::~ResultStore() { DBP_->close(DBP_, 0); }
} // namespace symabs_ai
#endif /* ENABLE_DYNAMIC */
