#include "dependency_fixture.hpp"

tower::Result<void> add_shared(tower::Dependencies& dependencies) {
    auto inserted = dependencies.insert(SharedDependency{29});
    if (!inserted) {
        return inserted.as_failure();
    }
    return tower::success();
}
