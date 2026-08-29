#pragma once

#include "Solvers/Datalog/Core/Program.h"

#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {

template <typename... Ts>
std::set<std::tuple<Ts...>>
asSet(const lotus::datalog::Relation<Ts...> &relation) {
  const auto rows = relation.rows();
  return {rows.begin(), rows.end()};
}

} // namespace
