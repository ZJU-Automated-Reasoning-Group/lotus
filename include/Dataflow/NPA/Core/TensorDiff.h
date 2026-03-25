#ifndef NPA_TENSOR_DIFF_H
#define NPA_TENSOR_DIFF_H

#include "Dataflow/NPA/Core/Expressions.h"
#include "Dataflow/NPA/Core/TensorSemiring.h"

#include <mutex>
#include <sstream>

namespace npa {

/// Direct tensor-side differential builder. This exposes a first-class tensor
/// differential API instead of forcing callers to build the ordinary
/// differential and convert it afterwards.
template <class D> struct TensorDiff {
  using V = DomVal<D>;
  using Traits = TensorSemiringTraits<D>;
  using TD = typename Traits::tensor_domain;
  using Map = std::unordered_map<Symbol, V>;
  struct Plan {
    std::vector<typename Exp0<D>::K> preorder;
    bool has_mu = false;
    bool has_project = false;
  };

  static E1<TD> build(const Map &nu, const E0<D> &e) {
    auto plan = get_plan(e);
    if ((*plan).has_mu)
      throw UnsupportedNewtonMuError{};
    if ((*plan).has_project && !domain_project_newton_safe<D>())
      throw UnsafeNewtonProjectError{};
    return aux(nu, e);
  }

private:
  static void describe(const E0<D> &e, Plan &plan, std::ostringstream &signature) {
    if (!e) {
      signature << "N;";
      return;
    }
    plan.preorder.push_back(e->k);
    signature << static_cast<int>(e->k) << ';';
    using K0 = typename Exp0<D>::K;
    switch (e->k) {
    case K0::Project:
      plan.has_project = true;
      describe(e->t, plan, signature);
      break;
    case K0::Mu:
      plan.has_mu = true;
      describe(e->t, plan, signature);
      break;
    case K0::Seq:
    case K0::Call:
    case K0::Star:
      describe(e->t, plan, signature);
      break;
    case K0::Mul:
    case K0::Cond:
    case K0::Ndet:
    case K0::Concat:
      describe(e->t1, plan, signature);
      describe(e->t2, plan, signature);
      break;
    default:
      break;
    }
  }

  static Optional<Plan> get_plan(const E0<D> &e) {
    static std::mutex cache_mu;
    static std::unordered_map<std::string, Plan> cache;

    Plan plan;
    std::ostringstream signature;
    describe(e, plan, signature);
    const std::string key = signature.str();
    {
      std::lock_guard<std::mutex> lock(cache_mu);
      auto it = cache.find(key);
      if (it != cache.end()) {
        Optional<Plan> out;
        out = it->second;
        return out;
      }
    }
    {
      std::lock_guard<std::mutex> lock(cache_mu);
      auto inserted = cache.emplace(key, plan);
      Optional<Plan> out;
      out = inserted.first->second;
      return out;
    }
  }

  static E1<TD> aux(const Map &nu, const E0<D> &o) {
    using K0 = typename Exp0<D>::K;
    switch (o->k) {
    case K0::Term:
      return Exp1<TD>::term(TD::zero());
    case K0::Seq:
      return Exp1<TD>::seqR(aux(nu, o->t), Traits::left_constant(o->c));
    case K0::Mul: {
      assert(o->t1->val.has_value() && o->t2->val.has_value());
      auto lhs = aux(nu, o->t1);
      auto rhs = aux(nu, o->t2);
      return Exp1<TD>::add(
          Exp1<TD>::seqR(lhs, Traits::right_constant(*o->t2->val)),
          Exp1<TD>::seqR(rhs, Traits::left_constant(*o->t1->val)));
    }
    case K0::Call: {
      auto dArg = aux(nu, o->t);
      auto left = Exp1<TD>::seqR(dArg, Traits::left_constant(nu.at(o->sym)));
      assert(o->t->val.has_value());
      auto right =
          Exp1<TD>::seqR(Exp1<TD>::hole(o->sym),
                         Traits::right_constant(*o->t->val));
      return Exp1<TD>::add(left, right);
    }
    case K0::Cond:
      return Exp1<TD>::cond(o->phi, aux(nu, o->t1), aux(nu, o->t2));
    case K0::Ndet:
      return Exp1<TD>::add(aux(nu, o->t1), aux(nu, o->t2));
    case K0::Project:
      return Exp1<TD>::project(aux(nu, o->t));
    case K0::Hole:
      return Exp1<TD>::hole(o->sym);
    case K0::Bound:
      return Exp1<TD>::term(TD::zero());
    case K0::Concat: {
      assert(o->t1->val.has_value() && o->t2->val.has_value());
      V t1_val = *o->t1->val;
      V t2_val = *o->t2->val;
      V nu_x = nu.at(o->sym);
      auto d1 = aux(nu, o->t1);
      auto d2 = aux(nu, o->t2);
      auto term1 = Exp1<TD>::seqR(
          d1, Traits::right_constant(D::extend(nu_x, t2_val)));
      auto term2 =
          Exp1<TD>::seqR(Exp1<TD>::hole(o->sym), Traits::couple(t1_val, t2_val));
      auto term3 = Exp1<TD>::seqR(
          d2, Traits::left_constant(D::extend(t1_val, nu_x)));
      return Exp1<TD>::add(Exp1<TD>::add(term1, term2), term3);
    }
    case K0::Star:
      // TOPLAS 2016, Section 6.2: D^T(g*) = D^T(g) ⊗T ((g(ν)*)^t ⊙ g(ν)*).
      assert(o->val.has_value());
      return Exp1<TD>::seqR(aux(nu, o->t),
                            Traits::couple(*o->val, *o->val));
    case K0::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return nullptr;
  }
};

} // namespace npa

#endif // NPA_TENSOR_DIFF_H
