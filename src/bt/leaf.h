#pragma once

#include <string>

#include "node.h"

class Leaf : public Node {
protected:
    Leaf(uint32_t id, std::string type, std::string name)
        : Node(id, std::move(type), std::move(name)) {}
};
