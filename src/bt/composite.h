#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "node.h"

class Composite : public Node {
public:
    void AddChild(std::unique_ptr<Node> child);

    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    bool is_mid_sequence() const { return current_child_index_ > 0; }
    size_t current_child_index() const { return current_child_index_; }

    void Reset() override;
    void OnAborted() override;

protected:
    Composite(uint32_t id, std::string type, std::string name);

    std::vector<std::unique_ptr<Node>> children_;
    size_t current_child_index_ = 0;
};
