#include "Dataflow/NPA/Domains/AffineRelationDomain.h"

#include <algorithm>

#include <llvm/IR/Value.h>

namespace npa {

AffineRelationVocabulary AffineRelationDomain::Vocabulary{};
bool AffineRelationDomain::HasVocabulary = false;

namespace {

using Row = std::vector<llvm::APInt>;
using Matrix = std::vector<Row>;

unsigned numVarsFor(unsigned bitWidth) {
  auto *vocab = AffineRelationDomain::getVocabulary();
  return (!vocab || bitWidth != AffineRelationDomain::componentBitWidth())
             ? 0u
             : static_cast<unsigned>(vocab->values.size());
}

Row zeroRow(unsigned bitWidth, unsigned cols) {
  return Row(cols, llvm::APInt(bitWidth, 0));
}

bool isZeroRow(const Row &row) {
  return std::all_of(row.begin(), row.end(),
                     [](const llvm::APInt &entry) { return entry.isZero(); });
}

int leadingIndex(const Row &row) {
  for (size_t i = 0; i < row.size(); ++i) {
    if (!row[i].isZero())
      return static_cast<int>(i);
  }
  return -1;
}

unsigned rankOf(const llvm::APInt &value) {
  return value.isZero() ? value.getBitWidth() : value.countTrailingZeros();
}

llvm::APInt oddInverse(const llvm::APInt &odd) {
  unsigned bitWidth = odd.getBitWidth();
  llvm::APInt inv(bitWidth, 1);
  llvm::APInt two(bitWidth, 2);
  for (unsigned bits = 1; bits < bitWidth; bits <<= 1)
    inv *= (two - odd * inv);
  return inv;
}

void scaleRow(Row &row, const llvm::APInt &factor) {
  for (auto &entry : row)
    entry *= factor;
}

void subtractScaledRow(Row &row, const Row &pivot, const llvm::APInt &factor) {
  for (size_t i = 0; i < row.size(); ++i)
    row[i] -= factor * pivot[i];
}

Matrix howellize(Matrix rows, unsigned bitWidth) {
  if (rows.empty())
    return rows;
  const size_t cols = rows.front().size();
  size_t nextRow = 0;

  for (size_t col = 0; col < cols; ++col) {
    std::vector<size_t> candidates;
    for (size_t r = nextRow; r < rows.size(); ++r) {
      if (leadingIndex(rows[r]) == static_cast<int>(col))
        candidates.push_back(r);
    }
    if (candidates.empty())
      continue;

    size_t pivotPos = candidates.front();
    for (size_t idx : candidates) {
      if (rankOf(rows[idx][col]) < rankOf(rows[pivotPos][col]))
        pivotPos = idx;
    }

    unsigned pivotRank = rankOf(rows[pivotPos][col]);
    llvm::APInt oddPart = rows[pivotPos][col].lshr(pivotRank);
    scaleRow(rows[pivotPos], oddInverse(oddPart));

    for (size_t idx : candidates) {
      if (idx == pivotPos)
        continue;
      unsigned curRank = rankOf(rows[idx][col]);
      llvm::APInt factor(bitWidth, 1);
      factor <<= (curRank - pivotRank);
      factor *= rows[idx][col].lshr(curRank);
      subtractScaledRow(rows[idx], rows[pivotPos], factor);
    }

    rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
    auto it = std::find_if(rows.begin() + nextRow, rows.end(),
                           [col](const Row &row) {
                             return leadingIndex(row) == static_cast<int>(col);
                           });
    if (it == rows.end())
      continue;
    std::iter_swap(rows.begin() + nextRow, it);

    const Row pivot = rows[nextRow];
    for (size_t upper = 0; upper < nextRow; ++upper) {
      llvm::APInt factor = rows[upper][col].lshr(pivotRank);
      subtractScaledRow(rows[upper], pivot, factor);
    }

    if (!rows[nextRow][col].isOne()) {
      llvm::APInt factor(bitWidth, 1);
      factor <<= (bitWidth - pivotRank);
      Row implied = rows[nextRow];
      scaleRow(implied, factor);
      if (!isZeroRow(implied))
        rows.push_back(std::move(implied));
    }

    ++nextRow;
  }

  rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
  return rows;
}

AffineRelationComponent makeIdentityComponent(unsigned bitWidth) {
  AffineRelationComponent component;
  component.bitWidth = bitWidth;
  const unsigned vars = numVarsFor(bitWidth);
  for (unsigned i = 0; i < vars; ++i) {
    Row row = zeroRow(bitWidth, 2 * vars + 1);
    row[i] = llvm::APInt(bitWidth, 1);
    row[vars + i] = llvm::APInt(bitWidth, -1, true);
    component.constraints.push_back(std::move(row));
  }
  component.constraints = howellize(std::move(component.constraints), bitWidth);
  return component;
}

AffineRelationComponent bottomComponent(unsigned bitWidth) {
  AffineRelationComponent component;
  component.bitWidth = bitWidth;
  Row row = zeroRow(bitWidth, 2 * numVarsFor(bitWidth) + 1);
  row.back() = llvm::APInt(bitWidth, 1);
  component.constraints.push_back(std::move(row));
  return component;
}

bool componentIsBottom(const AffineRelationComponent &component) {
  if (component.constraints.size() != 1)
    return false;
  const Row &row = component.constraints.front();
  return std::all_of(row.begin(), row.end() - 1,
                     [](const llvm::APInt &entry) { return entry.isZero(); }) &&
         row.back().isOne();
}

AffineRelationComponent normalizeComponent(AffineRelationComponent component) {
  component.constraints =
      howellize(std::move(component.constraints), component.bitWidth);
  const unsigned vars = numVarsFor(component.bitWidth);
  for (const Row &row : component.constraints) {
    if (leadingIndex(row) == static_cast<int>(2 * vars) && row.back().isOne()) {
      return bottomComponent(component.bitWidth);
    }
  }
  return component;
}

AffineRelationComponent projectSuffix(const AffineRelationComponent &component,
                                      unsigned keepCols) {
  if (component.constraints.empty())
    return component;
  const size_t totalCols = component.constraints.front().size();
  const size_t dropCols = totalCols - keepCols;
  Matrix rows = howellize(component.constraints, component.bitWidth);
  Matrix projected;
  for (const Row &row : rows) {
    int lead = leadingIndex(row);
    if (lead < 0 || static_cast<size_t>(lead) < dropCols)
      continue;
    Row keep(row.begin() + dropCols, row.end());
    if (!isZeroRow(keep))
      projected.push_back(std::move(keep));
  }
  AffineRelationComponent out = component;
  out.constraints = howellize(std::move(projected), component.bitWidth);
  return out;
}

AffineRelationComponent composeComponent(const AffineRelationComponent &outer,
                                         const AffineRelationComponent &inner) {
  if (componentIsBottom(outer) || componentIsBottom(inner))
    return bottomComponent(outer.bitWidth);
  const unsigned vars = numVarsFor(outer.bitWidth);
  Matrix rows;
  for (const Row &row : inner.constraints) {
    Row lifted = zeroRow(outer.bitWidth, 3 * vars + 1);
    std::copy(row.begin() + vars, row.begin() + 2 * vars, lifted.begin());
    std::copy(row.begin(), row.begin() + vars, lifted.begin() + vars);
    lifted.back() = row.back();
    rows.push_back(std::move(lifted));
  }
  for (const Row &row : outer.constraints) {
    Row lifted = zeroRow(outer.bitWidth, 3 * vars + 1);
    std::copy(row.begin(), row.begin() + vars, lifted.begin());
    std::copy(row.begin() + vars, row.begin() + 2 * vars,
              lifted.begin() + 2 * vars);
    lifted.back() = row.back();
    rows.push_back(std::move(lifted));
  }
  AffineRelationComponent tmp;
  tmp.bitWidth = outer.bitWidth;
  tmp.constraints = std::move(rows);
  tmp = normalizeComponent(std::move(tmp));
  return projectSuffix(tmp, 2 * vars + 1);
}

AffineRelationComponent joinComponent(const AffineRelationComponent &lhs,
                                      const AffineRelationComponent &rhs) {
  if (componentIsBottom(lhs))
    return rhs;
  if (componentIsBottom(rhs))
    return lhs;
  const unsigned vars = numVarsFor(lhs.bitWidth);
  Matrix rows;
  for (const Row &row : lhs.constraints) {
    Row lifted = zeroRow(lhs.bitWidth, 4 * vars + 2);
    for (unsigned i = 0; i < 2 * vars + 1; ++i)
      lifted[i] = -row[i];
    std::copy(row.begin(), row.end(), lifted.begin() + 2 * vars + 1);
    rows.push_back(std::move(lifted));
  }
  for (const Row &row : rhs.constraints) {
    Row lifted = zeroRow(lhs.bitWidth, 4 * vars + 2);
    std::copy(row.begin(), row.end(), lifted.begin());
    rows.push_back(std::move(lifted));
  }
  AffineRelationComponent tmp;
  tmp.bitWidth = lhs.bitWidth;
  tmp.constraints = std::move(rows);
  tmp = normalizeComponent(std::move(tmp));
  return projectSuffix(tmp, 2 * vars + 1);
}

} // namespace

bool AffineRelationComponent::operator==(const AffineRelationComponent &other) const {
  return bitWidth == other.bitWidth && constraints == other.constraints;
}

bool AffineRelation::operator==(const AffineRelation &other) const {
  return bottom == other.bottom && components == other.components;
}

void AffineRelationDomain::configure(const AffineRelationVocabulary *vocabulary) {
  if (vocabulary) {
    Vocabulary = *vocabulary;
    HasVocabulary = true;
  } else {
    Vocabulary = {};
    HasVocabulary = false;
  }
}

const AffineRelationVocabulary *AffineRelationDomain::getVocabulary() {
  return HasVocabulary ? &Vocabulary : nullptr;
}

bool AffineRelationDomain::isTrackedValue(const llvm::Value *value) {
  return HasVocabulary && Vocabulary.indices.count(value);
}

unsigned AffineRelationDomain::bitWidthOf(const llvm::Value *value) {
  auto it = Vocabulary.actualBitWidths.find(value);
  return it == Vocabulary.actualBitWidths.end() ? 0u : it->second;
}

unsigned AffineRelationDomain::componentBitWidth() { return 64; }

unsigned AffineRelationDomain::indexOf(const llvm::Value *value) {
  auto it = Vocabulary.indices.find(value);
  return it == Vocabulary.indices.end() ? 0u : it->second;
}

AffineRelationDomain::value_type AffineRelationDomain::zero() {
  value_type relation;
  relation.bottom = true;
  if (!HasVocabulary)
    return relation;
  relation.components.emplace(componentBitWidth(), bottomComponent(componentBitWidth()));
  return relation;
}

AffineRelationDomain::value_type AffineRelationDomain::one() {
  return identity();
}

AffineRelationDomain::value_type AffineRelationDomain::identity() {
  value_type relation;
  if (!HasVocabulary)
    return relation;
  relation.components.emplace(componentBitWidth(),
                              makeIdentityComponent(componentBitWidth()));
  return relation;
}

AffineRelationDomain::value_type
AffineRelationDomain::addPrecondition(const value_type &relation,
                                      const llvm::Value *value,
                                      int64_t constant) {
  if (!isTrackedValue(value))
    return relation;
  value_type out = relation;
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  Row row = zeroRow(bitWidth, 2 * vars + 1);
  row[indexOf(value)] = llvm::APInt(bitWidth, 1);
  row.back() = llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true);
  out.components[bitWidth].constraints.push_back(std::move(row));
  Row postRow = zeroRow(bitWidth, 2 * vars + 1);
  postRow[vars + indexOf(value)] = llvm::APInt(bitWidth, 1);
  postRow.back() = llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true);
  out.components[bitWidth].constraints.push_back(std::move(postRow));
  out.components[bitWidth] =
      normalizeComponent(std::move(out.components[bitWidth]));
  return out;
}

