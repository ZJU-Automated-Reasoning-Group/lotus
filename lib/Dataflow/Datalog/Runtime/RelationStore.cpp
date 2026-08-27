#include "Dataflow/Datalog/EngineInternal.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lotus::datalog::internal {

namespace {

std::size_t hashValue(const ColumnType &column, ValueRef value) {
  if (column.hash_value)
    return column.hash_value(value);
  return column.hash(value.materialize());
}

bool equalValues(const ColumnType &column, ValueRef lhs, ValueRef rhs) {
  if (column.equal_value)
    return column.equal_value(lhs, rhs);
  return column.equal(lhs.materialize(), rhs.materialize());
}

class AnyColumnStorage final : public ColumnStorage {
public:
  std::size_t size() const override { return values_.size(); }
  void reserve(std::size_t count) override { values_.reserve(count); }
  void append(std::any value) override { values_.push_back(std::move(value)); }
  void update(std::size_t row, std::any value) override {
    values_.at(row) = std::move(value);
  }
  void truncate(std::size_t count) override {
    values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(count),
                  values_.end());
  }
  ValueRef value(std::size_t row) const override {
    return ValueRef::fromAny(values_.at(row));
  }
  std::any materialize(std::size_t row) const override {
    return values_.at(row);
  }
  std::unique_ptr<ColumnStorage>
  select(const std::vector<std::size_t> &rows) const override {
    auto result = std::make_unique<AnyColumnStorage>();
    result->values_.reserve(rows.size());
    for (std::size_t row : rows)
      result->values_.push_back(values_.at(row));
    return result;
  }
  std::size_t approximateMemoryBytes() const override {
    return values_.capacity() * sizeof(std::any);
  }

private:
  std::vector<std::any> values_;
};

} // namespace

RowView::RowView(const RelationStorage &storage, std::size_t row_id)
    : storage_(&storage), row_id_(row_id) {}

RowView::RowView(const Row &row) : dynamic_(&row) {}

std::size_t RowView::size() const {
  return storage_ ? storage_->definition().columns.size() : dynamic_->size();
}

ValueRef RowView::operator[](std::size_t column) const {
  return storage_ ? storage_->value(row_id_, column)
                  : ValueRef::fromAny(dynamic_->at(column));
}

void combineHash(std::size_t &seed, std::size_t value) {
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::size_t RowHash::operator()(const Row &row) const {
  std::size_t seed = 0;
  for (std::size_t i = 0; i < row.size(); ++i)
    combineHash(seed, columns[i].hash(row[i]));
  return seed;
}

bool RowEqual::operator()(const Row &lhs, const Row &rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!columns[i].equal(lhs[i], rhs[i]))
      return false;
  }
  return true;
}

std::size_t KeyHash::operator()(const Row &row) const {
  std::size_t seed = 0;
  for (std::size_t i = 0; i < row.size(); ++i)
    combineHash(seed, columns[i].hash(row[i]));
  return seed;
}

bool KeyEqual::operator()(const Row &lhs, const Row &rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!columns[i].equal(lhs[i], rhs[i]))
      return false;
  }
  return true;
}

RuntimeIndex::RuntimeIndex(ColumnMask mask,
                           const std::vector<ColumnType> &all_columns) {
  for (std::size_t i = 0; i < all_columns.size(); ++i) {
    if (mask & (ColumnMask{1} << i)) {
      columns_.push_back(i);
      column_types_.push_back(all_columns[i]);
    }
  }
}

std::size_t RuntimeIndex::hash(RowView row) const {
  std::size_t seed = 0;
  for (std::size_t index = 0; index < columns_.size(); ++index)
    combineHash(seed,
                hashValue(column_types_[index], row[columns_[index]]));
  return seed;
}

std::size_t RuntimeIndex::hash(const KeyView &key) const {
  if (key.size != columns_.size())
    throw std::logic_error("Datalog lookup key does not match index mask");
  std::size_t seed = 0;
  for (std::size_t index = 0; index < key.size; ++index)
    combineHash(seed, hashValue(column_types_[index], key.values[index]));
  return seed;
}

