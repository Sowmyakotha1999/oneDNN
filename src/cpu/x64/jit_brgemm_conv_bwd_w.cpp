/*******************************************************************************
* Copyright 2022 Intel Corporation
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

#include "common/c_types_map.hpp"
#include "common/dnnl_thread.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"
#include "cpu/cpu_primitive.hpp"
#include "cpu/x64/injectors/jit_uni_binary_injector.hpp"

#include "cpu/x64/jit_brgemm_conv_bwd_w.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

using namespace dnnl::impl::status;
using namespace dnnl::impl::memory_tracking::names;
using namespace dnnl::impl::utils;

using namespace nstl;
using namespace data_type;

#define wht_blk_off(d, g, ...) \
    (pd()->with_groups() ? (d).blk_off((g), __VA_ARGS__) \
                         : (d).blk_off(__VA_ARGS__))

status_t brgemm_convolution_bwd_weights_t::pd_t::init(engine_t *engine) {
    bool ok = true && is_bwd_w()
            && set_default_alg_kind(alg_kind::convolution_direct)
            && (expect_data_types(data_type::bf16, data_type::bf16,
                        data_type::undef, data_type::bf16, data_type::undef)
                    || expect_data_types(data_type::bf16, data_type::f32,
                            data_type::undef, data_type::bf16,
                            data_type::undef))
            && IMPLICATION(with_bias(),
                    utils::one_of(diff_bias_md_.data_type, data_type::f32,
                            data_type::bf16))
            && attr()->has_default_values() && !has_zero_dim_memory();
    if (!ok) return status::unimplemented;

    auto scratchpad = scratchpad_registry().registrar();

    status_t status = brgemm_convolution_utils::init_conf_bwd_w(jcp_, *desc(),
            src_md_, diff_weights_md_, diff_bias_md_, diff_dst_md_, attr_,
            dnnl_get_max_threads());
    if (status != status::success) return status;

    status = brgemm_convolution_utils::init_scratchpad_bwd_w(
            scratchpad, jcp_, src_md_, diff_weights_md_, diff_dst_md_);

    if (status != status::success) return status;
    copy2jit_jcp();

    assert(jcp_.var_bs);
    bs_c = jcp_.var_bs ? 1 : (jcp_.max_batch + 1);
    batchsizes.resize(bs_c + 1);
    for (int i = 0; i <= bs_c; i++)
        batchsizes[i] = -1;

    batchsizes[1] = 0;

    const auto adj_M = nstl::max(jcp_.M, jcp_.M_tail);
    brgs_sz_ = bs_c * (adj_M + 1) * 2 * 2 * 2;
    brgs_.resize(brgs_sz_);
    bd_masks.resize(brgs_sz_);

    const float alpha = 1.0;
    const float beta = 1.0;

    int M_begin = 0;
    int M_end = (jcp_.M_tail == jcp_.M) ? 1 : 2;
    int N_begin = 0;
    int N_end = (jcp_.N_tail == jcp_.N) ? 1 : 2;
    int K_begin = 0;
    int K_end = (jcp_.K_tail == jcp_.K) ? 1 : 2;
    int init_begin = 0;
    int init_end = 2;

    const auto src_type = data_type::bf16;
    const auto wei_type = data_type::bf16;

    for (int i = M_begin; i < M_end; i++) {
        auto M = (i) ? jcp_.M_tail : jcp_.M;
        if (M <= 0) continue;
        // init only needed brgemm descriptors
        for (int bs = 0; bs <= jcp_.max_batch; bs++) {
            if (batchsizes[bs] == -1) continue;
            for_(int i_init = init_begin; i_init < init_end; i_init++)
            for_(int i_N = N_begin; i_N < N_end; i_N++)
            for (int i_K = K_begin; i_K < K_end; i_K++) {
                auto vbeta = (i_init) ? 0 : beta;
                auto vN = (i_N) ? jcp_.N_tail : jcp_.N;
                auto vK = (i_K) ? jcp_.K_tail : jcp_.K;
                if (vN == 0 || vK == 0) continue;
                auto brg_idx = get_brg_idx(bs, M, i_init, i_N, i_K);
                // if brgemm_t already created then skip this iteration
                if (brgs_[brg_idx] != nullptr) continue;
                brgs_[brg_idx] = std::make_shared<brgemm_t>();
                brgemm_t *brg = brgs_[brg_idx].get();
                CHECK(brgemm_desc_init(brg, isa, jcp_.brg_type, src_type,
                        wei_type, false, false, brgemm_row_major, alpha, vbeta,
                        jcp_.LDA, jcp_.LDB, jcp_.LDC, M, vN, vK, nullptr));

                brgemm_attr_t brgattr;
                brgattr.use_uker = jcp_.use_uker;
                brgattr.var_bs = jcp_.var_bs;
                brgattr.use_interleave_stores = jcp_.use_interleave_stores;
                brgattr.hint_prefetching = jcp_.hint_prefetching;
                brgattr.max_bs = bs;
                brgattr.hint_innermost_loop = jcp_.brgemm_bd_loop_innermost
                        ? brgemm_bd_loop_innermost
                        : brgemm_ld_loop_innermost;

                brgattr.hint_expected_A_size = 0;
                brgattr.hint_expected_B_size = 0;
                brgattr.hint_expected_C_size = 0;

                brgattr.wary_tail_read = false;
                brgattr.bd_mask_level = jcp_.use_M_mask;

                brgattr.max_top_vpad = 0;
                brgattr.max_bottom_vpad = 0;

                brgattr.LDA2 = jcp_.tr_iw * jcp_.ih * jcp_.id;
                brgattr.LDB2 = jcp_.tr_ow * jcp_.oc_block * jcp_.oh * jcp_.od;
                brgattr.LDC2_M = jcp_.oc_block * jcp_.kd * jcp_.kh * jcp_.kw;
                brgattr.LDC2_N = jcp_.nb_ic * jcp_.ic_block * jcp_.oc_block
                        * jcp_.kd * jcp_.kh * jcp_.kw;

                CHECK(brgemm_desc_set_attr(brg, brgattr));
            }
        }
    }
    return status;
}

// jit_jcp used to initialize transpose kernels shared with jit implementation
void brgemm_convolution_bwd_weights_t::pd_t::copy2jit_jcp() {
    jit_jcp_ = zero<decltype(jit_jcp_)>();
    jit_jcp_.prop_kind = jcp_.prop_kind;
    jit_jcp_.has_vnni = jcp_.has_vnni;
    jit_jcp_.harness = jcp_.harness;
    jit_jcp_.simd_w = jcp_.simd_w;
    jit_jcp_.ndims = jcp_.ndims;
    jit_jcp_.mb = jcp_.mb;
    jit_jcp_.ngroups = jcp_.ngroups;
    jit_jcp_.ic = jcp_.ic;
    jit_jcp_.oc = jcp_.oc;
    jit_jcp_.oc_without_padding = jcp_.oc;
    jit_jcp_.ic_without_padding = jcp_.ic_without_padding;
    jit_jcp_.id = jcp_.id;
    jit_jcp_.ih = jcp_.ih;
    jit_jcp_.iw = jcp_.iw;
    jit_jcp_.od = jcp_.od;
    jit_jcp_.oh = jcp_.oh;
    jit_jcp_.ow = jcp_.ow;
    jit_jcp_.f_pad = jcp_.f_pad;
    jit_jcp_.l_pad = jcp_.l_pad;
    jit_jcp_.t_pad = jcp_.t_pad;
    jit_jcp_.back_pad = jcp_.back_pad;
    jit_jcp_.r_pad = jcp_.r_pad;
    jit_jcp_.b_pad = jcp_.b_pad;
    jit_jcp_.kd = jcp_.kd;
    jit_jcp_.kh = jcp_.kh;
    jit_jcp_.kw = jcp_.kw;
    jit_jcp_.stride_d = jcp_.stride_d;
    jit_jcp_.stride_h = jcp_.stride_h;
    jit_jcp_.stride_w = jcp_.stride_w;
    jit_jcp_.dilate_d = jcp_.dilate_d;
    jit_jcp_.dilate_h = jcp_.dilate_h;
    jit_jcp_.dilate_w = jcp_.dilate_w;
    jit_jcp_.src_tag = jcp_.src_tag;
    jit_jcp_.wei_tag = jcp_.wei_tag;
    jit_jcp_.dst_tag = jcp_.dst_tag;
    jit_jcp_.with_bias = jcp_.with_bias;
    jit_jcp_.with_sum = jcp_.with_sum;
    jit_jcp_.with_eltwise = jcp_.with_eltwise;
    jit_jcp_.with_binary = jcp_.with_binary;
    jit_jcp_.is_fused_conv = jcp_.is_fused_conv;
    jit_jcp_.nb_ic = jcp_.nb_ic;
    jit_jcp_.ic_block = jcp_.ic_block;
    jit_jcp_.nb_oc = jcp_.nb_oc;
    jit_jcp_.oc_block = jcp_.oc_block;
    jit_jcp_.nb_oc_blocking = jcp_.nb_oc_blocking;

    jit_jcp_.ic_tail = jcp_.ic_tail;
    jit_jcp_.oc_tail = jcp_.oc_tail;

    jit_jcp_.tr_iw = jcp_.tr_iw;
    jit_jcp_.tr_ow = jcp_.tr_ow;
    jit_jcp_.tr_diff_dst_buf_size = jcp_.tr_diff_dst_buf_size;
    jit_jcp_.typesize_in = jcp_.typesize_in;
    jit_jcp_.typesize_out = jcp_.typesize_out;
}

status_t brgemm_convolution_bwd_weights_t::add_brg_kernel(
        int bs, int M, int i_N, int i_K, int i_init) {
    if (M <= 0) return status::success;
    const auto _pd = pd();
    const auto &jcp = _pd->jcp_;
    const auto &brgs = _pd->brgs_;

    auto N = (i_N) ? jcp.N_tail : jcp.N;
    auto K = (i_K) ? jcp.K_tail : jcp.K;
    if (N <= 0 || K <= 0) return status::success;
    auto brg_idx = _pd->get_brg_idx(bs, M, i_init, i_N, i_K);
    auto brg = brgs[brg_idx];
    if (!brg_kernels_[brg_idx] && brg && brg->bcast_dim > 0 && brg->load_dim > 0
            && brg->reduce_dim > 0) {
        brgemm_kernel_t *brg_kernel = nullptr;
        CHECK(brgemm_kernel_create(&brg_kernel, *brg));
        CHECK(safe_ptr_assign(brg_kernels_[brg_idx], brg_kernel));
        CHECK(brgemm_init_tiles(*brg, &brg_kernel_palettes_[brg_idx].a[0]));
    }
    return status::success;
}

status_t brgemm_convolution_bwd_weights_t::init(engine_t *engine) {
    const auto _pd = pd();
    const auto &jcp = _pd->jcp_;
    const auto &jit_jcp = pd()->jit_jcp_;

    CHECK(safe_ptr_assign(trans_kernel_, create_trans_src(&jit_jcp)));
    CHECK(trans_kernel_->create_kernel());
    CHECK(safe_ptr_assign(trans_dst_kernel_, create_trans_dst(&jit_jcp)));
    CHECK(trans_dst_kernel_->create_kernel());

    if (jcp.with_bias) {
        CHECK(safe_ptr_assign(diff_bias_kernel_,
                new jit_avx512_core_amx_bwd_bias_kernel_t(jit_jcp)));
        CHECK(diff_bias_kernel_->create_kernel());
    }

    if (jcp.nthr_mb > 1) {
        CHECK(safe_ptr_assign(
                acc_ker_, new cpu_accumulator_1d_t<data_type::f32>()));
        CHECK(acc_ker_->create_kernel());
    }
    if (jcp.transform_to_vnni) {
        CHECK(safe_ptr_assign(diff_wei_trans_kernel_,
                new jit_diff_wei_trans_to_vnni_t(
                        jcp.kd, jcp.kh, jcp.kw, jcp.ic_block, jcp.oc_block)));
        CHECK(diff_wei_trans_kernel_->create_kernel());
    }

    brg_kernels_.resize(_pd->brgs_sz_);
    brg_kernel_palettes_.resize(_pd->brgs_sz_);

    for (int i = 0; i < _pd->brgs_sz_; i++)
        brg_kernels_[i] = nullptr;

    int M_begin = 0;
    int M_end = (jcp.M_tail == jcp.M) ? 1 : 2;
    int N_begin = 0;
    int N_end = (jcp.N_tail == jcp.N) ? 1 : 2;
    int K_begin = 0;
    int K_end = (jcp.K_tail == jcp.K) ? 1 : 2;
    int init_begin = 0;
    int init_end = 2;

    for (int bs = 0; bs <= jcp.max_batch; bs++) {
        if (_pd->batchsizes[bs] == -1) continue;

        for_(int i_N = N_begin; i_N < N_end; i_N++)
        for_(int i_M = M_begin; i_M < M_end; i_M++)
        for_(int i_init = init_begin; i_init < init_end; i_init++)
        for (int i_K = K_begin; i_K < K_end; i_K++) {
            auto M = (i_M) ? jcp.M_tail : jcp.M;
            if (M <= 0) continue;
            add_brg_kernel(bs, M, i_N, i_K, i_init);
        }
    }

    return status::success;
}

struct brgemm_convolution_bwd_weights_t::thread_info_t {
    const src_data_t *src = nullptr;
    const diff_dst_data_t *diff_dst = nullptr;
    const void *diff_weights = nullptr;
    const void *diff_bias = nullptr;

    const brgemm_convolution_bwd_weights_t *self;
    const memory_tracking::grantor_t scratchpad;

    src_data_t *tr_src = nullptr;
    diff_dst_data_t *tr_diff_dst = nullptr;
    simple_barrier::ctx_t *tr_src_bctx = nullptr;
    simple_barrier::ctx_t *tr_diff_dst_bctx = nullptr;

    float *wei_bia_reduction = nullptr;
    float *bia_reduction = nullptr;
    simple_barrier::ctx_t *wei_bia_reduction_bctx = nullptr;

    // All nthreads are mapped to a multidimensional "cube" with sizes:
    // (nthr_mb, nthr_g, nthr_oc, nthr_ic).
    // Variables ithr_* define the coordinates and "layers" of the current
    // thread in this "cube"
    int ithr = 0;
    int ithr_ic_b = 0, ithr_oc_b = 0, ithr_g = 0, ithr_mb = 0;
    int ithr_but_oc = 0;
    int ithr_but_ic = 0;

    int img_start = 0, img_end = 0, img_work = 0;
    int g_start = 0, g_end = 0, g_work = 0;
    int oc_b_start = 0, oc_b_end = 0, oc_b_work = 0;
    int ic_b_start = 0, ic_b_end = 0, ic_b_work = 0;

    S_t cur_palette;
    brgemm_batch_element_t *__restrict brg_batch;
    char *wsp_tile;
    const exec_ctx_t &exec_ctx;
    const jit_brgemm_conv_conf_t &jcp;
    const memory_desc_wrapper src_d;
    const memory_desc_wrapper diff_dst_d;
    const memory_desc_wrapper diff_weights_d;

    thread_info_t(const brgemm_convolution_bwd_weights_t *pcnv,
            const exec_ctx_t &ctx, int ithr)
        : self(pcnv)
        , scratchpad(ctx.get_scratchpad_grantor())
        , ithr(ithr)
        , exec_ctx(ctx)
        , jcp(self->pd()->jcp_)
        , src_d(self->pd()->src_md())
        , diff_dst_d(self->pd()->diff_dst_md())
        , diff_weights_d(self->pd()->diff_weights_md(0)) {
        diff_dst = CTX_IN_MEM(const diff_dst_data_t *, DNNL_ARG_DIFF_DST);
        src = CTX_IN_MEM(const src_data_t *, DNNL_ARG_SRC);
        diff_weights = CTX_OUT_MEM(void *, DNNL_ARG_DIFF_WEIGHTS);

        diff_bias = self->pd()->with_bias() && (jcp.oc % jcp.oc_block != 0)
                        && self->pd()->jcp_.bia_dt == data_type::f32
                ? (void *)scratchpad.template get<float>(key_conv_padded_bias)
                : CTX_OUT_MEM(void *, DNNL_ARG_DIFF_BIAS);

        tr_src = scratchpad.template get<src_data_t>(key_conv_tr_src);
        if (jcp.global_transpose)
            tr_src_bctx = scratchpad.template get<simple_barrier::ctx_t>(
                    key_conv_tr_src_bctx);

        tr_diff_dst = scratchpad.template get<diff_dst_data_t>(
                key_conv_tr_diff_dst);
        if (jcp.global_transpose)
            tr_diff_dst_bctx = scratchpad.template get<simple_barrier::ctx_t>(
                    key_conv_tr_diff_dst_bctx);
        wei_bia_reduction
                = scratchpad.template get<float>(key_conv_wei_bia_reduction);
        bia_reduction = nullptr;
        if (jcp.with_bias) {
            const size_t wei_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block
                    * jcp.nb_ic * jcp.ic_block * jcp.kh * jcp.kw * jcp.kd;
            const int num_wei_buffers = jcp.wei_dt == data_type::bf16
                    ? jcp.nthr_mb
                    : jcp.nthr_mb - 1;
            bia_reduction = wei_bia_reduction + wei_size * num_wei_buffers;
        }

        wei_bia_reduction_bctx = scratchpad.template get<simple_barrier::ctx_t>(
                key_conv_wei_bia_reduction_bctx);

        ithr_ic_b = ithr % jcp.nthr_ic_b;
        ithr_oc_b = ithr / jcp.nthr_ic_b % jcp.nthr_oc_b;
        ithr_g = ithr / jcp.nthr_ic_b / jcp.nthr_oc_b % jcp.nthr_g;
        ithr_mb = ithr / jcp.nthr_ic_b / jcp.nthr_oc_b / jcp.nthr_g;

        ithr_but_oc
                = (ithr_mb * jcp.nthr_g + ithr_g) * jcp.nthr_ic_b + ithr_ic_b;

        ithr_but_ic
                = (ithr_mb * jcp.nthr_g + ithr_g) * jcp.nthr_oc_b + ithr_oc_b;

        int work_amount = jcp.nthr_mb_work;
        /* reduction dimension */
        balance211(work_amount, jcp.nthr_mb, ithr_mb, img_start, img_end);
        img_work = img_end - img_start;

        /* independent dimensions */
        balance211(jcp.ngroups, jcp.nthr_g, ithr_g, g_start, g_end);
        g_work = g_end - g_start;

        balance211(jcp.nb_oc, jcp.nthr_oc_b, ithr_oc_b, oc_b_start, oc_b_end);
        oc_b_work = oc_b_end - oc_b_start;

        balance211(jcp.nb_ic, jcp.nthr_ic_b, ithr_ic_b, ic_b_start, ic_b_end);
        if (jcp.transform_to_vnni) {
            if (ic_b_start % 2 != 0) ic_b_start++;
            if (ic_b_end != jcp.nb_ic && ic_b_end % 2 != 0) ic_b_end++;
        }
        ic_b_work = ic_b_end - ic_b_start;

        std::memset(cur_palette.a, 0, AMX_PALETTE_SIZE);
        brgemm_batch_element_t *const __restrict brg_batch_global
                = (jcp.brg_type == brgemm_strd)
                ? nullptr
                : scratchpad.template get<brgemm_batch_element_t>(
                        key_brgemm_primitive_batch);
        brg_batch = brg_batch_global
                + static_cast<size_t>(ithr) * jcp.adjusted_batch_size;

        auto wsp_tile_global
                = scratchpad.template get<char>(key_conv_amx_tile_buffer);
        wsp_tile = wsp_tile_global + ithr * 2 * brgemm_convolution_utils::P4K;
    }

    size_t tr_src_buf_number(int g, int ic) const {
        return jcp.global_transpose
                ? ithr_mb * jcp.nb_ic * jcp.ngroups + g * jcp.nb_ic + ic
                : ithr;
    }

    size_t tr_diff_dst_buf_number(int g, int oc) const {
        return jcp.global_transpose
                ? ithr_mb * jcp.nb_oc * jcp.ngroups + g * jcp.nb_oc + oc
                : ithr;
    }

    size_t tr_src_off(
            int g, int ic, int id_end, int id, int ih_end, int ih) const {
        const size_t tr_row_size = jcp.tr_iw * jcp.ic_block;
        const size_t tr_3d_size = tr_row_size * jcp.ih;
        int adj = (jcp.global_transpose) ? 1 : jcp.nb_ic_blocking;
        // Aligned to buffer end to use guard elements
        return tr_src_buf_number(g, ic) * adj * jcp.tr_src_buf_size
                + (jcp.id - id_end + id) * tr_3d_size
                + (jcp.ih - ih_end + ih) * tr_row_size;
    }

    size_t tr_diff_dst_off(int g, int oc, int od, int oh) const {
        const size_t tr_row_size = jcp.tr_ow * jcp.oc_block;
        const size_t tr_3d_size = tr_row_size * jcp.oh;
        int adj = (jcp.global_transpose) ? 1 : jcp.nb_oc_blocking;
        return tr_diff_dst_buf_number(g, oc) * adj * jcp.tr_diff_dst_buf_size
                + od * tr_3d_size + oh * tr_row_size;
    }

    void trans_src_nxc(src_data_t *tr_src, const src_data_t *src_base,
            int spatial_start, dim_t spatial_start_offset, int icb_start,
            dim_t chb_stride, int row_count) const {
        const int src_stride = jcp.iw * jcp.ngroups * jcp.ic;
        const int tr_src_stride = jcp.tr_iw * jcp.ic_block;

        int work_rest = row_count;
        int max_spatial_work = jcp.id * jcp.ih;
        int sp_work = nstl::min(work_rest, max_spatial_work - spatial_start);
        const src_data_t *src = src_base + spatial_start_offset;
        int icb = 0;
        const int ic_tail_work = jcp.ic_tail ? jcp.ic_tail : jcp.ic_block;
        while (work_rest > 0) {
            for (int iwork = 0; iwork < sp_work; iwork++) {
                auto ctx = jit_trans_src_t::ctx_t();
                ctx.src = src;
                ctx.tr_src = tr_src;
                assert(icb_start + icb < jcp.nb_ic);
                ctx.ch_work = (icb_start + icb + 1) == jcp.nb_ic ? ic_tail_work
                                                                 : jcp.ic_block;
                ctx.src_prf = nullptr;
                ctx.tr_src_prf = nullptr;
                (*self->trans_kernel_)(&ctx);
                src += src_stride;
                tr_src += tr_src_stride;
            }
            work_rest -= sp_work;
            sp_work = nstl::min(work_rest, max_spatial_work);
            icb++;
            src = src_base + icb * chb_stride;
        }
    }

    void trans_dst_nxc(diff_dst_data_t *tr_diff_dst,
            const diff_dst_data_t *diff_dst_base, int spatial_start,
            dim_t spatial_start_offset, int ocb_start, dim_t chb_stride,
            int row_count) const {
        const int diff_dst_stride = jcp.ow * jcp.ngroups * jcp.oc;
        const int tr_diff_dst_stride = jcp.tr_ow * jcp.oc_block;
        int work_rest = row_count;
        int max_spatial_work = jcp.od * jcp.oh;
        int sp_work = nstl::min(work_rest, max_spatial_work - spatial_start);
        const src_data_t *diff_dst = diff_dst_base + spatial_start_offset;
        int ocb = 0;
        const int oc_tail_work = jcp.oc_tail ? jcp.oc_tail : jcp.oc_block;
        while (work_rest > 0) {
            for (int iwork = 0; iwork < sp_work; iwork++) {
                auto ctx = jit_trans_dst_t::ctx_t();
                ctx.src = diff_dst;
                ctx.tr_src = tr_diff_dst;
                assert(ocb_start + ocb < jcp.nb_oc);
                ctx.ch_work = (ocb_start + ocb + 1) == jcp.nb_oc ? oc_tail_work
                                                                 : jcp.oc_block;
                ctx.src_prf = nullptr;
                ctx.tr_src_prf = nullptr;
                (*self->trans_dst_kernel_)(&ctx);
                diff_dst += diff_dst_stride;
                tr_diff_dst += tr_diff_dst_stride;
            }
            work_rest -= sp_work;
            sp_work = nstl::min(work_rest, max_spatial_work);
            ocb++;
            diff_dst = diff_dst_base + ocb * chb_stride;
        }
    }

    void maybe_global_transpose(
            int img, int isp_s, int isp_e, int osp_s, int osp_e) const {
        if (!jcp.global_transpose) return;

        using simple_barrier::barrier;
        int work_amount = g_work * ic_b_work * (isp_e - isp_s);
        int tr_start {0}, tr_end {0};
        balance211(work_amount, jcp.nthr_oc_b, ithr_oc_b, tr_start, tr_end);

        int g {0}, ic_b {0}, j {0};
        nd_iterator_init(
                tr_start, g, g_work, ic_b, ic_b_work, j, isp_e - isp_s);

        if (jcp.nthr_oc_b > 1)
            barrier(&tr_src_bctx[ithr_but_oc], jcp.nthr_oc_b);
        while (tr_start < tr_end) {
            int g_ = g + g_start;
            int ic_b_ = ic_b + ic_b_start;
            int j_s = j + isp_s;
            int j_e = j_s + nstl::min(tr_end - tr_start, isp_e - j_s);
            const int ic_off_idx = g_ * jcp.ic + ic_b_ * jcp.ic_block;
            const src_data_t *p_src = &src[src_d.blk_off(img, ic_off_idx, j_s)];

            src_data_t *p_tr_src {nullptr};
            if (jcp.harness == harness_2d_reduction) {
                p_tr_src = &tr_src[tr_src_off(g_, ic_b_, 1, 0, isp_e, j_s)];
                trans_src_nxc(p_tr_src, p_src, 0, 0, ic_b_, 0, j_e - j_s);
            } else if (jcp.harness == harness_3d_reduction) {
                p_tr_src
                        = &tr_src[tr_src_off(g_, ic_b_, isp_e, j_s, jcp.ih, 0)];
                trans_src_nxc(
                        p_tr_src, p_src, 0, 0, ic_b_, 0, (j_e - j_s) * jcp.ih);
            } else
                assert(!"Invalid harness type");

            nd_iterator_jump(tr_start, tr_end, g, g_work, ic_b, ic_b_work, j,
                    isp_e - isp_s);
        }
        if (jcp.nthr_oc_b > 1)
            barrier(&tr_src_bctx[ithr_but_oc], jcp.nthr_oc_b);

        j = 0;
        work_amount = g_work * oc_b_work * (osp_e - osp_s);
        tr_start = 0;
        tr_end = 0;
        balance211(work_amount, jcp.nthr_ic_b, ithr_ic_b, tr_start, tr_end);

        g = 0;
        int oc_b = 0;
        nd_iterator_init(
                tr_start, g, g_work, oc_b, oc_b_work, j, osp_e - osp_s);

        if (jcp.nthr_ic_b > 1)
            barrier(&tr_diff_dst_bctx[ithr_but_ic], jcp.nthr_ic_b);
        while (tr_start < tr_end) {
            int g_ = g + g_start;
            int oc_b_ = oc_b + oc_b_start;
            int j_s = j + osp_s;
            int j_e = j_s + nstl::min(tr_end - tr_start, osp_e - j_s);
            const int oc_off_idx = g_ * jcp.oc + oc_b_ * jcp.oc_block;
            const diff_dst_data_t *p_diff_dst
                    = &diff_dst[diff_dst_d.blk_off(img, oc_off_idx, j_s)];

            diff_dst_data_t *p_tr_diff_dst {nullptr};
            if (jcp.harness == harness_2d_reduction) {
                p_tr_diff_dst
                        = &tr_diff_dst[tr_diff_dst_off(g_, oc_b_, 0, j_s)];
                trans_dst_nxc(
                        p_tr_diff_dst, p_diff_dst, 0, 0, oc_b_, 0, j_e - j_s);
            } else if (jcp.harness == harness_3d_reduction) {
                p_tr_diff_dst
                        = &tr_diff_dst[tr_diff_dst_off(g_, oc_b_, j_s, 0)];
                trans_dst_nxc(p_tr_diff_dst, p_diff_dst, 0, 0, oc_b_, 0,
                        (j_e - j_s) * jcp.oh);
            } else
                assert(!"Invalid harness type");

            nd_iterator_jump(tr_start, tr_end, g, g_work, oc_b, oc_b_work, j,
                    osp_e - osp_s);
        }
        if (jcp.nthr_ic_b > 1)
            barrier(&tr_diff_dst_bctx[ithr_but_ic], jcp.nthr_ic_b);
    }

    void maybe_local_traspose(int img, int isb_s, int isb_e, int g, int ic_b,
            int osb_s, int osb_e, int oc_b, void *&p_src, void *&p_dst) const {
        if (jcp.global_transpose) {
            if (jcp.harness == harness_2d_reduction) {
                p_src = &tr_src[tr_src_off(g, ic_b, 1, 0, isb_e, isb_s)];
                p_dst = &tr_diff_dst[tr_diff_dst_off(g, oc_b, 0, osb_s)];
            } else if (jcp.harness == harness_3d_reduction) {
                p_src = &tr_src[tr_src_off(g, ic_b, isb_e, isb_s, jcp.ih, 0)];
                p_dst = &tr_diff_dst[tr_diff_dst_off(g, oc_b, osb_s, 0)];
            } else
                assert(!"Invalid harness type");
            return;
        }

        const int nb_ic_blocks = (ic_b + jcp.nb_ic_blocking > ic_b_end)
                ? 1
                : jcp.nb_ic_blocking;

        const int nb_oc_blocks = (oc_b + jcp.nb_oc_blocking > oc_b_end)
                ? 1
                : jcp.nb_oc_blocking;

        src_data_t *p_tr_src {nullptr};
        if (jcp.harness == harness_2d_reduction) {
            p_tr_src = &tr_src[tr_src_off(0, 0, 1, 0, isb_e, isb_s)];
        } else if (jcp.harness == harness_3d_reduction) {
            p_tr_src = &tr_src[tr_src_off(0, 0, isb_e, isb_s, jcp.ih, 0)];
        } else
            assert(!"Invalid harness type");

        for (int icb = 0; icb < nb_ic_blocks; icb++) {
            const int ic_off_idx = g * jcp.ic + (ic_b + icb) * jcp.ic_block;
            const src_data_t *p_src
                    = (src_data_t *)&src[src_d.blk_off(img, ic_off_idx, isb_s)];
            src_data_t *tr_src_local = p_tr_src + icb * jcp.tr_src_buf_size;
            if (jcp.harness == harness_2d_reduction) {
                trans_src_nxc(tr_src_local, p_src, 0, 0, (ic_b + icb), 0,
                        isb_e - isb_s);
            } else if (jcp.harness == harness_3d_reduction) {
                trans_src_nxc(tr_src_local, p_src, 0, 0, (ic_b + icb), 0,
                        (isb_e - isb_s) * jcp.ih);
            } else
                assert(!"Invalid harness type");
        }
        p_src = p_tr_src;

        diff_dst_data_t *p_tr_diff_dst
                = &tr_diff_dst[tr_diff_dst_off(0, 0, 0, 0)];
        for (int ocb = 0; ocb < nb_oc_blocks; ocb++) {
            const int oc_off_idx = g * jcp.oc + (oc_b + ocb) * jcp.oc_block;
            const diff_dst_data_t *p_diff_dst
                    = &diff_dst[diff_dst_d.blk_off(img, oc_off_idx, osb_s)];
            diff_dst_data_t *tr_diff_dst_local
                    = p_tr_diff_dst + ocb * jcp.tr_diff_dst_buf_size;
            if (jcp.harness == harness_2d_reduction) {
                trans_dst_nxc(tr_diff_dst_local, p_diff_dst, 0, 0, (oc_b + ocb),
                        0, osb_e - osb_s);
            } else if (jcp.harness == harness_3d_reduction) {
                trans_dst_nxc(tr_diff_dst_local, p_diff_dst, 0, 0, (oc_b + ocb),
                        0, (osb_e - osb_s) * jcp.oh);
            } else
                assert(!"Invalid harness type");
        }
        p_dst = p_tr_diff_dst;
    }
};