bool AffineRelationDomain::equal(const value_type &lhs, const value_type &rhs) {
  return lhs == rhs;
}

AffineRelationDomain::value_type
AffineRelationDomain::combine(const value_type &lhs, const value_type &rhs) {
  if (lhs.bottom)
    return rhs;
  if (rhs.bottom)
    return lhs;
  value_type out;
  unsigned width = componentBitWidth();
  out.components.emplace(
      width, joinComponent(lhs.components.at(width), rhs.components.at(width)));
  return out;
}

AffineRelationDomain::value_type
AffineRelationDomain::ndetCombine(const value_type &lhs, const value_type &rhs) {
  return combine(lhs, rhs);
}

AffineRelationDomain::value_type
AffineRelationDomain::condCombine(bool phi, const value_type &t,
                                  const value_type &e) {
  return phi ? t : e;
}

AffineRelationDomain::value_type
AffineRelationDomain::extend(const value_type &outer, const value_type &inner) {
  if (outer.bottom || inner.bottom)
    return zero();
  value_type out;
  unsigned width = componentBitWidth();
  out.components.emplace(
      width, composeComponent(outer.components.at(width), inner.components.at(width)));
  return out;
}

AffineRelationDomain::value_type
AffineRelationDomain::extend_lin(const value_type &outer,
                                 const value_type &inner) {
  return extend(outer, inner);
}