void RuntimeIndex::insert(const RelationStorage &storage,
                          std::size_t row_index) {
  buckets_[hash(storage.row(row_index))].push_back(row_index);
}

void RuntimeIndex::erase(std::size_t key_hash, std::size_t row_index) {
  auto bucket = buckets_.find(key_hash);
  if (bucket == buckets_.end())
    throw std::logic_error("Datalog index lost an indexed row");
  auto position =
      std::find(bucket->second.begin(), bucket->second.end(), row_index);
  if (position == bucket->second.end())
    throw std::logic_error("Datalog index lost an indexed row");
  bucket->second.erase(position);
  if (bucket->second.empty())
    buckets_.erase(bucket);
}

void RuntimeIndex::rebuild(const RelationStorage &storage,
                           std::size_t version) {
  buckets_.clear();
  buckets_.reserve(storage.rowCount());
  for (std::size_t row_index = 0; row_index < storage.rowCount(); ++row_index)
    insert(storage, row_index);
  built_version_ = version;
}

void RuntimeIndex::append(const RelationStorage &storage, std::size_t row_index,
                          std::size_t version) {
  if (!isCurrent(version - 1))
    return;
  insert(storage, row_index);
  built_version_ = version;
}

void RuntimeIndex::update(const RelationStorage &storage, std::size_t row_index,
                          const Row &previous_row, std::size_t version) {
  if (!isCurrent(version - 1))
    return;
  const std::size_t old_hash = hash(RowView(previous_row));
  const std::size_t new_hash = hash(storage.row(row_index));
  if (old_hash != new_hash) {
    erase(old_hash, row_index);
    buckets_[new_hash].push_back(row_index);
  }
  built_version_ = version;
}

bool RuntimeIndex::isCurrent(std::size_t version) const {
  return built_version_ == version;
}

const std::vector<std::size_t> *RuntimeIndex::lookup(const KeyView &key) const {
  auto it = buckets_.find(hash(key));
  return it == buckets_.end() ? nullptr : &it->second;
}

bool RuntimeIndex::matches(RowView row, const KeyView &key) const {
  if (key.size != columns_.size())
    return false;
  for (std::size_t index = 0; index < key.size; ++index) {
    if (!equalValues(column_types_[index], row[columns_[index]],
                     key.values[index]))
      return false;
  }
  return true;
}

std::size_t RuntimeIndex::bucketCount() const { return buckets_.size(); }

std::size_t RuntimeIndex::entryCount() const {
  std::size_t count = 0;
  for (const auto &[key, rows] : buckets_) {
    (void)key;
    count += rows.size();
  }
  return count;
}

std::size_t RuntimeIndex::approximateMemoryBytes() const {
  std::size_t bytes = buckets_.bucket_count() * sizeof(void *);
  for (const auto &[key, rows] : buckets_) {
    (void)key;
    bytes += sizeof(key) + sizeof(rows) + rows.capacity() * sizeof(std::size_t);
  }
  return bytes;
}

RelationStorage::RelationStorage(RelationIR definition)
    : definition_(std::move(definition)) {
  columns_.reserve(definition_.columns.size());
  for (const ColumnType &column : definition_.columns) {
    columns_.push_back(column.make_storage ? column.make_storage()
                                           : std::make_unique<AnyColumnStorage>());
  }
  if (definition_.kind == RelationKind::Lattice) {
    std::vector<ColumnType> key_columns(definition_.columns.begin(),
                                        definition_.columns.end() - 1);
    lattice_keys_ = std::make_unique<KeyMap>(0, KeyHash{key_columns},
                                             KeyEqual{key_columns});
    base_lattice_keys_ = std::make_unique<KeyMap>(0, KeyHash{key_columns},
                                                  KeyEqual{key_columns});
  }
}

const RelationIR &RelationStorage::definition() const { return definition_; }

std::size_t RelationStorage::rowCount() const { return row_count_; }

RowView RelationStorage::row(std::size_t row_id) const {
  if (row_id >= row_count_)
    throw std::out_of_range("Datalog row ID is outside relation storage");
  return RowView(*this, row_id);
}

