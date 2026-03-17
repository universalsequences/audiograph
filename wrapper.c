#include "graph_engine.h"

// Expose the static inline params_push as a linkable symbol for Rust FFI.
bool params_push_wrapper(LiveGraph *lg, ParamMsg m) {
    return params_push(lg->params, m);
}
