/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <assert.h>
#include "mkldnn.h"

#include "c_types_map.hpp"
#include "engine.hpp"
#include "primitive.hpp"
#include "primitive_exec_types.hpp"
#include "stream.hpp"
#include "utils.hpp"

using namespace mkldnn::impl;
using namespace mkldnn::impl::status;
using namespace mkldnn::impl::utils;

status_t stream_t::enqueue_primitive(const primitive_t *primitive, const exec_ctx_t &ctx) {
    return primitive->execute(ctx);
}

/* API */

status_t mkldnn_stream_create(stream_t **stream, engine_t *engine,
        unsigned flags) {
    bool args_ok = true
        && !utils::any_null(stream, engine)
        && flags == stream_flags::default_flags;
    if (!args_ok)
        return invalid_arguments;

    return engine->create_stream(stream, flags);
}

status_t mkldnn_stream_wait(stream_t *stream) {
    bool args_ok = !any_null(stream);
    if (!args_ok)
        return invalid_arguments;

    return stream->wait();
}

status_t mkldnn_stream_destroy(stream_t *stream) {
    delete stream;
    return success;
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