ValueRef RelationStorage::value(std::size_t row_id, std::size_t column) const {
  if (row_id >= row_count_)
    throw std::out_of_range("Datalog row ID is outside relation storage");
  return columns_.at(column)->value(row_id);
}

Row RelationStorage::materializeRow(std::size_t row_id) const {
  Row result;
  result.reserve(columns_.size());
  for (const auto &column : columns_)
    result.push_back(column->materialize(row_id));
  return result;
}

std::vector<Row> RelationStorage::materializeRows() const {
  std::vector<Row> result;
  result.reserve(row_count_);
  for (std::size_t row_id = 0; row_id < row_count_; ++row_id)
    result.push_back(materializeRow(row_id));
  return result;
}

std::optional<std::size_t>
RelationStorage::findSetRow(const Row &row) const {
  const auto bucket = set_directory_.find(candidateHash(row));
  if (bucket == set_directory_.end())
    return std::nullopt;
  for (std::size_t row_id : bucket->second) {
    if (rowsEqual(this->row(row_id), RowView(row)))
      return row_id;
  }
  return std::nullopt;
}

void RelationStorage::addSetRow(std::size_t row_id) {
  set_directory_[candidateHash(row(row_id))].push_back(row_id);
}

void RelationStorage::rebuildSetDirectory() {
  set_directory_.clear();
  set_directory_.reserve(row_count_);
  for (std::size_t row_id = 0; row_id < row_count_; ++row_id)
    addSetRow(row_id);
}

void RelationStorage::appendRow(Row row) {
  const std::size_t row_index = row_count_;
  std::size_t appended = 0;
  try {
    for (; appended < columns_.size(); ++appended)
      columns_[appended]->append(std::move(row[appended]));
  } catch (...) {
    for (std::size_t column = 0; column < appended; ++column)
      columns_[column]->truncate(row_count_);
    throw;
  }
  ++row_count_;
  if (definition_.kind == RelationKind::Set)
    base_flags_.push_back(0);
  if (definition_.kind == RelationKind::Set)
    base_add_versions_.push_back(0);
  ++version_;
  std::lock_guard<std::mutex> lock(index_mutex_);
  for (auto &[mask, index] : indices_) {
    (void)mask;
    index->append(*this, row_index, version_);
  }
}

void RelationStorage::updateRow(std::size_t row_index, Row row) {
  Row previous_row = materializeRow(row_index);
  std::size_t updated = 0;
  try {
    for (; updated < columns_.size(); ++updated)
      columns_[updated]->update(row_index, std::move(row[updated]));
  } catch (...) {
    for (std::size_t column = 0; column < updated; ++column)
      columns_[column]->update(row_index, previous_row[column]);
    throw;
  }
  ++version_;
  std::lock_guard<std::mutex> lock(index_mutex_);
  for (auto &[mask, index] : indices_) {
    (void)mask;
    index->update(*this, row_index, previous_row, version_);
  }
}

bool RelationStorage::insertBase(Row row) {
  validateRow(row);
  if (definition_.kind == RelationKind::Set) {
    if (const std::optional<std::size_t> existing = findSetRow(row)) {
      if (base_flags_[*existing])
        return false;
      base_flags_[*existing] = 1;
      ++base_version_;
      return true;
    }
    const std::size_t row_id = row_count_;
    appendRow(std::move(row));
    addSetRow(row_id);
    base_flags_[row_id] = 1;
    ++base_version_;
    base_add_versions_[row_id] = base_version_;
    return true;
  }

  Row key = latticeKey(row);
  auto base_found = base_lattice_keys_->find(key);
  if (base_found == base_lattice_keys_->end()) {
    const std::size_t base_index = base_rows_.size();
    base_rows_.push_back(row);
    base_lattice_keys_->emplace(std::move(key), base_index);
    ++base_version_;
  } else {
    Row proposed = base_rows_[base_found->second];
    if (!definition_.lattice_join(proposed.back(), row.back()))
      return false;
    base_rows_[base_found->second] = std::move(proposed);
    ++base_version_;
  }

  const Row &base_row =
      base_rows_[base_lattice_keys_->find(latticeKey(row))->second];
  auto total_found = lattice_keys_->find(latticeKey(base_row));
  if (total_found == lattice_keys_->end()) {
    const std::size_t total_index = row_count_;
    lattice_keys_->emplace(latticeKey(base_row), total_index);
    appendRow(base_row);
    return true;
  }

  Row proposed = materializeRow(total_found->second);
  if (definition_.lattice_join(proposed.back(), base_row.back()))
    updateRow(total_found->second, std::move(proposed));
  return true;
}