void brgemm_convolution_bwd_weights_t::call_brgemm_kernel(thread_info_t &btc,
        int brg_idx, int batch_size, void *ptr_C, void *ptr_D) const {

    const auto brg_ker = brg_kernels_[brg_idx].get();
    assert(brg_ker != nullptr);

    // TODO: avoid costly tile reconfigurations
    if (std::memcmp(btc.cur_palette.a, brg_kernel_palettes_[brg_idx].a,
                AMX_PALETTE_SIZE)
            != 0) {
        amx_tile_configure(brg_kernel_palettes_[brg_idx].a);
        std::memcpy(btc.cur_palette.a, brg_kernel_palettes_[brg_idx].a,
                AMX_PALETTE_SIZE);
    }

    brgemm_kernel_execute(brg_ker, batch_size, btc.brg_batch, ptr_C,
            static_cast<void *>(btc.wsp_tile));
}

void brgemm_convolution_bwd_weights_t::call_brgemm(thread_info_t &btc,
        const void *ptr_A, const void *ptr_B, void *ptr_C, void *ptr_D,
        int ohb_l, bool do_init, bool M_tail, bool N_tail) const {

    const auto _pd = pd();
    const auto &jcp = _pd->jcp_;

    int bs = ohb_l;
    auto brg_idx = _pd->get_brg_idx(
            bs, M_tail ? jcp.M_tail : jcp.M, do_init, N_tail, false);

    for (int ohb = 0; ohb < ohb_l; ohb++) {
        btc.brg_batch[ohb].ptr.A = (char *)ptr_A
                + ohb * jcp.typesize_in * jcp.tr_iw * jcp.ic_block
                        * jcp.stride_h;
        btc.brg_batch[ohb].ptr.B = (char *)ptr_B
                + ohb * jcp.typesize_in * jcp.tr_ow * jcp.oc_block;
    }

    call_brgemm_kernel(btc, brg_idx, bs, ptr_C, ptr_D);
}