AffineRelationDomain::value_type
AffineRelationDomain::subtract(const value_type &lhs, const value_type & /*rhs*/) {
  return lhs;
}

AffineRelationDomain::value_type
AffineRelationDomain::makeForget(const llvm::Value *dest) {
  value_type relation = identity();
  if (!isTrackedValue(dest))
    return relation;
  unsigned bitWidth = componentBitWidth();
  unsigned idx = indexOf(dest);
  auto &rows = relation.components[bitWidth].constraints;
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [idx](const Row &row) {
                              return row[idx].isOne() &&
                                     row[idx + numVarsFor(row.front().getBitWidth())]
                                         .isAllOnes();
                            }),
             rows.end());
  relation.components[bitWidth] =
      normalizeComponent(std::move(relation.components[bitWidth]));
  return relation;
}

AffineRelationDomain::value_type
AffineRelationDomain::makeAffineAssignment(
    const llvm::Value *dest, int64_t constant,
    const std::vector<std::pair<const llvm::Value *, int64_t>> &terms) {
  if (!isTrackedValue(dest))
    return identity();
  value_type relation = makeForget(dest);
  unsigned bitWidth = componentBitWidth();
  unsigned vars = numVarsFor(bitWidth);
  unsigned idx = indexOf(dest);
  Row row = zeroRow(bitWidth, 2 * vars + 1);
  row[vars + idx] = llvm::APInt(bitWidth, 1);
  row.back() = llvm::APInt(bitWidth, static_cast<uint64_t>(-constant), true);
  for (const auto &term : terms) {
    if (!isTrackedValue(term.first))
      return makeForget(dest);
    row[indexOf(term.first)] =
        llvm::APInt(bitWidth, static_cast<uint64_t>(-term.second), true);
  }
  relation.components[bitWidth].constraints.push_back(std::move(row));
  relation.components[bitWidth] =
      normalizeComponent(std::move(relation.components[bitWidth]));
  return relation;
}

} // namespace npa