bool RelationStorage::contains(const Row &row) const {
  validateRow(row);
  if (definition_.kind == RelationKind::Lattice) {
    auto it = lattice_keys_->find(latticeKey(row));
    if (it == lattice_keys_->end())
      return false;
    const ColumnType &column = definition_.columns.back();
    return equalValues(column, value(it->second, columns_.size() - 1),
                       ValueRef::fromAny(row.back()));
  }
  return findSetRow(row).has_value();
}

std::vector<Row> RelationStorage::coalesce(std::vector<Row> candidates) const {
  if (definition_.kind != RelationKind::Lattice) {
    SetDirectory unique;
    unique.reserve(candidates.size());
    std::vector<Row> result;
    result.reserve(candidates.size());
    for (Row &candidate : candidates) {
      validateRow(candidate);
      const std::size_t fingerprint = candidateHash(candidate);
      std::vector<std::size_t> &bucket = unique[fingerprint];
      bool duplicate = false;
      for (std::size_t row_id : bucket) {
        if (rowsEqual(result[row_id], candidate)) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        bucket.push_back(result.size());
        result.push_back(std::move(candidate));
      }
    }
    return result;
  }

  std::vector<ColumnType> key_columns(definition_.columns.begin(),
                                      definition_.columns.end() - 1);
  KeyMap keys(0, KeyHash{key_columns}, KeyEqual{key_columns});
  std::vector<Row> result;
  for (Row &candidate : candidates) {
    validateRow(candidate);
    Row key = latticeKey(candidate);
    auto [it, inserted] = keys.emplace(std::move(key), result.size());
    if (inserted) {
      result.push_back(std::move(candidate));
      continue;
    }
    Row proposed = result[it->second];
    if (definition_.lattice_join(proposed.back(), candidate.back()))
      result[it->second] = std::move(proposed);
  }
  return result;
}

std::size_t RelationStorage::candidateHash(const Row &row) const {
  validateRow(row);
  const std::size_t column_count =
      definition_.kind == RelationKind::Lattice ? row.size() - 1 : row.size();
  std::size_t seed = 0;
  for (std::size_t column = 0; column < column_count; ++column)
    combineHash(seed, definition_.columns[column].hash(row[column]));
  return seed;
}

std::size_t RelationStorage::candidateHash(RowView row) const {
  const std::size_t column_count =
      definition_.kind == RelationKind::Lattice ? row.size() - 1 : row.size();
  std::size_t seed = 0;
  for (std::size_t column = 0; column < column_count; ++column)
    combineHash(seed, hashValue(definition_.columns[column], row[column]));
  return seed;
}

bool RelationStorage::rowsEqual(const Row &lhs, const Row &rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t column = 0; column < lhs.size(); ++column) {
    if (!definition_.columns[column].equal(lhs[column], rhs[column]))
      return false;
  }
  return true;
}

bool RelationStorage::rowsEqual(RowView lhs, RowView rhs) const {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t column = 0; column < lhs.size(); ++column) {
    if (!equalValues(definition_.columns[column], lhs[column], rhs[column]))
      return false;
  }
  return true;
}