void brgemm_convolution_bwd_weights_t::compute_diff_weights_2d(
        thread_info_t *ti) const {

    const memory_desc_wrapper diff_weights_d(pd()->diff_weights_md(0));
    const auto _pd = pd();
    const auto &jcp = _pd->jcp_;

    const int wei_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block * jcp.nb_ic
            * jcp.ic_block * jcp.kd * jcp.kh * jcp.kw;
    const int bias_buf_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block;
    const int optimal_spblock = jcp.spatial_blk_size;

    float *diff_wei;
    if (diff_weights_d.data_type() == data_type::bf16)
        diff_wei = ti->wei_bia_reduction + (ti->ithr_mb) * wei_size;
    else
        diff_wei = ti->ithr_mb == 0
                ? (float *)ti->diff_weights
                : ti->wei_bia_reduction + (ti->ithr_mb - 1) * wei_size;

    float *diff_bias = nullptr;
    if (jcp.with_bias) {
        if (jcp.bia_dt == data_type::bf16)
            diff_bias = ti->bia_reduction + (ti->ithr_mb) * bias_buf_size;
        else
            diff_bias = ti->ithr_mb == 0
                    ? (float *)ti->diff_bias
                    : ti->bia_reduction + (ti->ithr_mb - 1) * bias_buf_size;
    }

    int img {0}, oh_s {0};
    int start = ti->img_start;
    int end = ti->img_end;

    int img_s {0};
    nd_iterator_init(start, img_s, jcp.mb, oh_s, jcp.oh);
    img = img_s;

    auto do_brgemm_call = [&](int g, int bs, int ic_b, int oc_b, int ihb_s,
                                  int ohb_s, int bs_ih_s, int bs_oh_s,
                                  const void *p_src, const void *p_dst, int kh,
                                  int kw, bool do_init) {
        auto ocb_end = nstl::min(oc_b + jcp.nb_oc_blocking, ti->oc_b_end);
        auto icb_end = nstl::min(ic_b + jcp.nb_ic_blocking, ti->ic_b_end);
        const int src_stride_w_shift = jcp.tr_iw / jcp.stride_w;
        const void *ptr_A = ((src_data_t *)p_src)
                + _pd->filter_w_to_src(kw) / jcp.stride_w
                + (kw % jcp.stride_w) * src_stride_w_shift
                + (bs_ih_s - ihb_s) * jcp.tr_iw * jcp.ic_block;
        const void *ptr_B = ((diff_dst_data_t *)p_dst)
                + (bs_oh_s - ohb_s) * jcp.tr_ow * jcp.oc_block;

        void *ptr_C = (jcp.transform_to_vnni)
                ? diff_wei + wei_offset_int(g, oc_b, ic_b, 0, kh, kw)
                : diff_wei + wht_blk_off(diff_weights_d, g, oc_b, ic_b, kh, kw);
        void *ptr_D = ptr_C;
        bool M_tail = (icb_end < ic_b + jcp.nb_ic_blocking);
        bool N_tail = (ocb_end < oc_b + jcp.nb_oc_blocking);

        auto brg_idx = _pd->get_brg_idx(
                bs, M_tail ? jcp.M_tail : jcp.M, do_init, N_tail, false);

        for (int ohb = 0; ohb < bs; ohb++) {
            ti->brg_batch[ohb].ptr.A = (char *)ptr_A
                    + ohb * jcp.typesize_in * jcp.tr_iw * jcp.ic_block
                            * jcp.stride_h;
            ti->brg_batch[ohb].ptr.B = (char *)ptr_B
                    + ohb * jcp.typesize_in * jcp.tr_ow * jcp.oc_block;
        }

        call_brgemm_kernel(*ti, brg_idx, bs, ptr_C, ptr_D);
    };

    while (start < end) {
        const int oh_e = _pd->get_finish_oh(
                oh_s, start, nstl::min(end, start + jcp.oh_block));
        int ih_s = nstl::max(0, -jcp.t_pad + oh_s * jcp.stride_h);
        const int ih_e = nstl::min(
                jcp.ih, -jcp.t_pad + (oh_e - 1) * jcp.stride_h + jcp.ext_kh);

        ti->maybe_global_transpose(img, ih_s, ih_e, oh_s, oh_e);

        int height_block = jcp.global_transpose ? oh_e - oh_s : optimal_spblock;

        for_(int ohb_s = oh_s; ohb_s < oh_e; ohb_s += height_block)
        for_(int g = ti->g_start; g < ti->g_end; ++g)
        for_(int oc_b = ti->oc_b_start; oc_b < ti->oc_b_end;
                oc_b += jcp.nb_oc_blocking)
        for (int ic_b = ti->ic_b_start; ic_b < ti->ic_b_end;
                ic_b += jcp.nb_ic_blocking) {
            const int ohb_e = nstl::min(ohb_s + height_block, oh_e);
            const int ihb_s = nstl::max(0, -jcp.t_pad + ohb_s * jcp.stride_h);
            const int ihb_e = nstl::min(jcp.ih,
                    -jcp.t_pad + (ohb_e - 1) * jcp.stride_h + jcp.ext_kh);
            assert(IMPLICATION(jcp.global_transpose,
                    oh_s == ohb_s && oh_e == ohb_e && ih_s == ihb_s
                            && ih_e == ihb_e));
            const int nb_oc_blocks = (oc_b + jcp.nb_oc_blocking > ti->oc_b_end)
                    ? 1
                    : jcp.nb_oc_blocking;
            void *p_src {nullptr};
            void *p_dst {nullptr};
            ti->maybe_local_traspose(img, ihb_s, ihb_e, g, ic_b, ohb_s, ohb_e,
                    oc_b, p_src, p_dst);

            assert(ohb_e <= jcp.oh);
            if (jcp.with_bias && ic_b == 0) {
                auto bp = jit_conv_call_s();

                bp.bias = diff_bias + g * rnd_up(jcp.oc, jcp.oc_block)
                        + oc_b * jcp.oc_block;
                bp.channel = (start == ti->img_start) && (ohb_s == oh_s);

                bp.os_index_begin = ohb_s;
                bp.os_index_end = ohb_e;
                bp.last_oc_block = (nb_oc_blocks == jcp.nb_oc_blocking) ? 0 : 1;

                bp.dst = p_dst;

                (*diff_bias_kernel_)(&bp);
            }

            const auto do_init = (start == ti->img_start);

            for (int kh = 0; kh < jcp.kh; kh++) {
                const int bs_ih_s = _pd->get_start_ih(kh, ohb_s);
                const int bs_ih_e = _pd->get_finish_ih(kh, ohb_e);
                const auto bs = div_up(bs_ih_e - bs_ih_s, jcp.stride_h);
                if (bs == 0 && !do_init) continue;

                const int bs_oh_s = utils::saturate(0, jcp.oh,
                        (bs_ih_s + jcp.t_pad - kh * (jcp.dilate_h + 1))
                                / jcp.stride_h);
                for_(int s = 0; s < jcp.stride_w; s++)
                for (int kw = s; kw < jcp.kw; kw += jcp.stride_w)
                    do_brgemm_call(g, bs, ic_b, oc_b, ihb_s, ohb_s, bs_ih_s,
                            bs_oh_s, p_src, p_dst, kh, kw, do_init);
            }
        }

        nd_iterator_jump(start, nstl::min(end, start + jcp.oh_block), img,
                jcp.mb, oh_s, jcp.oh);
    }
}

