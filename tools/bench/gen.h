#pragma once
#include "common.h"

namespace bench {

const std::vector<std::string>& GeneratorNames();
// Generates the named synthetic ground-truth pattern at w x h. Returns false for an unknown name.
bool Generate(const std::string& name, uint32_t w, uint32_t h, Image& out);

} // namespace bench