RelationStorage::BatchMergeResult RelationStorage::mergeDerivedCoalesced(
    std::vector<Row> candidates, Scheduler &scheduler, std::size_t grain_size) {
  BatchMergeResult result;
  if (candidates.empty())
    return result;
  for (const Row &candidate : candidates)
    validateRow(candidate);

  const std::size_t grain = std::max<std::size_t>(1, grain_size);
  const std::size_t task_count = std::min(
      scheduler.workerCount(), (candidates.size() + grain - 1) / grain);
  std::vector<unsigned char> changed(candidates.size(), 0);
  std::vector<std::optional<std::size_t>> lattice_rows(candidates.size());
  std::vector<std::optional<Row>> proposals(candidates.size());

  // Inspection deliberately works on copies.  A user lattice operation may
  // throw; no live row or index may change unless every inspect task succeeds.
  auto inspect = [&](std::size_t task) {
    const std::size_t begin = candidates.size() * task / task_count;
    const std::size_t end = candidates.size() * (task + 1) / task_count;
    if (definition_.kind == RelationKind::Set) {
      for (std::size_t index = begin; index < end; ++index)
        changed[index] = !findSetRow(candidates[index]).has_value();
      return;
    }
    for (std::size_t index = begin; index < end; ++index) {
      auto found = lattice_keys_->find(latticeKey(candidates[index]));
      if (found == lattice_keys_->end()) {
        changed[index] = 1;
        continue;
      }
      lattice_rows[index] = found->second;
      Row proposed = materializeRow(found->second);
      if (definition_.lattice_join(proposed.back(), candidates[index].back())) {
        changed[index] = 1;
        proposals[index] = std::move(proposed);
      }
    }
  };

  if (task_count > 1) {
    scheduler.parallelFor(task_count, inspect);
    result.parallel_tasks += task_count;
  } else {
    inspect(0);
  }

  result.changed_row_ids.reserve(candidates.size());
  result.changed_lattice_rows.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (!changed[index])
      continue;
    if (definition_.kind == RelationKind::Set) {
      if (!findSetRow(candidates[index])) {
        const std::size_t row_id = row_count_;
        appendRow(std::move(candidates[index]));
        addSetRow(row_id);
        has_derived_state_ = true;
        result.changed_row_ids.push_back(row_id);
      }
      continue;
    }
    if (lattice_rows[index]) {
      updateRow(*lattice_rows[index], std::move(*proposals[index]));
      has_derived_state_ = true;
      result.changed_lattice_rows.push_back(
          materializeRow(*lattice_rows[index]));
      continue;
    }
    const std::size_t row_index = row_count_;
    lattice_keys_->emplace(latticeKey(candidates[index]), row_index);
    appendRow(std::move(candidates[index]));
    has_derived_state_ = true;
    result.changed_lattice_rows.push_back(materializeRow(row_count_ - 1));
  }
  return result;
}

void RelationStorage::rebuildFromBase() {
  if (definition_.kind == RelationKind::Set) {
    std::vector<std::size_t> retained;
    std::vector<std::size_t> retained_base_versions;
    retained.reserve(row_count_);
    retained_base_versions.reserve(row_count_);
    for (std::size_t row_id = 0; row_id < row_count_; ++row_id) {
      if (base_flags_[row_id]) {
        retained.push_back(row_id);
        retained_base_versions.push_back(base_add_versions_[row_id]);
      }
    }
    std::vector<std::unique_ptr<ColumnStorage>> selected;
    selected.reserve(columns_.size());
    for (const auto &column : columns_)
      selected.push_back(column->select(retained));
    columns_ = std::move(selected);
    row_count_ = retained.size();
    base_flags_.assign(row_count_, 1);
    base_add_versions_ = std::move(retained_base_versions);
    rebuildSetDirectory();
  } else {
    std::vector<std::unique_ptr<ColumnStorage>> replacement;
    replacement.reserve(definition_.columns.size());
    for (const ColumnType &column : definition_.columns) {
      replacement.push_back(column.make_storage
                                ? column.make_storage()
                                : std::make_unique<AnyColumnStorage>());
      replacement.back()->reserve(base_rows_.size());
    }
    for (const Row &base_row : base_rows_) {
      for (std::size_t column = 0; column < replacement.size(); ++column)
        replacement[column]->append(base_row[column]);
    }
    columns_ = std::move(replacement);
    row_count_ = base_rows_.size();
    lattice_keys_->clear();
    lattice_keys_->reserve(base_rows_.size());
    for (std::size_t row_index = 0; row_index < base_rows_.size(); ++row_index)
      lattice_keys_->emplace(latticeKey(base_rows_[row_index]), row_index);
  }

  ++version_;
  std::lock_guard<std::mutex> lock(index_mutex_);
  for (auto &[mask, index] : indices_) {
    (void)mask;
    index->rebuild(*this, version_);
  }
}

