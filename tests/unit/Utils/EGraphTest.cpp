
#include "Utils/ADT/egraphs.h"
#include <gtest/gtest.h>


enum class NodeKind {
  F, G, H,
  X, Y, Z, A, B, C
};

const char* NODE_KIND_NAMES[] = {
  "F", "G", "H",
  "X", "Y", "Z", "A", "B", "C"
};

std::ostream& operator<<(std::ostream& stream, const NodeKind& node_kind) {
  stream << NODE_KIND_NAMES[(size_t)node_kind];
  return stream;
}

using Node = egraphs::EGraph<NodeKind>::Node;
using EClass = egraphs::EGraph<NodeKind>::EClass;

class EGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }
    
    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(EGraphTest, Hashcons) {
    egraphs::EGraph<NodeKind> e_graph;
    
    EXPECT_EQ(e_graph.node(NodeKind::X), e_graph.node(NodeKind::X));
    EXPECT_NE(e_graph.node(NodeKind::Y), e_graph.node(NodeKind::X));
    
    Node* a = nullptr;
    Node* b = nullptr;
    
    a = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    });
    b = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    });
    EXPECT_EQ(a, b);
    
    a = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    });
    b = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    });
    EXPECT_NE(a, b);
    
    a = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    });
    b = e_graph.node(NodeKind::G, {
      e_graph.node(NodeKind::X)
    });
    EXPECT_NE(a, b);
    
    a = e_graph.node(NodeKind::H, {
      e_graph.node(NodeKind::X),
      e_graph.node(NodeKind::Y)
    });
    b = e_graph.node(NodeKind::H, {
      e_graph.node(NodeKind::X),
      e_graph.node(NodeKind::Y)
    });
    EXPECT_EQ(a, b);
    
    a = e_graph.node(NodeKind::H, {
      e_graph.node(NodeKind::X),
      e_graph.node(NodeKind::Y)
    });
    b = e_graph.node(NodeKind::H, {
      e_graph.node(NodeKind::X)
    });
    EXPECT_NE(a, b);
}

TEST_F(EGraphTest, Transitive) {
    egraphs::EGraph<NodeKind> e_graph;
    
    EXPECT_NE(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Y));
    EXPECT_NE(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Z));
    EXPECT_NE(e_graph.node(NodeKind::Y), e_graph.node(NodeKind::Z));
    
    e_graph.merge(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Y));
    
    EXPECT_EQ(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Y));
    
    e_graph.merge(e_graph.node(NodeKind::Y), e_graph.node(NodeKind::Z));
    
    EXPECT_EQ(e_graph.node(NodeKind::Y), e_graph.node(NodeKind::Z));
    EXPECT_EQ(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Z));
}

TEST_F(EGraphTest, CongruentMergeBefore) {
    egraphs::EGraph<NodeKind> e_graph;
    
    e_graph.merge(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Y));
    
    EXPECT_EQ(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    }), e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    }));
    
    e_graph.merge(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    }), e_graph.node(NodeKind::A));
    e_graph.merge(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    }), e_graph.node(NodeKind::B));
    
    EXPECT_EQ(e_graph.node(NodeKind::A), e_graph.node(NodeKind::B));
}

TEST_F(EGraphTest, CongruentMergeAfter) {
    egraphs::EGraph<NodeKind> e_graph;
    
    EXPECT_NE(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    }), e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    }));
    
    e_graph.merge(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    }), e_graph.node(NodeKind::A));
    e_graph.merge(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    }), e_graph.node(NodeKind::B));
    
    e_graph.merge(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Y));
    
    EXPECT_EQ(e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    }), e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    }));
    
    EXPECT_EQ(e_graph.node(NodeKind::A), e_graph.node(NodeKind::B));
}

TEST_F(EGraphTest, CongruentMergeAfter2Levels) {
    egraphs::EGraph<NodeKind> e_graph;
    
    EXPECT_NE(
      e_graph.node(NodeKind::G, {
        e_graph.node(NodeKind::F, {
          e_graph.node(NodeKind::X)
        })
      }),
      e_graph.node(NodeKind::G, {
        e_graph.node(NodeKind::F, {
          e_graph.node(NodeKind::Y)
        })
      })
    );
    
    e_graph.merge(e_graph.node(NodeKind::G, {
      e_graph.node(NodeKind::F, {
        e_graph.node(NodeKind::X)
      })
    }), e_graph.node(NodeKind::A));
    e_graph.merge(e_graph.node(NodeKind::G, {
      e_graph.node(NodeKind::F, {
        e_graph.node(NodeKind::Y)
      })
    }), e_graph.node(NodeKind::B));
    
    e_graph.merge(e_graph.node(NodeKind::X), e_graph.node(NodeKind::Y));
    
    EXPECT_EQ(
      e_graph.node(NodeKind::G, {
        e_graph.node(NodeKind::F, {
          e_graph.node(NodeKind::X)
        })
      }),
      e_graph.node(NodeKind::G, {
        e_graph.node(NodeKind::F, {
          e_graph.node(NodeKind::Y)
        })
      })
    );
    
    EXPECT_EQ(e_graph.node(NodeKind::A), e_graph.node(NodeKind::B));
}

TEST_F(EGraphTest, Match) {
    egraphs::EGraph<NodeKind> e_graph;
    
    Node* a = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    });
    
    Node* b = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    });
    
    Node* c = e_graph.node(NodeKind::G, {
      e_graph.node(NodeKind::X)
    });
    
    e_graph.merge(a, b);
    e_graph.merge(a, c);
    
    auto check_matches = [&](Node* node, const NodeKind& kind, size_t expected_count){
      size_t count = 0;
      for (Node* match : node->e_class().match(kind)) {
        EXPECT_EQ(match->data(), kind);
        count++;
      }
      EXPECT_EQ(count, expected_count);
    };
    
    check_matches(a, NodeKind::F, 2);
    check_matches(b, NodeKind::F, 2);
    check_matches(c, NodeKind::F, 2);
    
    check_matches(a, NodeKind::G, 1);
    check_matches(b, NodeKind::G, 1);
    check_matches(c, NodeKind::G, 1);
    
    check_matches(a, NodeKind::X, 0);
    check_matches(b, NodeKind::X, 0);
    check_matches(c, NodeKind::X, 0);
}

TEST_F(EGraphTest, MatchCollect) {
    egraphs::EGraph<NodeKind> e_graph;
    
    Node* a = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::X)
    });
    
    Node* b = e_graph.node(NodeKind::F, {
      e_graph.node(NodeKind::Y)
    });
    
    Node* c = e_graph.node(NodeKind::G, {
      e_graph.node(NodeKind::X)
    });
    
    e_graph.merge(a, b);
    e_graph.merge(a, c);
    
    auto check_matches = [&](Node* node, const NodeKind& kind, size_t expected_count){
      std::vector<Node*> matches;
      matches.insert(matches.end(),
        node->e_class().match(kind).begin(),
        node->e_class().match(kind).end()
      );
      
      EXPECT_EQ(matches.size(), expected_count);
      for (Node* match : matches) {
        EXPECT_EQ(match->data(), kind);
      }
    };
    
    check_matches(a, NodeKind::F, 2);
    check_matches(b, NodeKind::F, 2);
    check_matches(c, NodeKind::F, 2);
    
    check_matches(a, NodeKind::G, 1);
    check_matches(b, NodeKind::G, 1);
    check_matches(c, NodeKind::G, 1);
    
    check_matches(a, NodeKind::X, 0);
    check_matches(b, NodeKind::X, 0);
    check_matches(c, NodeKind::X, 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}