void brgemm_convolution_bwd_weights_t::compute_diff_weights_3d(
        thread_info_t *ti) const {

    const memory_desc_wrapper diff_weights_d(pd()->diff_weights_md(0));
    const auto _pd = pd();
    const auto &jcp = _pd->jcp_;

    const int wei_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block * jcp.nb_ic
            * jcp.ic_block * jcp.kd * jcp.kh * jcp.kw;
    const int bias_buf_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block;
    const int optimal_spblock = jcp.spatial_blk_size;

    float *diff_wei;
    if (diff_weights_d.data_type() == data_type::bf16)
        diff_wei = ti->wei_bia_reduction + (ti->ithr_mb) * wei_size;
    else
        diff_wei = ti->ithr_mb == 0
                ? (float *)ti->diff_weights
                : ti->wei_bia_reduction + (ti->ithr_mb - 1) * wei_size;

    float *diff_bias = nullptr;
    if (jcp.with_bias) {
        if (jcp.bia_dt == data_type::bf16)
            diff_bias = ti->bia_reduction + (ti->ithr_mb) * bias_buf_size;
        else
            diff_bias = ti->ithr_mb == 0
                    ? (float *)ti->diff_bias
                    : ti->bia_reduction + (ti->ithr_mb - 1) * bias_buf_size;
    }

    int img {0}, od_s {0};
    int start = ti->img_start;
    int end = ti->img_end;

    int img_s {0};
    nd_iterator_init(start, img_s, jcp.mb, od_s, jcp.od);
    img = img_s;

    auto do_brgemm_call = [&](int g, int bs_d, int bs_h, int ic_b, int oc_b,
                                  int ih_s, int idb_s, int oh_s, int odb_s,
                                  int ohb_s, int bs_id_s, int bs_ih_s,
                                  int bs_od_s, int bs_oh_s, const void *p_src,
                                  const void *p_dst, int kd, int kh, int kw,
                                  bool do_init) {
        auto ocb_end = nstl::min(oc_b + jcp.nb_oc_blocking, ti->oc_b_end);
        auto icb_end = nstl::min(ic_b + jcp.nb_ic_blocking, ti->ic_b_end);
        const int src_stride_w_shift = jcp.tr_iw / jcp.stride_w;
        const void *ptr_A = ((src_data_t *)p_src)
                + _pd->filter_w_to_src(kw) / jcp.stride_w
                + (kw % jcp.stride_w) * src_stride_w_shift
                + (bs_ih_s - ih_s) * jcp.tr_iw * jcp.ic_block
                + (bs_id_s - idb_s) * jcp.ih * jcp.tr_iw * jcp.ic_block;
        const void *ptr_B = ((diff_dst_data_t *)p_dst)
                + (bs_oh_s - oh_s) * jcp.tr_ow * jcp.oc_block
                + (bs_od_s - odb_s) * jcp.oh * jcp.tr_ow * jcp.oc_block;
        void *ptr_C = (jcp.transform_to_vnni)
                ? diff_wei + wei_offset_int(g, oc_b, ic_b, kd, kh, kw)
                : diff_wei
                        + wht_blk_off(
                                diff_weights_d, g, oc_b, ic_b, kd, kh, kw);
        void *ptr_D = ptr_C;
        bool M_tail = (icb_end < ic_b + jcp.nb_ic_blocking);
        bool N_tail = (ocb_end < oc_b + jcp.nb_oc_blocking);

        const auto bs = bs_d * bs_h;
        auto brg_idx = _pd->get_brg_idx(
                bs, M_tail ? jcp.M_tail : jcp.M, do_init, N_tail, false);

        for (int odb = 0; odb < bs_d; odb++) {
            for (int ohb = 0; ohb < bs_h; ohb++) {
                ti->brg_batch[odb * bs_h + ohb].ptr.A = (char *)ptr_A
                        + ohb * jcp.typesize_in * jcp.tr_iw * jcp.ic_block
                                * jcp.stride_h
                        + odb * jcp.typesize_in * jcp.ih * jcp.tr_iw
                                * jcp.ic_block * jcp.stride_h;
                ti->brg_batch[odb * bs_h + ohb].ptr.B = (char *)ptr_B
                        + ohb * jcp.typesize_in * jcp.tr_ow * jcp.oc_block
                        + odb * jcp.typesize_in * jcp.oh * jcp.tr_ow
                                * jcp.oc_block;
            }
        }

        call_brgemm_kernel(*ti, brg_idx, bs, ptr_C, ptr_D);
    };

    const auto oh_s = 0;
    const auto oh_e = jcp.oh;
    const int ih_s = nstl::max(0, -jcp.t_pad);

    while (start < end) {
        const int od_e = _pd->get_finish_od(
                od_s, start, nstl::min(end, start + jcp.od_block));
        int id_s = nstl::max(0, -jcp.f_pad + od_s * jcp.stride_d);
        const int id_e = nstl::min(
                jcp.id, -jcp.f_pad + (od_e - 1) * jcp.stride_d + jcp.ext_kd);

        ti->maybe_global_transpose(img, id_s, id_e, od_s, od_e);

        int sp_block = jcp.global_transpose ? od_e - od_s : optimal_spblock;

        for_(int odb_s = od_s; odb_s < od_e; odb_s += sp_block)
        for_(int g = ti->g_start; g < ti->g_end; ++g)
        for_(int oc_b = ti->oc_b_start; oc_b < ti->oc_b_end;
                oc_b += jcp.nb_oc_blocking)
        for (int ic_b = ti->ic_b_start; ic_b < ti->ic_b_end;
                ic_b += jcp.nb_ic_blocking) {
            const int odb_e = nstl::min(odb_s + sp_block, od_e);
            const int idb_s = nstl::max(0, -jcp.f_pad + odb_s * jcp.stride_d);
            const int idb_e = nstl::min(jcp.id,
                    -jcp.f_pad + (odb_e - 1) * jcp.stride_d + jcp.ext_kd);
            assert(IMPLICATION(jcp.global_transpose,
                    od_s == odb_s && od_e == odb_e && id_s == idb_s
                            && id_e == idb_e));
            const int nb_oc_blocks = (oc_b + jcp.nb_oc_blocking > ti->oc_b_end)
                    ? 1
                    : jcp.nb_oc_blocking;
            void *p_src {nullptr};
            void *p_dst {nullptr};
            ti->maybe_local_traspose(img, idb_s, idb_e, g, ic_b, odb_s, odb_e,
                    oc_b, p_src, p_dst);

            assert(odb_e <= jcp.od);

            if (jcp.with_bias && ic_b == 0) {
                for (int iodb = odb_s; iodb < odb_e; iodb++) {
                    auto bp = jit_conv_call_s();

                    bp.bias = diff_bias + g * rnd_up(jcp.oc, jcp.oc_block)
                            + oc_b * jcp.oc_block;
                    bp.os_index_begin = oh_s;
                    bp.os_index_end = oh_e;
                    bp.last_oc_block
                            = (nb_oc_blocks == jcp.nb_oc_blocking) ? 0 : 1;

                    bp.channel = (start == ti->img_start) && (odb_s == od_s)
                            && (iodb == odb_s);
                    bp.dst = ((diff_dst_data_t *)p_dst)
                            + (iodb - odb_s) * jcp.oh * jcp.tr_ow
                                    * jcp.oc_block;
                    (*diff_bias_kernel_)(&bp);
                }
            }

            for (int ohb_s = oh_s; ohb_s < oh_e; ohb_s += jcp.oh_block) {

                const auto ohb_e = nstl::min(jcp.oh, ohb_s + jcp.oh_block);

                const auto do_init = (start == ti->img_start && ohb_s == oh_s);

                for (int kd = 0; kd < jcp.kd; kd++) {
                    const int bs_id_s = _pd->get_start_id(kd, odb_s);
                    const int bs_id_e = _pd->get_finish_id(kd, odb_e);
                    const auto bs_d = div_up(bs_id_e - bs_id_s, jcp.stride_d);
                    // bs_d may be 0 but we may still need to call brgemm to
                    // initialize output
                    if (bs_d == 0 && !do_init) continue;

                    const int bs_od_s = utils::saturate(0, jcp.od,
                            (bs_id_s + jcp.f_pad - kd * (jcp.dilate_d + 1))
                                    / jcp.stride_d);

                    for (int kh = 0; kh < jcp.kh; kh++) {
                        const int bs_ih_s = _pd->get_start_ih(kh, ohb_s);
                        const int bs_ih_e = _pd->get_finish_ih(kh, ohb_e);
                        const auto bs_h
                                = div_up(bs_ih_e - bs_ih_s, jcp.stride_h);
                        if (bs_h == 0 && !do_init) continue;

                        const int bs_oh_s = utils::saturate(0, jcp.oh,
                                (bs_ih_s + jcp.t_pad - kh * (jcp.dilate_h + 1))
                                        / jcp.stride_h);
                        for_(int s = 0; s < jcp.stride_w; s++)
                        for (int kw = s; kw < jcp.kw; kw += jcp.stride_w)
                            do_brgemm_call(g, bs_d, bs_h, ic_b, oc_b, ih_s,
                                    idb_s, oh_s, odb_s, ohb_s, bs_id_s, bs_ih_s,
                                    bs_od_s, bs_oh_s, p_src, p_dst, kd, kh, kw,
                                    do_init);
                    }
                }
            }
        }

        nd_iterator_jump(start, nstl::min(end, start + jcp.od_block), img,
                jcp.mb, od_s, jcp.od);
    }
}

