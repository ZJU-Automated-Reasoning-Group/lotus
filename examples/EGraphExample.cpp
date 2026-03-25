// Copyright 2024 Can Joshua Lehmann
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>

#include "Utils/ADT/egraphs.h"

enum class NodeKind {
  Constant, Variable,
  And, Or, Not
};

const char* NODE_KIND_NAMES[] = {
  "Constant", "Variable",
  "And", "Or", "Not"
};

std::ostream& operator<<(std::ostream& stream, const NodeKind& node_kind) {
  stream << NODE_KIND_NAMES[(size_t)node_kind];
  return stream;
}

class NodeData {
private:
  NodeKind _kind;
  bool _bool_value;
  std::string _str_value;
public:
  NodeData(const NodeKind& kind): _kind(kind), _bool_value(false) {}
  NodeData(bool value): _kind(NodeKind::Constant), _bool_value(value) {}
  NodeData(const std::string& value): _kind(NodeKind::Variable), _bool_value(false), _str_value(value) {}
  NodeData(const char* value): _kind(NodeKind::Variable), _bool_value(false), _str_value(std::string(value)) {}
  
  NodeKind kind() const { return _kind; }
  
  inline bool constant() const { return _bool_value; }
  inline std::string variable() const { return _str_value; }
  
  bool operator==(const NodeData& other) const {
    if (_kind == other._kind) {
      switch (_kind) {
        case NodeKind::Constant:
          return _bool_value == other._bool_value;
        case NodeKind::Variable:
          return _str_value == other._str_value;
        default: return true;
      }
    }
    return false;
  }
  
  bool operator!=(const NodeData& other) const { return !(*this == other); }
};

template <>
struct std::hash<NodeData> {
  size_t operator()(const NodeData& data) const {
    size_t hash = std::hash<NodeKind>()(data.kind());
    switch (data.kind()) {
      case NodeKind::Constant:
        hash ^= std::hash<bool>()(data.constant()) << 17;
      break;
      case NodeKind::Variable:
        hash ^= std::hash<std::string>()(data.variable());
      break;
      default: break;
    }
    return hash;
  }
};

std::ostream& operator<<(std::ostream& stream, const NodeData& data) {
  switch (data.kind()) {
    case NodeKind::Constant: stream << (data.constant() ? "true" : "false"); break;
    case NodeKind::Variable: stream << data.variable(); break;
    default: stream << data.kind(); break;
  }
  return stream;
}

int main() {
  using EGraph = egraphs::EGraph<NodeKind, NodeData>;
  using Node = EGraph::Node;
  using EClass = EGraph::EClass;
  
  EGraph e_graph;
  
  Node* extraction_root = e_graph.node(NodeKind::Not, {
    e_graph.node(NodeKind::And, {
      e_graph.node("y"),
      e_graph.node(NodeKind::Not, {
        e_graph.node("x")
      })
    })
  });
  
  EGraph::MergeQueue queue;
  do {
    for (Node* root : e_graph.roots()) {
      for (Node* node : root->e_class()) {
        switch (node->data().kind()) {
          case NodeKind::Not:
            for (Node* and_node : node->at(0)->e_class().match(NodeKind::And)) {
              queue.merge(
                node,
                e_graph.node(NodeKind::Or, {
                  e_graph.node(NodeKind::Not, {and_node->at(0)}),
                  e_graph.node(NodeKind::Not, {and_node->at(1)})
                })
              );
            }
            for (Node* or_node : node->at(0)->e_class().match(NodeKind::Or)) {
              queue.merge(
                node,
                e_graph.node(NodeKind::And, {
                  e_graph.node(NodeKind::Not, {or_node->at(0)}),
                  e_graph.node(NodeKind::Not, {or_node->at(1)})
                })
              );
            }
            for (Node* not_node : node->at(0)->e_class().match(NodeKind::Not)) {
              queue.merge(
                node,
                not_node->at(0)
              );
            }
            for (Node* constant_node : node->at(0)->e_class().match(NodeKind::Constant)) {
              queue.merge(
                node,
                e_graph.node(!constant_node->data().constant())
              );
            }
          break;
          case NodeKind::And:
            queue.merge(node, e_graph.node(NodeKind::And, {
              node->at(1),
              node->at(0)
            }));
            if (node->at(0)->e_class().match(NodeData(false)).not_empty()) {
              queue.merge(node, e_graph.node(false));
            }
            if (node->at(0)->e_class().match(NodeData(true)).not_empty()) {
              queue.merge(node, node->at(1));
            }
            if (node->at(0) == node->at(1)) {
              queue.merge(node, node->at(0));
            }
            for (Node* not_node : node->at(0)->e_class().match(NodeKind::Not)) {
              if (not_node->at(0) == node->at(1)) {
                queue.merge(node, e_graph.node(false));
              }
            }
          break;
          case NodeKind::Or:
            queue.merge(node, e_graph.node(NodeKind::Or, {
              node->at(1),
              node->at(0)
            }));
            if (node->at(0)->e_class().match(NodeData(true)).not_empty() ) {
              queue.merge(node, e_graph.node(true));
            }
            if (node->at(0)->e_class().match(NodeData(false)).not_empty()) {
              queue.merge(node, node->at(1));
            }
            if (node->at(0) == node->at(1)) {
              queue.merge(node, node->at(0));
            }
            for (Node* not_node : node->at(0)->e_class().match(NodeKind::Not)) {
              if (not_node->at(0) == node->at(1)) {
                queue.merge(node, e_graph.node(true));
              }
            }
          break;
        }
      }
    }
    std::cout << queue.size() << std::endl;
  } while(e_graph.merge(queue));
  
  e_graph.save_dot("graph.gv");
  
  EGraph::Extracted extracted = e_graph.extract();
  e_graph.save_dot("extracted.gv", extracted, extraction_root);
  
  return 0;
}