void RelationStorage::discardDerived() {
  if (!has_derived_state_)
    return;
  has_derived_state_ = false;
  rebuildFromBase();
}

std::size_t RelationStorage::baseVersion() const { return base_version_; }

std::vector<std::size_t> RelationStorage::baseSetDeltaSince(
    std::size_t completed_base_version) const {
  std::vector<std::size_t> result;
  if (definition_.kind != RelationKind::Set)
    return result;
  for (std::size_t row_id = 0; row_id < row_count_; ++row_id) {
    if (base_add_versions_[row_id] > completed_base_version)
      result.push_back(row_id);
  }
  return result;
}

std::size_t RelationStorage::estimatedLookupCardinality(ColumnMask mask) const {
  if (mask == 0)
    return std::max<std::size_t>(1, row_count_);

  std::lock_guard<std::mutex> lock(statistics_mutex_);
  auto cached = lookup_estimates_.find(mask);
  if (cached != lookup_estimates_.end() && cached->second.first == version_)
    return cached->second.second;

  std::vector<ColumnType> key_columns;
  for (std::size_t column = 0; column < definition_.columns.size(); ++column) {
    if (mask & (ColumnMask{1} << column))
      key_columns.push_back(definition_.columns[column]);
  }
  std::unordered_set<Row, KeyHash, KeyEqual> distinct(0, KeyHash{key_columns},
                                                      KeyEqual{key_columns});
  for (std::size_t row_id = 0; row_id < row_count_; ++row_id) {
    Row key;
    key.reserve(key_columns.size());
    for (std::size_t column = 0; column < definition_.columns.size();
         ++column) {
      if (mask & (ColumnMask{1} << column))
        key.push_back(columns_[column]->materialize(row_id));
    }
    distinct.insert(std::move(key));
  }
  const std::size_t estimate =
      distinct.empty()
          ? 1
          : std::max<std::size_t>(1, (row_count_ + distinct.size() - 1) /
                                         distinct.size());
  lookup_estimates_[mask] = {version_, estimate};
  return estimate;
}

void RelationStorage::ensureIndex(ColumnMask mask) {
  if (mask == 0)
    return;
  std::lock_guard<std::mutex> lock(index_mutex_);
  RuntimeIndex &index = getIndex(mask);
  if (!index.isCurrent(version_))
    index.rebuild(*this, version_);
}

std::size_t RelationStorage::indexCount() const { return indices_.size(); }

std::size_t RelationStorage::indexEntries() const {
  std::size_t count = 0;
  for (const auto &[mask, index] : indices_) {
    (void)mask;
    count += index->entryCount();
  }
  return count;
}

std::size_t RelationStorage::indexMemoryBytes() const {
  std::size_t bytes = 0;
  for (const auto &[mask, index] : indices_) {
    (void)mask;
    bytes += index->approximateMemoryBytes();
  }
  return bytes;
}

std::size_t RelationStorage::tupleMemoryBytes() const {
  std::size_t bytes = 0;
  for (const auto &column : columns_)
    bytes += column->approximateMemoryBytes();
  return bytes;
}

std::size_t RelationStorage::uniquenessMemoryBytes() const {
  std::size_t bytes = 0;
  if (definition_.kind == RelationKind::Set) {
    bytes += set_directory_.bucket_count() * sizeof(void *);
    for (const auto &[fingerprint, rows] : set_directory_) {
      (void)fingerprint;
      bytes += sizeof(fingerprint) + sizeof(rows) +
               rows.capacity() * sizeof(std::size_t);
    }
    return bytes;
  }

  bytes += lattice_keys_->bucket_count() * sizeof(void *);
  for (const auto &[key, row_id] : *lattice_keys_) {
    (void)row_id;
    bytes += sizeof(key) + key.capacity() * sizeof(std::any) + sizeof(row_id);
  }
  return bytes;
}

