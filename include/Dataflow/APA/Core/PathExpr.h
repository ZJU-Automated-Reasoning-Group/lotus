#ifndef DATAFLOW_APA_CORE_PATHEXPR_H_
#define DATAFLOW_APA_CORE_PATHEXPR_H_

#include <memory>
#include <utility>

namespace elimination {

template <typename TransferT> class PathExprFactory {
public:
  enum class Kind { Zero, One, Atom, Union, Concat, Star };

  struct Expr final {
    explicit Expr(Kind K) : K(K) {}
    Expr(Kind K, TransferT Transfer)
        : K(K), Transfer(std::make_shared<TransferT>(std::move(Transfer))) {}
    Expr(Kind K, std::shared_ptr<const Expr> L, std::shared_ptr<const Expr> R)
        : K(K), L(std::move(L)), R(std::move(R)) {}
    Expr(Kind K, std::shared_ptr<const Expr> Sub) : K(K), L(std::move(Sub)) {}

    Kind K;
    std::shared_ptr<const TransferT> Transfer;
    std::shared_ptr<const Expr> L;
    std::shared_ptr<const Expr> R;
  };

  using Ref = std::shared_ptr<const Expr>;

  Ref zero() const {
    static const Ref Zero = std::make_shared<Expr>(Kind::Zero);
    return Zero;
  }

  Ref one() const {
    static const Ref One = std::make_shared<Expr>(Kind::One);
    return One;
  }

  Ref atom(TransferT T) const {
    return std::make_shared<Expr>(Kind::Atom, std::move(T));
  }

  Ref unite(const Ref &A, const Ref &B) const {
    if (isZero(A)) {
      return B;
    }
    if (isZero(B)) {
      return A;
    }
    if (A == B) {
      return A;
    }
    return std::make_shared<Expr>(Kind::Union, A, B);
  }

  Ref concat(const Ref &A, const Ref &B) const {
    if (isZero(A) || isZero(B)) {
      return zero();
    }
    if (isOne(A)) {
      return B;
    }
    if (isOne(B)) {
      return A;
    }
    return std::make_shared<Expr>(Kind::Concat, A, B);
  }

  Ref star(const Ref &A) const {
    if (isZero(A) || isOne(A)) {
      return one();
    }
    if (A->K == Kind::Star) {
      return A;
    }
    return std::make_shared<Expr>(Kind::Star, A);
  }

  static bool isZero(const Ref &E) { return E && E->K == Kind::Zero; }
  static bool isOne(const Ref &E) { return E && E->K == Kind::One; }
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_PATHEXPR_H_
