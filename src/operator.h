#ifndef OPERATOR_H
#define OPERATOR_H

#include "value.h"

#include <vector>
#include <string>

struct Tuple {
    std::vector<Value> values; // reused between rows, sized once
};

class Operator {
public:
    virtual ~Operator() {}
    virtual bool open() = 0;
    virtual bool next(Tuple& out) = 0; // false at end
    virtual void close() = 0;
    virtual const std::vector<std::string>& columnNames() const = 0;
};

#endif // OPERATOR_H