std::size_t RelationStorage::baseMemoryBytes() const {
  if (definition_.kind == RelationKind::Set)
    return base_flags_.capacity() * sizeof(unsigned char) +
           base_add_versions_.capacity() * sizeof(std::size_t);

  std::size_t bytes = base_rows_.capacity() * sizeof(Row);
  for (const Row &row : base_rows_)
    bytes += row.capacity() * sizeof(std::any);
  bytes += base_lattice_keys_->bucket_count() * sizeof(void *);
  for (const auto &[key, row_id] : *base_lattice_keys_) {
    (void)row_id;
    bytes += sizeof(key) + key.capacity() * sizeof(std::any) + sizeof(row_id);
  }
  return bytes;
}

void RelationStorage::forEachMatching(
    const KeyView &key, ExecutionStats &stats,
    const std::function<void(RowView)> &callback) {
  if (key.mask == 0) {
    for (std::size_t row_id = 0; row_id < row_count_; ++row_id) {
      ++stats.tuples_scanned;
      callback(row(row_id));
    }
    return;
  }

  ++stats.index_lookups;
  RuntimeIndex &index = preparedIndex(key.mask);
  const std::vector<std::size_t> *matches = index.lookup(key);
  if (!matches)
    return;
  for (std::size_t row_index : *matches) {
    RowView matching_row = row(row_index);
    if (!index.matches(matching_row, key))
      continue;
    ++stats.tuples_scanned;
    callback(matching_row);
  }
}

std::size_t RelationStorage::matchingCandidateCount(const KeyView &key,
                                                    ExecutionStats &stats) {
  if (key.mask == 0)
    return row_count_;
  ++stats.index_lookups;
  RuntimeIndex &index = preparedIndex(key.mask);
  const std::vector<std::size_t> *matches = index.lookup(key);
  return matches ? matches->size() : 0;
}

void RelationStorage::forEachMatchingSlice(
    const KeyView &key, std::size_t begin, std::size_t end,
    const std::function<void(RowView)> &callback) {
  if (key.mask == 0) {
    const std::size_t bounded_end = std::min(end, row_count_);
    for (std::size_t row_index = begin; row_index < bounded_end; ++row_index)
      callback(row(row_index));
    return;
  }

  RuntimeIndex &index = preparedIndex(key.mask);
  const std::vector<std::size_t> *matches = index.lookup(key);
  if (!matches)
    return;
  const std::size_t bounded_end = std::min(end, matches->size());
  for (std::size_t match_index = begin; match_index < bounded_end;
       ++match_index) {
    RowView matching_row = row((*matches)[match_index]);
    if (index.matches(matching_row, key))
      callback(matching_row);
  }
}

void RelationStorage::validateRow(const Row &row) const {
  if (row.size() != definition_.columns.size())
    throw std::invalid_argument("fact arity does not match relation '" +
                                definition_.name + "'");
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (std::type_index(row[i].type()) != definition_.columns[i].type) {
      throw std::invalid_argument("fact column type does not match relation '" +
                                  definition_.name + "'");
    }
    if (definition_.columns[i].validate)
      definition_.columns[i].validate(row[i]);
    const bool is_key = definition_.kind == RelationKind::Set ||
                        i + 1 != definition_.columns.size();
    if (is_key && definition_.columns[i].validate_key)
      definition_.columns[i].validate_key(row[i]);
  }
}

RuntimeIndex &RelationStorage::getIndex(ColumnMask mask) {
  auto it = indices_.find(mask);
  if (it == indices_.end()) {
    it = indices_
             .emplace(mask,
                      std::make_unique<RuntimeIndex>(mask, definition_.columns))
             .first;
  }
  return *it->second;
}

RuntimeIndex &RelationStorage::preparedIndex(ColumnMask mask) {
  auto it = indices_.find(mask);
  if (it == indices_.end() || !it->second->isCurrent(version_)) {
    throw std::logic_error(
        "Datalog execution attempted to read an unprepared runtime index");
  }
  return *it->second;
}

Row RelationStorage::latticeKey(const Row &row) const {
  return Row(row.begin(), row.end() - 1);
}

} // namespace lotus::datalog::internal