void brgemm_convolution_bwd_weights_t::reduce_and_convert_diff_weights_and_bias(
        thread_info_t *ti) const {

    const memory_desc_wrapper diff_weights_d(pd()->diff_weights_md(0));

    const auto &jcp = pd()->jcp_;
    const int wei_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block * jcp.nb_ic
            * jcp.ic_block * jcp.kh * jcp.kw * ((jcp.ndims == 5) ? jcp.kd : 1);

    const bool is_bf16_out = diff_weights_d.data_type() == data_type::bf16;
    const bool is_bf16_bias = jcp.with_bias && jcp.bia_dt == data_type::bf16;

    auto store_in_vnni_format = [&]() {
        for_(int g = ti->g_start; g < ti->g_end; g++)
        for_(int oc_b = ti->oc_b_start; oc_b < ti->oc_b_end; oc_b++)
        for_(int ic_b = ti->ic_b_start; ic_b < ti->ic_b_start + ti->ic_b_work;
                ic_b += 2)
        {
            jit_conv_call_s p = jit_conv_call_s();

            bfloat16_t *output = (bfloat16_t *)ti->diff_weights
                    + wei_offset_ext(g, oc_b, (ic_b / 2), 0);
            float *input
                    = ti->wei_bia_reduction + wei_offset_int(g, oc_b, ic_b, 0);

            p.src = (void *)input;
            p.dst = (void *)output;
            p.last_ic_block = ((ic_b + 1) >= jcp.nb_ic) ? 1 : 0;
            (*diff_wei_trans_kernel_)(&p);
        }
    };

    if (jcp.nthr_mb == 1) {
        if (is_bf16_out) {
            // reduction is not required, only conversion
            if (jcp.transform_to_vnni) {
                store_in_vnni_format();
            } else {
                for_(int g = ti->g_start; g < ti->g_end; g++)
                for (int oc_b = ti->oc_b_start; oc_b < ti->oc_b_end; oc_b++) {
                    const size_t acc_size = (size_t)ti->ic_b_work * jcp.kh
                            * jcp.kw * ((jcp.ndims == 5) ? jcp.kd : 1)
                            * jcp.ic_block * jcp.oc_block;
                    const size_t off = wht_blk_off(
                            diff_weights_d, g, oc_b, ti->ic_b_start);
                    cvt_float_to_bfloat16(
                            (bfloat16_t *)(ti->diff_weights) + off,
                            (ti->wei_bia_reduction + off), acc_size);
                }
            }
        }
        if (is_bf16_bias && ti->ithr_ic_b == 0 && ti->ic_b_work > 0) {
            for (int g = ti->g_start; g < ti->g_end; g++) {
                int result_start_idx
                        = g * jcp.oc + ti->oc_b_start * jcp.oc_block;
                int buffer_start_idx = g * rnd_up(jcp.oc, jcp.oc_block)
                        + ti->oc_b_start * jcp.oc_block;
                const size_t acc_size
                        = nstl::min(jcp.oc, ti->oc_b_end * jcp.oc_block)
                        - ti->oc_b_start * jcp.oc_block;
                bfloat16_t *diff_bias
                        = (bfloat16_t *)ti->diff_bias + result_start_idx;
                float *buffer = ti->bia_reduction + buffer_start_idx;
                cvt_float_to_bfloat16(diff_bias, buffer, acc_size);
            }
        }
        return;
    }

    /* diff_weights[:] += sum(wei_reduction_[thr_mb][:]) */
    if (jcp.global_transpose)
        simple_barrier::barrier(ti->wei_bia_reduction_bctx, jcp.nthr);

    const int ic_b_kh_work
            = ti->ic_b_work * ((jcp.ndims == 5) ? jcp.kd : jcp.kh);
    const int work = ti->g_work * ti->oc_b_work * ic_b_kh_work;

    int start {0}, end {0};
    balance211(work, jcp.nthr_mb, ti->ithr_mb, start, end);
    if (!jcp.transform_to_vnni && start == end) return;

    const int _start_nthr_mb = 1;
    for (int thr_mb = _start_nthr_mb; thr_mb < jcp.nthr_mb; ++thr_mb) {
        int w = start;
        int sub_g_start {0}, sub_oc_b_start {0}, sub_ic_b_kh_start {0};
        nd_iterator_init(w, sub_g_start, ti->g_work, sub_oc_b_start,
                ti->oc_b_work, sub_ic_b_kh_start, ic_b_kh_work);
        while (w < end) {
            const int g = ti->g_start + sub_g_start;
            const int oc_b = ti->oc_b_start + sub_oc_b_start;
            const int ic_b = ti->ic_b_start
                    + sub_ic_b_kh_start / ((jcp.ndims == 5) ? jcp.kd : jcp.kh);
            const int kX
                    = sub_ic_b_kh_start % ((jcp.ndims == 5) ? jcp.kd : jcp.kh);

            const size_t acc_size = (size_t)jcp.kw * jcp.ic_block * jcp.oc_block
                    * ((jcp.ndims == 5) ? jcp.kh : 1)
                    * nstl::min(end - w, ic_b_kh_work - sub_ic_b_kh_start);

            const size_t off_ext
                    = wht_blk_off(diff_weights_d, g, oc_b, ic_b, kX);
            const size_t off_int = (jcp.transform_to_vnni)
                    ? wei_offset_int(g, oc_b, ic_b, kX)
                    : off_ext;

            float *wei_reduced = is_bf16_out
                    ? ti->wei_bia_reduction + off_int
                    : (float *)(ti->diff_weights) + off_ext;

            int thr_mb_buffer_idx = is_bf16_out ? thr_mb : thr_mb - 1;
            float *wei_to_reduce = ti->wei_bia_reduction
                    + thr_mb_buffer_idx * wei_size + off_int;

            if (!jcp.transform_to_vnni && is_bf16_out
                    && thr_mb == jcp.nthr_mb - 1)
                // the last iteration for bfloat16 requires conversion and
                // store to diff_weights array
                add_floats_and_cvt_to_bfloat16(
                        (bfloat16_t *)(ti->diff_weights) + off_ext, wei_reduced,
                        wei_to_reduce, acc_size);
            else
                acc_ker_->accumulate(wei_reduced, wei_to_reduce, acc_size);

            nd_iterator_jump(w, end, sub_g_start, ti->g_work, sub_oc_b_start,
                    ti->oc_b_work, sub_ic_b_kh_start, ic_b_kh_work);
        }
        if (jcp.with_bias && ti->ithr_ic_b == 0 && ti->ic_b_work > 0
                && ti->ithr_mb == 0 && ti->img_work > 0) {
            for (int g = ti->g_start; g < ti->g_end; g++) {
                float *bias_reduced = is_bf16_bias ? ti->bia_reduction
                                                   : (float *)(ti->diff_bias);
                int thr_mb_buffer_idx = is_bf16_bias ? thr_mb : thr_mb - 1;
                int bias_buf_size = jcp.ngroups * jcp.nb_oc * jcp.oc_block;
                float *bias_to_reduce
                        = ti->bia_reduction + thr_mb_buffer_idx * bias_buf_size;
                const size_t acc_size
                        = nstl::min(jcp.oc, ti->oc_b_end * jcp.oc_block)
                        - ti->oc_b_start * jcp.oc_block;
                int idx = g * rnd_up(jcp.oc, jcp.oc_block)
                        + ti->oc_b_start * jcp.oc_block;
                if (is_bf16_bias && thr_mb == jcp.nthr_mb - 1) {
                    // the last iteration for bfloat16 requires conversion and
                    // store to diff_weights array
                    int diff_bias_idx
                            = g * jcp.oc + ti->oc_b_start * jcp.oc_block;
                    add_floats_and_cvt_to_bfloat16(
                            (bfloat16_t *)(ti->diff_bias) + diff_bias_idx,
                            &bias_reduced[idx], &bias_to_reduce[idx], acc_size);
                } else {
                    acc_ker_->accumulate(
                            &bias_reduced[idx], &bias_to_reduce[idx], acc_size);
                }
            }
        }
    }

    if (jcp.transform_to_vnni) {
        simple_barrier::barrier(ti->wei_bia_reduction_bctx, jcp.nthr);
        store_in_vnni_format();
    }
}

