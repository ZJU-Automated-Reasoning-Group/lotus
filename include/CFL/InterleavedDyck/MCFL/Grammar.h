#pragma once

#include "CFL/InterleavedDyck/MCFL/Graph.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lotus::cfl::interleaved_dyck::mcfl {

class Grammar {
public:
  using Nonterminal = std::uint32_t;

  struct NonterminalInfo {
    std::string name;
    std::size_t arity = 0;
  };

  struct BasicRule {
    Nonterminal head = 0;
    Label label;
  };

  struct PrependRule {
    Nonterminal head = 0;
    Nonterminal body = 0;
    Label label;
    std::size_t component = 0;
  };

  struct AppendRule {
    Nonterminal head = 0;
    Nonterminal body = 0;
    Label label;
    std::size_t component = 0;
  };

  struct InsertRule {
    Nonterminal head = 0;
    Nonterminal body = 0;
    Label label;
    std::size_t component = 0;
  };

  struct VariableRef {
    std::size_t body = 0;
    std::size_t component = 0;

    bool operator==(const VariableRef &other) const;
  };

  struct ConcatenateRule {
    Nonterminal head = 0;
    std::vector<Nonterminal> bodies;
    std::vector<std::vector<VariableRef>> outputs;
  };

  Nonterminal addNonterminal(std::string name, std::size_t arity);
  Nonterminal nonterminal(std::string_view name) const;
  bool hasNonterminal(std::string_view name) const;
  const NonterminalInfo &info(Nonterminal nonterminal) const;

  void setStart(Nonterminal start);
  Nonterminal start() const;

  void addBasic(Nonterminal head, Label label);
  void addPrepend(Nonterminal head, Nonterminal body, Label label,
                  std::size_t component);
  void addAppend(Nonterminal head, Nonterminal body, Label label,
                 std::size_t component);
  void addInsert(Nonterminal head, Nonterminal body, Label label,
                 std::size_t component);
  void addConcatenate(Nonterminal head, std::vector<Nonterminal> bodies,
                      std::vector<std::vector<VariableRef>> outputs);

  void validate() const;
  std::size_t dimension() const;
  std::size_t rank() const;

  const std::vector<NonterminalInfo> &nonterminals() const {
    return nonterminals_;
  }
  const std::vector<BasicRule> &basicRules() const { return basic_rules_; }
  const std::vector<PrependRule> &prependRules() const {
    return prepend_rules_;
  }
  const std::vector<AppendRule> &appendRules() const { return append_rules_; }
  const std::vector<InsertRule> &insertRules() const { return insert_rules_; }
  const std::vector<ConcatenateRule> &concatenateRules() const {
    return concatenate_rules_;
  }

private:
  void checkNonterminal(Nonterminal nonterminal) const;

  std::vector<NonterminalInfo> nonterminals_;
  std::unordered_map<std::string, Nonterminal> name_to_nonterminal_;
  Nonterminal start_ = 0;
  bool has_start_ = false;
  std::vector<BasicRule> basic_rules_;
  std::vector<PrependRule> prepend_rules_;
  std::vector<AppendRule> append_rules_;
  std::vector<InsertRule> insert_rules_;
  std::vector<ConcatenateRule> concatenate_rules_;
};

} // namespace lotus::cfl::interleaved_dyck::mcfl
