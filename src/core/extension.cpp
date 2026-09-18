// =============================================================================
// src/core/extension.cpp — registration queue for out-of-tree native tools
// =============================================================================

#include "extension.h"
#include <vector>

namespace funes::ext {

namespace {
std::vector<RegisterFn>& queue() {
    static std::vector<RegisterFn> q;
    return q;
}
} // namespace

void register_extension(RegisterFn fn) {
    if (fn) queue().push_back(fn);
}

size_t register_all_extensions(const Services& services) {
    for (RegisterFn fn : queue()) fn(services);
    return queue().size();
}

} // namespace funes::ext