void brgemm_convolution_bwd_weights_t::prepare_scratchpad_data(
        const exec_ctx_t &ctx) const {
    auto scratchpad = ctx.get_scratchpad_grantor();

    const auto &jcp = pd()->jcp_;

    auto tr_src = scratchpad.template get<src_data_t>(key_conv_tr_src);
    // Zero out guard elements that cross a buffer boundary to prevent a
    // race condition due to buffer overflows from memory optimization where
    // buffers sharing padding
    // TODO: optimize it
    for (size_t isb = 1; isb <= jcp.tr_src_buf_count; ++isb) {
        src_data_t *ts
                = &tr_src[isb * jcp.tr_src_buf_size * jcp.nb_ic_blocking];
        for (int i = 0; i < jcp.tr_src_num_guard_elems; ++i)
            ts[i] = 0;
    }

    if (jcp.global_transpose && jcp.nthr_oc_b > 1) {
        const int tr_src_bctx_size = jcp.nthr / jcp.nthr_oc_b;
        auto tr_src_bctx = scratchpad.template get<simple_barrier::ctx_t>(
                key_conv_tr_src_bctx);
        for (int i = 0; i < tr_src_bctx_size; ++i)
            simple_barrier::ctx_init(&tr_src_bctx[i]);
    }
    if (jcp.global_transpose) {
        if (jcp.nthr_ic_b > 1) {
            const int tr_diff_dst_bctx_size = jcp.nthr / jcp.nthr_ic_b;
            auto tr_diff_dst_bctx
                    = scratchpad.template get<simple_barrier::ctx_t>(
                            key_conv_tr_diff_dst_bctx);
            for (int i = 0; i < tr_diff_dst_bctx_size; ++i)
                simple_barrier::ctx_init(&tr_diff_dst_bctx[i]);
        }
    }

    if (jcp.nthr_mb > 1
            || pd()->diff_weights_md(0)->data_type == data_type::bf16) {
        // TODO: don't use barrier for case
        // diff_weights_type == data_type::bf16 && nthr_mb_ == 1
        simple_barrier::ctx_init(scratchpad.template get<simple_barrier::ctx_t>(
                key_conv_wei_bia_reduction_bctx));
    }
}

