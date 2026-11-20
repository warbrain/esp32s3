#pragma once

#include <string>
#include <string_view>

namespace hmi::engine
{
// Search application/models, then the nearest recognized HMI project/models.
// Never search other ancestor model directories or a fixed drive.
// HMI_MODEL_DIR overrides the search. Per-model paths remain configurable.
std::string default_model_path(std::string_view filename);
}
