#pragma once

#include "util/json_reader.hpp"

namespace hmi::util::json
{
// Throws std::invalid_argument for a non-finite number.
std::string stringify(const Value &value);
}
