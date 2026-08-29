#ifndef DATAFLOW_APA_DOMAINS_AFFINERELATIONDOMAIN_H_
#define DATAFLOW_APA_DOMAINS_AFFINERELATIONDOMAIN_H_

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

using AffineRow = std::vector<llvm::APInt>;
using AffineMatrix = std::vector<AffineRow>;
using MOSTransformerSet = std::vector<AffineMatrix>;

struct AffineRelationVocabulary {
  std::vector<const llvm::Value *> values;
  std::unordered_map<const llvm::Value *, unsigned> indices;
  std::unordered_map<const llvm::Value *, unsigned> actualBitWidths;
  std::vector<const llvm::Value *> localValues;
};

struct AffineRelationComponent {
  unsigned bitWidth = 0;
  AffineMatrix constraints;

  bool operator==(const AffineRelationComponent &other) const;
};

struct AffineRelation {
  bool bottom = false;
  std::map<unsigned, AffineRelationComponent> components;

  bool operator==(const AffineRelation &other) const;
};

struct AffineGeneratorRelation {
  AffineRelation relation;
  std::map<unsigned, AffineMatrix> generators;
  bool bottom = false;
  bool exact = true;

  bool operator==(const AffineGeneratorRelation &other) const;
};

struct AffineDiagonalDecomposition {
  unsigned bitWidth = 0;
  AffineMatrix left;
  AffineMatrix leftInverse;
  AffineMatrix diagonal;
  AffineMatrix right;
  AffineMatrix rightInverse;
  AffineMatrix dual;
  bool exact = true;

  bool operator==(const AffineDiagonalDecomposition &other) const;
};

struct MOSRelation {
  enum class ConversionKind { Direct, HavocPreStateGuards, MakeExplicit };

  AffineRelation relation;
  std::map<unsigned, MOSTransformerSet> transformers;
  ConversionKind kind = ConversionKind::Direct;
  bool exact = true;

  bool operator==(const MOSRelation &other) const;
};

class AffineRelationDomain {
public:
  using value_type = AffineRelation;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static void configure(const AffineRelationVocabulary *vocabulary);
  static const AffineRelationVocabulary *getVocabulary();

  static bool isTrackedValue(const llvm::Value *value);
  static unsigned bitWidthOf(const llvm::Value *value);
  static unsigned componentBitWidth();
  static unsigned indexOf(const llvm::Value *value);

  static value_type zero();
  static value_type bottom() { return zero(); }
  static value_type top();
  static value_type one();
  static bool equal(const value_type &lhs, const value_type &rhs);
  static bool isBottom(const value_type &relation);
  static bool contains(const value_type &lhs, const value_type &rhs);
  static value_type meet(const value_type &lhs, const value_type &rhs);
  static value_type combine(const value_type &lhs, const value_type &rhs);
  static value_type join(const value_type &lhs, const value_type &rhs) {
    return combine(lhs, rhs);
  }
  static value_type ndetCombine(const value_type &lhs, const value_type &rhs);
  static value_type condCombine(bool /*phi*/, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &outer, const value_type &inner);
  static value_type extend_lin(const value_type &outer,
                               const value_type &inner);
  static value_type subtract(const value_type &lhs, const value_type &rhs);
  static value_type project(const value_type &relation);

  static value_type identity();
  static value_type addStateConstraint(
      const value_type &relation, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);
  static value_type addPrecondition(const value_type &relation,
                                    const llvm::Value *value, int64_t constant);
  static value_type makeForget(const llvm::Value *dest);
  static value_type havoc(const value_type &relation, const llvm::Value *value);
  static value_type havoc(const value_type &relation,
                          const std::vector<const llvm::Value *> &values);
  static value_type
  projectOnto(const value_type &relation,
              const std::vector<const llvm::Value *> &keepValues);
  static value_type
  mergePreservingLocals(const value_type &callSite,
                        const value_type &calleeExit,
                        const std::vector<const llvm::Value *> &locals);
  static llvm::APInt size(const value_type &relation);
  static value_type makeAffineAssignment(
      const llvm::Value *dest, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);
  static value_type makeAffineCongruenceAssignment(
      const llvm::Value *dest, unsigned modulusBits, int64_t constant,
      const std::vector<std::pair<const llvm::Value *, int64_t>> &terms);

  static AffineGeneratorRelation toAffineGenerator(const value_type &relation);
  static value_type
  fromAffineGenerator(const AffineGeneratorRelation &relation);
  static AffineGeneratorRelation
  joinAffineGenerators(const AffineGeneratorRelation &lhs,
                       const AffineGeneratorRelation &rhs);
  static AffineDiagonalDecomposition
  diagonalDecompose(const AffineMatrix &matrix, unsigned bitWidth);
  static AffineMatrix dualizePerp(const AffineMatrix &matrix, unsigned bitWidth,
                                  unsigned columns);

  static MOSRelation toMOS(const value_type &relation);
  static MOSRelation toMOSWithHavocedPreStateGuards(const value_type &relation);
  static MOSRelation toMOSWithMakeExplicit(const value_type &relation);
  static value_type fromMOS(const MOSRelation &relation);
  static MOSRelation joinMOS(const MOSRelation &lhs, const MOSRelation &rhs);

private:
  static AffineRelationVocabulary Vocabulary;
  static bool HasVocabulary;
};

} // namespace elimination

#endif // DATAFLOW_APA_DOMAINS_AFFINERELATIONDOMAIN_H_
