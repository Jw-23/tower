#pragma once

#include <tower/dependencies.hpp>

struct SharedDependency { int value; };

tower::Result<void> add_shared(tower::Dependencies& dependencies);