void brgemm_convolution_bwd_weights_t::execute_backward_weights(
        const exec_ctx_t &ctx) const {
    prepare_scratchpad_data(ctx);

    const auto &jcp = pd()->jcp_;

    parallel(jcp.nthr, [&](const int ithr, const int nthr) {
        assert(jcp.nthr == nthr);
        assert(utils::one_of(pd()->ndims(), 3, 4, 5));

        thread_info_t thread_info(this, ctx, ithr);
        switch (jcp.harness) {
            case harness_2d_reduction:
                compute_diff_weights_2d(&thread_info);
                if (jcp.global_transpose)
                    reduce_and_convert_diff_weights_and_bias(&thread_info);
                break;
            case harness_3d_reduction:
                compute_diff_weights_3d(&thread_info);
                if (jcp.global_transpose)
                    reduce_and_convert_diff_weights_and_bias(&thread_info);
                break;
            default: assert(!"Invalid harness type");
        }

        amx_tile_release();
    });

    if (!jcp.global_transpose) {
        parallel(jcp.nthr, [&](const int ithr, const int nthr) {
            assert(jcp.nthr == nthr);
            thread_info_t thread_info(this, ctx, ithr);
            reduce_and_convert_diff_weights_and_bias(&thread_info);
        });
    }

    if (pd()->with_bias() && (jcp.oc % jcp.oc_block != 0)
            && jcp.bia_dt != data_type::bf16) {
        auto diff_bias = ctx.get_scratchpad_grantor().template get<const float>(
                key_conv_padded_bias);
        auto diff_bias_in = CTX_OUT_MEM(float *, DNNL_ARG_DIFF_BIAS);
        const int padded_stride = rnd_up(jcp.oc, jcp.oc_block);
        const int stride = jcp.oc;
        for (int g = 0; g < jcp.ngroups; ++g) {
            utils::array_copy(diff_bias_in + g * stride,
                    diff_bias + g * padded_stride, stride);
        }
    }
}

} // namespace x64

} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
