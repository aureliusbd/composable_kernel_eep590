// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2022, Advanced Micro Devices, Inc. All rights reserved.

#pragma once

#include "ck/utility/common_header.hpp"
#include "ck/tensor_operation/gpu/thread/threadwise_tensor_slice_transfer.hpp"
#include "ck/tensor_operation/gpu/warp/wmma_gemm.hpp"
#include "ck/tensor_description/tensor_adaptor.hpp"
#include "ck/tensor_operation/gpu/element/unary_element_wise_operation.hpp"

#define CK_MNK_LOOP

namespace ck {

template <index_t BlockSize,
          typename FloatA,
          typename FloatB,
          typename FloatAcc,
          typename ABlockDesc,
          typename BBlockDesc,
          index_t MPerBlock,
          index_t NPerBlock,
          index_t KPerBlock,
          index_t MPerWMMA,
          index_t NPerWMMA,
          index_t MRepeat,
          index_t NRepeat,
          index_t KPack,
          bool AEnableLds = true,
          bool BEnableLds = true,
          bool TransposeC = false>
/* Option: Read from LDS, big buffer hold all threads required data
 * Source
 * A: K0PerBlock x MPerBlock x K1
 * B: K0PerBlock x NPerBlock x K1
 * Destination
 * C, non-transpose
 * thread level: MRepeat x NRepeat x MAccVgprs
 * block  level: MRepeat x MWave x MSubGroup x NRepeat x NWave x NThreadPerSubGroup x MAccVgprs
 * KPACK == WMMA_K = 16
 *
 * Option: Read from VMEM, small buffer hold each thread own required data (Skip LDS)
 * Source:
 * A(if skip LDS): MRepeat x KPack
 * B(if skip LDS): NRepeat x KPack
 * Destination
 * C, non-transpose
 * block level: MRepeat x MWave x MSubGroup x NRepeat x NWave x NThreadPerSubGroup x MAccVgprs
 */
struct BlockwiseGemmWMMA
{
    static constexpr auto I0    = Number<0>{};
    static constexpr auto I1    = Number<1>{};
    static constexpr auto I2    = Number<2>{};
    static constexpr auto I3    = Number<3>{};
    static constexpr auto I4    = Number<4>{};
    static constexpr auto I5    = Number<5>{};
    static constexpr auto WmmaK = Number<16>{};

    using ThisThreadBlock = ThisThreadBlock<BlockSize>;

    // Hardcode of WaveSize, since current HIP Runtime(5.4.0-10984) could not return correct one.
    static constexpr index_t WaveSize = 32;

    // When use LDS, each Row(16 consecutive lanes) read whole data from source buffer
    // When not use LDS, each Row read half of whole data from source buffer, exchange the data via
    // permutation
    static constexpr index_t A_KRow = AEnableLds ? 1 : 2;
    static constexpr index_t B_KRow = BEnableLds ? 1 : 2;
    static constexpr index_t A_K1   = ABlockDesc{}.GetLength(I5);
    static constexpr index_t B_K1   = BBlockDesc{}.GetLength(I5);

    static constexpr auto wmma_gemm =
        WmmaGemm<FloatA, FloatB, FloatAcc, MPerWMMA, NPerWMMA, KPack, TransposeC>{};

    static constexpr index_t MWaves = MPerBlock / (MRepeat * MPerWMMA);
    static constexpr index_t NWaves = NPerBlock / (NRepeat * NPerWMMA);

    StaticBufferTupleOfVector<AddressSpaceEnum::Vgpr,
                              FloatAcc,
                              MRepeat * NRepeat,
                              wmma_gemm.GetRegSizePerWmma(),
                              true>
        c_thread_buf_;

    __host__ __device__ constexpr auto& GetCThreadBuffer() { return c_thread_buf_; }

    __device__ static auto GetWaveIdx()
    {
        const index_t thread_id = ThisThreadBlock::GetThreadId();

        constexpr auto threadid_to_wave_idx_adaptor = make_single_stage_tensor_adaptor(
            make_tuple(make_merge_transform(make_tuple(MWaves, NWaves, WaveSize))),
            make_tuple(Sequence<0, 1, 2>{}),
            make_tuple(Sequence<0>{}));

        return threadid_to_wave_idx_adaptor.CalculateBottomIndex(make_multi_index(thread_id));
    }

    // Default, Block buffer in LDS, thread level offset enabled
    __device__ static auto CalculateAThreadOriginDataIndex()
    {
        if constexpr(AEnableLds)
        {
            const auto wave_idx   = GetWaveIdx();
            const auto waveId_m   = wave_idx[I0];
            const auto WMMA_a_idx = wmma_gemm.CalculateAThreadOriginDataIndex();

            //  |KRepeat   |MRepeat|MWave    |KRow  |MLane  |KPack
            return make_tuple(0, 0, waveId_m, 0, WMMA_a_idx, 0);
        }
        else
        {
            return make_tuple(0, 0, 0, 0, 0, 0);
        }
    }

    __device__ static auto CalculateBThreadOriginDataIndex()
    {
        if constexpr(BEnableLds)
        {
            const auto wave_idx   = GetWaveIdx();
            const auto waveId_n   = wave_idx[I1];
            const auto WMMA_b_idx = wmma_gemm.CalculateBThreadOriginDataIndex();

            //  |KRepeat   |NRepeat|Nwave     |KRow  |NLane  |KPack
            return make_tuple(0, 0, waveId_n, 0, WMMA_b_idx, 0);
        }
        else
        {
            return make_tuple(0, 0, 0, 0, 0, 0);
        }
    }

    template <index_t m0, index_t n0>
    __device__ static auto CalculateCThreadOriginDataIndex(Number<m0>, Number<n0>)
    {
        const auto wave_idx = GetWaveIdx();

        const auto waveId_m = wave_idx[I0];
        const auto waveId_n = wave_idx[I1];

        const auto blk_idx = wmma_gemm.GetBeginOfThreadBlk();

        constexpr auto mrepeat_mwave_mperWMMA_to_m_adaptor = make_single_stage_tensor_adaptor(
            make_tuple(make_unmerge_transform(make_tuple(MRepeat, MWaves, MPerWMMA))),
            make_tuple(Sequence<0>{}),
            make_tuple(Sequence<0, 1, 2>{}));

        constexpr auto nrepeat_nwave_nperWMMA_to_n_adaptor = make_single_stage_tensor_adaptor(
            make_tuple(make_unmerge_transform(make_tuple(NRepeat, NWaves, NPerWMMA))),
            make_tuple(Sequence<0>{}),
            make_tuple(Sequence<0, 1, 2>{}));

        const index_t c_thread_m = mrepeat_mwave_mperWMMA_to_m_adaptor.CalculateBottomIndex(
            make_tuple(m0, waveId_m, blk_idx[I0]))[I0];
        const index_t c_thread_n = nrepeat_nwave_nperWMMA_to_n_adaptor.CalculateBottomIndex(
            make_tuple(n0, waveId_n, blk_idx[I1]))[I0];

        return make_tuple(c_thread_m, c_thread_n);
    }

    template <index_t m0, index_t n0>
    __device__ static auto CalculateCThreadOriginDataIndex7D(Number<m0>, Number<n0>)
    {
        const auto wave_idx = GetWaveIdx();

        const auto waveId_m = wave_idx[I0];
        const auto waveId_n = wave_idx[I1];

        const auto blk_idx = wmma_gemm.GetBeginOfThreadBlk3D();

        return make_tuple(
            Number<m0>{}, waveId_m, blk_idx[I0], Number<n0>{}, waveId_n, blk_idx[I1], blk_idx[I2]);
    }

    using Tuple6 = decltype(CalculateAThreadOriginDataIndex());
    __host__ __device__ BlockwiseGemmWMMA(Tuple6 a_origin = CalculateAThreadOriginDataIndex(),
                                          Tuple6 b_origin = CalculateBThreadOriginDataIndex())
        : a_thread_copy_(a_origin), b_thread_copy_(b_origin)
    {
        static_assert(ABlockDesc::IsKnownAtCompileTime() && BBlockDesc::IsKnownAtCompileTime(),
                      "wrong! Desc should be known at compile-time");

        static_assert(ThisThreadBlock::GetNumOfThread() == MWaves * NWaves * WaveSize,
                      "ThisThreadBlock::GetNumOfThread() != MWaves * NWaves * WaveSize\n");

        static_assert(MPerBlock % (MPerWMMA * MRepeat) == 0 &&
                          NPerBlock % (NPerWMMA * NRepeat) == 0,
                      "wrong!");
    }

    // transposed WMMA output C' = B' * A'
    __host__ __device__ static constexpr auto
    GetCThreadDescriptor_MRepeat_MWave_MThreadPerSubGroup_NRepeat_NWave_NSubGroup_NAccVgprs()
    {
        constexpr auto c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens =
            wmma_gemm.GetCMSubGroupNThreadPerSubGroupMAccVgprsThreadBlkLengths();

        constexpr auto NAccVgprs = c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens[I2];

        return make_naive_tensor_descriptor_packed(
            //        |MRepeat            |MWave |MSubGroup |NRepeat           |NWave
            //        |NThreadPerSubGroup |MAccVgprs
            make_tuple(Number<MRepeat>{}, I1, I1, Number<NRepeat>{}, I1, I1, NAccVgprs));
    }

    // Thread level, register decriptor. Vector-write
    __host__ __device__ static constexpr auto
    GetCThreadDescriptor_MRepeat_MWave_MSubGroup_NRepeat_NWave_NThreadPerSubGroup_MAccVgprs()
    {
        constexpr auto c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens =
            wmma_gemm.GetCMSubGroupNThreadPerSubGroupMAccVgprsThreadBlkLengths();

        constexpr auto MAccVgprs = c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens[I2];
        constexpr auto AccStride = c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens[I3];
        return make_naive_tensor_descriptor(
            //        |MRepeat           |MWave |MSubGroup |NRepeat           |NWave
            //        |NThreadPerSubGroup |MAccVgprs
            make_tuple(Number<MRepeat>{}, I1, I1, Number<NRepeat>{}, I1, I1, MAccVgprs),
            make_tuple(Number<NRepeat>{} * MAccVgprs * AccStride,
                       Number<NRepeat>{} * MAccVgprs * AccStride,
                       Number<NRepeat>{} * MAccVgprs * AccStride,
                       MAccVgprs * AccStride,
                       MAccVgprs * AccStride,
                       MAccVgprs * AccStride,
                       AccStride));
    }

    template <typename CGridDesc_M_N>
    __host__ __device__ static constexpr auto
    MakeCGridDescriptor_MBlockxRepeat_MWave_MSubGroup_NBlockxRepeat_NWave_NThreadPerSubGroup_MAccVgprs(
        const CGridDesc_M_N& c_grid_desc_m_n)
    {
        const auto M = c_grid_desc_m_n.GetLength(I0);
        const auto N = c_grid_desc_m_n.GetLength(I1);

        const auto c_grid_desc_mblockxrepeat_mwave_mperwmma_nblockxrepeat_nwave_nperwmma =
            transform_tensor_descriptor(
                c_grid_desc_m_n,
                make_tuple(
                    make_unmerge_transform(make_tuple(M / (MWaves * MPerWMMA), MWaves, MPerWMMA)),
                    make_unmerge_transform(make_tuple(N / (NWaves * NPerWMMA), NWaves, NPerWMMA))),
                make_tuple(Sequence<0>{}, Sequence<1>{}),
                make_tuple(Sequence<0, 1, 2>{}, Sequence<3, 4, 5>{}));

        return wmma_gemm
            .MakeCDesc_MBlockxRepeat_MWave_MSubGroup_NBlockxRepeat_NWave_NThreadPerSubGroup_MAccVgprs(
                c_grid_desc_mblockxrepeat_mwave_mperwmma_nblockxrepeat_nwave_nperwmma);
    }

    // transposed WMMA output C' = B' * A'
    __host__ __device__ static constexpr auto
    GetCBlockDescriptor_MRepeat_MWave_MThreadPerSubGroup_NRepeat_NWave_NSubGroup_NAccVgprs()
    {
        constexpr auto c_block_desc_mrepeat_mwave_mperwmma_nrepeat_nwave_nperwmma =
            make_naive_tensor_descriptor_packed(make_tuple(Number<MRepeat>{},
                                                           Number<MWaves>{},
                                                           Number<MPerWMMA>{},
                                                           Number<NRepeat>{},
                                                           Number<NWaves>{},
                                                           Number<NPerWMMA>{}));

        return wmma_gemm
            .MakeCDesc_MBlockxRepeat_MWave_MThreadPerSubGroup_NBlockxRepeat_NWave_NSubGroup_NAccVgprs(
                c_block_desc_mrepeat_mwave_mperwmma_nrepeat_nwave_nperwmma);
    }

    // Provide dimension size
    __host__ __device__ static constexpr auto
    GetCBlockDescriptor_MRepeat_MWave_MSubGroup_NRepeat_NWave_NThreadPerSubGroup_MAccVgprs()
    {
        constexpr auto c_block_desc_mrepeat_mwave_mperwmma_nrepeat_nwave_nperwmma =
            make_naive_tensor_descriptor_packed(make_tuple(Number<MRepeat>{},
                                                           Number<MWaves>{},
                                                           Number<MPerWMMA>{},
                                                           Number<NRepeat>{},
                                                           Number<NWaves>{},
                                                           Number<NPerWMMA>{}));

        return wmma_gemm
            .MakeCDesc_MBlockxRepeat_MWave_MSubGroup_NBlockxRepeat_NWave_NThreadPerSubGroup_MAccVgprs(
                c_block_desc_mrepeat_mwave_mperwmma_nrepeat_nwave_nperwmma);
    }

    // Describe how data allocated in thread copy src buffer
    // M0_M1_M2 = MRepeat_MWave_MPerWmma, N0_N1_N2 = NRepeat_NWave_NPerWmma
    static constexpr ABlockDesc a_block_desc_k0_m0_m1_m2_k1;
    static constexpr BBlockDesc b_block_desc_k0_n0_n1_n2_k1;

    template <typename ABlockBuffer, typename BBlockBuffer, typename CThreadBuffer>
    __device__ void Run(const ABlockBuffer& a_block_buf,
                        const BBlockBuffer& b_block_buf,
                        CThreadBuffer& c_thread_buf) const
    {
        auto a_thread_buf = make_static_buffer<AddressSpaceEnum::Vgpr, FloatA>(
            a_thread_desc_.GetElementSpaceSize());
        auto b_thread_buf = make_static_buffer<AddressSpaceEnum::Vgpr, FloatB>(
            b_thread_desc_.GetElementSpaceSize());

        // basic intrinsic to determine loopover direction
        if constexpr(MRepeat < NRepeat)
        {
            static_for<0, KPerBlock / WmmaK, 1>{}(
                [&](auto k) { // k=0,1,2 instead of k=0,kpack*1, ...
                    static_for<0, MRepeat, 1>{}([&](auto m0) {
                        // read A
                        a_thread_copy_.Run(
                            a_block_desc_k0_m0_m1_m2_k1,
                            make_tuple(Number<k * WmmaK / A_K1 / A_KRow>{}, m0, I0, I0, I0, I0),
                            a_block_buf,
                            a_thread_desc_,
                            make_tuple(I0, m0, I0, I0, I0, I0),
                            a_thread_buf);

                        static_for<0, NRepeat, 1>{}([&](auto n0) {
                            // read B
                            b_thread_copy_.Run(
                                b_block_desc_k0_n0_n1_n2_k1,
                                make_tuple(Number<k * WmmaK / B_K1 / B_KRow>{}, n0, I0, I0, I0, I0),
                                b_block_buf,
                                b_thread_desc_,
                                make_tuple(I0, n0, I0, I0, I0, I0),
                                b_thread_buf);

                            vector_type<FloatA, WmmaK> a_thread_vec;
                            vector_type<FloatB, WmmaK> b_thread_vec;

                            static_for<0, WmmaK, 1>{}([&](auto i) {
                                a_thread_vec.template AsType<FloatA>()(i) =
                                    a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(i / A_K1 / A_KRow,
                                                   m0,
                                                   0,
                                                   (i / A_K1) % A_KRow,
                                                   0,
                                                   i % A_K1))>{}];
                                b_thread_vec.template AsType<FloatB>()(i) =
                                    b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                                        make_tuple(i / B_K1 / B_KRow,
                                                   n0,
                                                   0,
                                                   (i / B_K1) % B_KRow,
                                                   0,
                                                   i % B_K1))>{}];
                            });

#if 0
                            if (get_thread_local_1d_id() == 0){
                                printf("repeat: m,n,k:(%02d, %02d, %02d) a_thread_buf: %04x %04x %04x %04x %04x %04x %04x %04x | %04x %04x %04x %04x %04x %04x %04x %04x\n",
                                m0.value, n0.value, k.value,
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(0 / A_K1, m0, 0, 0, 0 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(1 / A_K1, m0, 0, 0, 1 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(2 / A_K1, m0, 0, 0, 2 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(3 / A_K1, m0, 0, 0, 3 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(4 / A_K1, m0, 0, 0, 4 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(5 / A_K1, m0, 0, 0, 5% A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(6 / A_K1, m0, 0, 0, 6 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(7 / A_K1, m0, 0, 0, 7 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(8 / A_K1, m0, 0, 0, 8 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(9 / A_K1, m0, 0, 0, 9% A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(10 / A_K1, m0, 0, 0, 10 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(11 / A_K1, m0, 0, 0, 11 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(12 / A_K1, m0, 0, 0, 12 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(13 / A_K1, m0, 0, 0, 13 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(14 / A_K1, m0, 0, 0, 14 % A_K1))>{}]))),
                                *(reinterpret_cast<const uint16_t*>(&(a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(15 / A_K1, m0, 0, 0, 15% A_K1))>{}])))
                                        );
                            }

                            // if (get_thread_local_1d_id() == 0){
                            //     printf("repeat: m,n,k:(%02d, %02d, %02d) b_thread_buf: %04x %04x %04x %04x %04x %04x %04x %04x | %04x %04x %04x %04x %04x %04x %04x %04x\n",
                            //     m0.value, n0.value, k.value,
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(0 / B_K1, n0, 0, 0, 0 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(1 / B_K1, n0, 0, 0, 1 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(2 / B_K1, n0, 0, 0, 2 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(3 / B_K1, n0, 0, 0, 3 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(4 / B_K1, n0, 0, 0, 4 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(5 / B_K1, n0, 0, 0, 5% B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(6 / B_K1, n0, 0, 0, 6 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(7 / B_K1, n0, 0, 0, 7 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(8 / B_K1, n0, 0, 0, 8 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(9 / B_K1, n0, 0, 0, 9% B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(10 / B_K1, n0, 0, 0, 10 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(11 / B_K1, n0, 0, 0, 11 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(12 / B_K1, n0, 0, 0, 12 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(13 / B_K1, n0, 0, 0, 13 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(14 / B_K1, n0, 0, 0, 14 % B_K1))>{}]))),
                            //     *(reinterpret_cast<const uint16_t*>(&(b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            //             make_tuple(15 / B_K1, n0, 0, 0, 15% B_K1))>{}])))
                            //             );
                            // }
#endif

                            using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                            using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                            constexpr index_t c_offset =
                                c_thread_desc_.CalculateOffset(make_tuple(m0, n0, 0));

                            wmma_gemm.template Run(
                                a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                                b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                                c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                        });
                    });
                });
        }
        else
        {

            static_for<0, NRepeat, 1>{}([&](auto n0) {
                static_for<0, MRepeat, 1>{}([&](auto m0) {
                    static_for<0, KPerBlock / WmmaK, 1>{}(
                        [&](auto k) { // k=0,1,2 instead of k=0,kpack*1, ...
                                      // read B
                            b_thread_copy_.Run(
                                b_block_desc_k0_n0_n1_n2_k1,
                                make_tuple(Number<k * WmmaK / B_K1 / B_KRow>{}, n0, I0, I0, I0, I0),
                                b_block_buf,
                                b_thread_desc_,
                                make_tuple(I0, n0, I0, I0, I0, I0),
                                b_thread_buf);
                            // read A
                            a_thread_copy_.Run(
                                a_block_desc_k0_m0_m1_m2_k1,
                                make_tuple(Number<k * WmmaK / A_K1 / A_KRow>{}, m0, I0, I0, I0, I0),
                                a_block_buf,
                                a_thread_desc_,
                                make_tuple(I0, m0, I0, I0, I0, I0),
                                a_thread_buf);

                            vector_type<FloatA, WmmaK> a_thread_vec;
                            vector_type<FloatB, WmmaK> b_thread_vec;

                            static_for<0, WmmaK, 1>{}([&](auto i) {
                                b_thread_vec.template AsType<FloatB>()(i) =
                                    b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                                        make_tuple(i / B_K1 / B_KRow,
                                                   n0,
                                                   0,
                                                   (i / B_K1) % B_KRow,
                                                   0,
                                                   i % B_K1))>{}];
                                a_thread_vec.template AsType<FloatA>()(i) =
                                    a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                        make_tuple(i / A_K1 / A_KRow,
                                                   m0,
                                                   0,
                                                   (i / A_K1) % A_KRow,
                                                   0,
                                                   i % A_K1))>{}];
                            });

                            using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                            using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                            constexpr index_t c_offset =
                                c_thread_desc_.CalculateOffset(make_tuple(m0, n0, 0));

                            wmma_gemm.template Run(
                                a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                                b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                                c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                        });
                });
            });
        }
    }

    protected:
    static constexpr auto a_thread_desc_ =
        make_naive_tensor_descriptor(make_tuple(Number<WmmaK / A_K1 / A_KRow>{},
                                                Number<MRepeat>{},
                                                I1,
                                                Number<A_KRow>{},
                                                I1,
                                                Number<A_K1>{}),
                                     make_tuple(Number<A_K1 * A_KRow>{},
                                                Number<WmmaK>{},
                                                Number<A_K1 * A_KRow>{},
                                                Number<A_K1>{},
                                                Number<A_K1>{},
                                                Number<1>{}));

    static constexpr auto b_thread_desc_ =
        make_naive_tensor_descriptor(make_tuple(Number<WmmaK / B_K1 / B_KRow>{},
                                                Number<NRepeat>{},
                                                I1,
                                                Number<B_KRow>{},
                                                I1,
                                                Number<B_K1>{}),
                                     make_tuple(Number<B_K1 * B_KRow>{},
                                                Number<WmmaK>{},
                                                Number<B_K1 * B_KRow>{},
                                                Number<B_K1>{},
                                                Number<B_K1>{},
                                                Number<1>{}));

    // C[M, N, NumRegWMMA]
    static constexpr auto c_thread_desc_ = make_naive_tensor_descriptor_packed(
        make_tuple(Number<MRepeat>{}, Number<NRepeat>{}, wmma_gemm.GetRegSizePerWmma()));

    template <bool EnableLds>
    struct AThreadCopySelector;

    template <>
    struct AThreadCopySelector<true>
    {
        using type =
            ThreadwiseTensorSliceTransfer_v4<FloatA,
                                             FloatA,
                                             decltype(a_block_desc_k0_m0_m1_m2_k1),
                                             decltype(a_thread_desc_),
                                             Sequence<WmmaK / A_K1 / A_KRow, 1, 1, A_KRow, 1, A_K1>,
                                             Sequence<0, 1, 2, 3, 4, 5>,
                                             5,
                                             A_K1,
                                             A_K1>;
    };

    template <>
    struct AThreadCopySelector<false>
    {
        using type = ThreadwiseTensorSliceTransfer_StaticToStatic_InterRow<
            FloatA,
            FloatA,
            decltype(a_block_desc_k0_m0_m1_m2_k1),
            decltype(a_thread_desc_),
            tensor_operation::element_wise::PassThrough,
            Sequence<WmmaK / A_K1 / A_KRow, 1, 1, 1, 1, A_K1>,
            Sequence<0, 1, 2, 3, 4, 5>,
            5,
            A_K1,
            0x76543210,
            0xfedcba98,
            TransposeC ? false : true>;
    };

    template <bool EnableLds>
    struct BThreadCopySelector;

    template <>
    struct BThreadCopySelector<true>
    {
        using type =
            ThreadwiseTensorSliceTransfer_v4<FloatB,
                                             FloatB,
                                             decltype(b_block_desc_k0_n0_n1_n2_k1),
                                             decltype(b_thread_desc_),
                                             Sequence<WmmaK / B_K1 / B_KRow, 1, 1, B_KRow, 1, B_K1>,
                                             Sequence<0, 1, 2, 3, 4, 5>,
                                             5,
                                             B_K1,
                                             B_K1>;
    };

    template <>
    struct BThreadCopySelector<false>
    {
        using type = ThreadwiseTensorSliceTransfer_StaticToStatic_InterRow<
            FloatB,
            FloatB,
            decltype(b_block_desc_k0_n0_n1_n2_k1),
            decltype(b_thread_desc_),
            tensor_operation::element_wise::PassThrough,
            Sequence<WmmaK / B_K1 / B_KRow, 1, 1, 1, 1, B_K1>,
            Sequence<0, 1, 2, 3, 4, 5>,
            5,
            B_K1,
            0x76543210,
            0xfedcba98,
            TransposeC ? true : false>;
    };

    typename AThreadCopySelector<AEnableLds>::type a_thread_copy_;
    typename BThreadCopySelector<BEnableLds>::type b_thread_copy_;
};

// block wise level pipe designed for inline asm
template <index_t BlockSize,
          typename FloatA,
          typename FloatB,
          typename FloatAcc,
          typename AK0MK1BlockDesc,
          typename BK0NK1BlockDesc,
          index_t MPerWMMA,
          index_t NPerWMMA,
          index_t MRepeat,
          index_t NRepeat,
          index_t KPack,
          bool TransposeC      = false,
          bool AssemblyBackend = true>
/* A: K0PerBlock x MPerBlock x K1
 * B: K0PerBlock x NPerBlock x K1
 * C: MRepeat x MWave x MSubGroup x NRepeat x NWave x NThreadPerSubGroup x MAccVgprs
 * KPACK == WMMA_K = 16
 */
struct BlockwiseGemmWMMA_k0mk1_k0nk1_m0m1m2n0n1n2m3_CShuffle_FIFO
{
    static constexpr auto I0    = Number<0>{};
    static constexpr auto I1    = Number<1>{};
    static constexpr auto I2    = Number<2>{};
    static constexpr auto I3    = Number<3>{};
    static constexpr auto I4    = Number<4>{};
    static constexpr auto WmmaK = Number<16>{};

    using ThisThreadBlock = ThisThreadBlock<BlockSize>;

    // Hardcode of WaveSize, since current HIP Runtime(5.4.0-10984) could not return correct one.
    static constexpr index_t WaveSize = 32;

    static constexpr index_t MPerBlock = AK0MK1BlockDesc{}.GetLength(I1);
    static constexpr index_t NPerBlock = BK0NK1BlockDesc{}.GetLength(I1);
    static constexpr index_t KPerBlock =
        BK0NK1BlockDesc{}.GetLength(I0) * BK0NK1BlockDesc{}.GetLength(I2);

    static constexpr index_t A_K0 = AK0MK1BlockDesc{}.GetLength(I0);
    static constexpr index_t B_K0 = BK0NK1BlockDesc{}.GetLength(I0);
    static constexpr index_t A_K1 = AK0MK1BlockDesc{}.GetLength(I2);
    static constexpr index_t B_K1 = BK0NK1BlockDesc{}.GetLength(I2);

    static constexpr auto wmma_gemm = WmmaGemm<FloatA,
                                               FloatB,
                                               FloatAcc,
                                               MPerWMMA,
                                               NPerWMMA,
                                               KPack,
                                               TransposeC,
                                               AssemblyBackend>{};

    static constexpr index_t MWaves = MPerBlock / (MRepeat * MPerWMMA);
    static constexpr index_t NWaves = NPerBlock / (NRepeat * NPerWMMA);

    StaticBufferTupleOfVector<AddressSpaceEnum::Vgpr,
                              FloatAcc,
                              MRepeat * NRepeat,
                              wmma_gemm.GetRegSizePerWmma(),
                              true>
        c_thread_buf_;

    __host__ __device__ constexpr auto& GetCThreadBuffer() { return c_thread_buf_; }

    __device__ static auto GetWaveIdx()
    {
        const index_t thread_id = ThisThreadBlock::GetThreadId();

        constexpr auto threadid_to_wave_idx_adaptor = make_single_stage_tensor_adaptor(
            make_tuple(make_merge_transform(make_tuple(MWaves, NWaves, WaveSize))),
            make_tuple(Sequence<0, 1, 2>{}),
            make_tuple(Sequence<0>{}));

        return threadid_to_wave_idx_adaptor.CalculateBottomIndex(make_multi_index(thread_id));
    }

    __device__ static auto CalculateAThreadOriginDataIndex()
    {
        const auto wave_idx = GetWaveIdx();

        const auto waveId_m = wave_idx[I0];

        const auto WMMA_a_idx = wmma_gemm.CalculateAThreadOriginDataIndex();
        //  |KRepeat   |MRepeat|MWave      |MLane       |KPack
        return make_tuple(0, 0, waveId_m, WMMA_a_idx, 0);
    }

    __device__ static auto CalculateBThreadOriginDataIndex()
    {
        const auto wave_idx = GetWaveIdx();

        const auto waveId_n = wave_idx[I1];

        const auto WMMA_b_idx = wmma_gemm.CalculateBThreadOriginDataIndex();
        //  |KRepeat   |NRepeat|Nwave      |NLane       |KPack
        return make_tuple(0, 0, waveId_n, WMMA_b_idx, 0);
    }

    template <index_t m0, index_t n0>
    __device__ static auto CalculateCThreadOriginDataIndex(Number<m0>, Number<n0>)
    {
        const auto wave_idx = GetWaveIdx();

        const auto waveId_m = wave_idx[I0];
        const auto waveId_n = wave_idx[I1];

        const auto blk_idx = wmma_gemm.GetBeginOfThreadBlk();

        constexpr auto mrepeat_mwave_mperWMMA_to_m_adaptor = make_single_stage_tensor_adaptor(
            make_tuple(make_unmerge_transform(make_tuple(MRepeat, MWaves, MPerWMMA))),
            make_tuple(Sequence<0>{}),
            make_tuple(Sequence<0, 1, 2>{}));

        constexpr auto nrepeat_nwave_nperWMMA_to_n_adaptor = make_single_stage_tensor_adaptor(
            make_tuple(make_unmerge_transform(make_tuple(NRepeat, NWaves, NPerWMMA))),
            make_tuple(Sequence<0>{}),
            make_tuple(Sequence<0, 1, 2>{}));

        const index_t c_thread_m = mrepeat_mwave_mperWMMA_to_m_adaptor.CalculateBottomIndex(
            make_tuple(m0, waveId_m, blk_idx[I0]))[I0];
        const index_t c_thread_n = nrepeat_nwave_nperWMMA_to_n_adaptor.CalculateBottomIndex(
            make_tuple(n0, waveId_n, blk_idx[I1]))[I0];

        return make_tuple(c_thread_m, c_thread_n);
    }

    __host__ __device__ BlockwiseGemmWMMA_k0mk1_k0nk1_m0m1m2n0n1n2m3_CShuffle_FIFO()
    {
        static_assert(AK0MK1BlockDesc::IsKnownAtCompileTime() &&
                          BK0NK1BlockDesc::IsKnownAtCompileTime(),
                      "wrong! Desc should be known at compile-time");

        static_assert(ThisThreadBlock::GetNumOfThread() == MWaves * NWaves * WaveSize,
                      "ThisThreadBlock::GetNumOfThread() != MWaves * NWaves * WaveSize\n");

        static_assert(MPerBlock % (MPerWMMA * MRepeat) == 0 &&
                          NPerBlock % (NPerWMMA * NRepeat) == 0,
                      "wrong!");
    }
    // Thread level, register decriptor. Vector-write
    __host__ __device__ static constexpr auto
    GetCThreadDescriptor_MRepeat_MWave_MSubGroup_NRepeat_NWave_NThreadPerSubGroup_MAccVgprs()
    {
        constexpr auto c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens =
            wmma_gemm.GetCMSubGroupNThreadPerSubGroupMAccVgprsThreadBlkLengths();

        constexpr auto MSubGroup          = c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens[I0];
        constexpr auto NThreadPerSubGroup = c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens[I1];
        constexpr auto MAccVgprs          = c_msubgroup_nthreadpersubgroup_maccvgprs_tblk_lens[I2];

        return make_naive_tensor_descriptor_packed(
            //        |MRepeat           |MWave |MSubGroup |NRepeat           |NWave
            //        |NThreadPerSubGroup |MAccVgprs
            make_tuple(Number<MRepeat>{},
                       I1,
                       MSubGroup,
                       Number<NRepeat>{},
                       I1,
                       NThreadPerSubGroup,
                       MAccVgprs));
    }

    template <typename CGridDesc_M_N>
    __host__ __device__ static constexpr auto
    MakeCGridDescriptor_MBlockxRepeat_MWave_MSubGroup_NBlockxRepeat_NWave_NThreadPerSubGroup_MAccVgprs(
        const CGridDesc_M_N& c_grid_desc_m_n)
    {
        const auto M = c_grid_desc_m_n.GetLength(I0);
        const auto N = c_grid_desc_m_n.GetLength(I1);

        const auto c_grid_desc_mblockxrepeat_mwave_mperwmma_nblockxrepeat_nwave_nperwmma =
            transform_tensor_descriptor(
                c_grid_desc_m_n,
                make_tuple(
                    make_unmerge_transform(make_tuple(M / (MWaves * MPerWMMA), MWaves, MPerWMMA)),
                    make_unmerge_transform(make_tuple(N / (NWaves * NPerWMMA), NWaves, NPerWMMA))),
                make_tuple(Sequence<0>{}, Sequence<1>{}),
                make_tuple(Sequence<0, 1, 2>{}, Sequence<3, 4, 5>{}));

        return wmma_gemm
            .MakeCDesc_MBlockxRepeat_MWave_MSubGroup_NBlockxRepeat_NWave_NThreadPerSubGroup_MAccVgprs(
                c_grid_desc_mblockxrepeat_mwave_mperwmma_nblockxrepeat_nwave_nperwmma);
    }

    // Provide dimension size
    __host__ __device__ static constexpr auto
    GetCBlockDescriptor_MRepeat_MWave_MSubGroup_NRepeat_NWave_NThreadPerSubGroup_MAccVgprs()
    {
        constexpr auto c_block_desc_mrepeat_mwave_mperwmma_nrepeat_nwave_nperwmma =
            make_naive_tensor_descriptor_packed(make_tuple(Number<MRepeat>{},
                                                           Number<MWaves>{},
                                                           Number<MPerWMMA>{},
                                                           Number<NRepeat>{},
                                                           Number<NWaves>{},
                                                           Number<NPerWMMA>{}));

        return wmma_gemm
            .MakeCDesc_MBlockxRepeat_MWave_MSubGroup_NBlockxRepeat_NWave_NThreadPerSubGroup_MAccVgprs(
                c_block_desc_mrepeat_mwave_mperwmma_nrepeat_nwave_nperwmma);
    }

    __host__ __device__ static constexpr auto MakeABlockDescriptor_K0_M0_M1_M2_K1()
    {
        return transform_tensor_descriptor(
            AK0MK1BlockDesc{},
            make_tuple(make_pass_through_transform(Number<A_K0>{}),
                       make_unmerge_transform(
                           make_tuple(Number<MRepeat>{}, Number<MWaves>{}, Number<MPerWMMA>{})),
                       make_pass_through_transform(Number<A_K1>{})),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2>{}),
            make_tuple(Sequence<0>{}, Sequence<1, 2, 3>{}, Sequence<4>{}));
    }

    __host__ __device__ static constexpr auto MakeBBlockDescriptor_K0_N0_N1_N2_K1()
    {
        return transform_tensor_descriptor(
            BK0NK1BlockDesc{},
            make_tuple(make_pass_through_transform(Number<B_K0>{}),
                       make_unmerge_transform(
                           make_tuple(Number<NRepeat>{}, Number<NWaves>{}, Number<NPerWMMA>{})),
                       make_pass_through_transform(Number<B_K1>{})),
            make_tuple(Sequence<0>{}, Sequence<1>{}, Sequence<2>{}),
            make_tuple(Sequence<0>{}, Sequence<1, 2, 3>{}, Sequence<4>{}));
    }

    // M0_M1_M2 = MRepeat_MWave_MPerWmma, N0_N1_N2 = NRepeat_NWave_NPerWmma
    static constexpr auto a_block_desc_k0_m0_m1_m2_k1 = MakeABlockDescriptor_K0_M0_M1_M2_K1();
    static constexpr auto b_block_desc_k0_n0_n1_n2_k1 = MakeBBlockDescriptor_K0_N0_N1_N2_K1();

    template <typename ABlockBuffer, typename BBlockBuffer, typename CThreadBuffer>
    __device__ void Run(const ABlockBuffer& a_block_buf,
                        const BBlockBuffer& b_block_buf,
                        CThreadBuffer& c_thread_buf) const
    {
        auto a_thread_buf = make_static_buffer<AddressSpaceEnum::Vgpr, FloatA>(
            a_thread_desc_.GetElementSpaceSize());
        auto b_thread_buf = make_static_buffer<AddressSpaceEnum::Vgpr, FloatB>(
            b_thread_desc_.GetElementSpaceSize());

        // TODO: Fix it, MRepeat < NRepeat
        constexpr auto RepeatDiff = MRepeat - NRepeat;

        // Read all Mrepeat, Nrepeat
        static_for<0, NRepeat, 1>{}([&](auto iN) {
            b_thread_copy_.Run(b_block_desc_k0_n0_n1_n2_k1,
                               make_tuple(I0, Number<iN>{}, I0, I0, I0),
                               b_block_buf,
                               b_thread_desc_,
                               make_tuple(I0, Number<iN>{}, I0, I0, I0),
                               b_thread_buf);
        });

        static_for<0, MRepeat, 1>{}([&](auto iM) {
            a_thread_copy_.Run(a_block_desc_k0_m0_m1_m2_k1,
                               make_tuple(I0, Number<iM>{}, I0, I0, I0),
                               a_block_buf,
                               a_thread_desc_,
                               make_tuple(I0, Number<iM>{}, I0, I0, I0),
                               a_thread_buf);
        });

        // Stage 1: Cut to Repeat Retangle to Square, assume MRepeat > NRepeat
        static_for<0, RepeatDiff, 1>{}([&](auto iCut) {
            static_for<0, NRepeat, 1>{}([&](auto iN) {
                vector_type<FloatA, WmmaK> a_thread_vec;
                vector_type<FloatB, WmmaK> b_thread_vec;

                static_for<0, WmmaK, 1>{}([&](auto iK) {
                    a_thread_vec.template AsType<FloatA>()(iK) =
                        a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                            make_tuple(iK / A_K1, iCut, 0, 0, iK % A_K1))>{}];
                    b_thread_vec.template AsType<FloatB>()(iK) =
                        b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            make_tuple(iK / B_K1, iN, 0, 0, iK % B_K1))>{}];
                });
                using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                constexpr index_t c_offset =
                    c_thread_desc_.CalculateOffset(make_tuple(iCut, iN, 0));
                // s_nop();
                wmma_gemm.template Run(
                    a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                    b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                    c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                // s_nop();
            });
            if constexpr(KPerBlock > WmmaK)
            {
                // Read Consumed Next inner loop A
                a_thread_copy_.Run(a_block_desc_k0_m0_m1_m2_k1,
                                   make_tuple(Number<WmmaK / A_K1>{}, Number<iCut>{}, I0, I0, I0),
                                   a_block_buf,
                                   a_thread_desc_,
                                   make_tuple(I0, Number<iCut>{}, I0, I0, I0),
                                   a_thread_buf);
            }
        });

        static_for<WmmaK, KPerBlock, WmmaK>{}([&](auto iWmmaK) {
            // Stage 2: Run FIFO fashion loopover in Square
            static_for<0, NRepeat, 1>{}([&](auto WmmaInnerloop) {
                // Row Repeatation
                static_for<WmmaInnerloop, NRepeat, 1>{}([&](auto iN) {
                    vector_type<FloatA, WmmaK> a_thread_vec;
                    vector_type<FloatB, WmmaK> b_thread_vec;

                    static_for<0, WmmaK, 1>{}([&](auto iK) {
                        a_thread_vec.template AsType<FloatA>()(iK) =
                            a_thread_buf[Number<a_thread_desc_.CalculateOffset(make_tuple(
                                iK / A_K1, WmmaInnerloop + RepeatDiff, 0, 0, iK % A_K1))>{}];
                        b_thread_vec.template AsType<FloatB>()(iK) =
                            b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                                make_tuple(iK / B_K1, iN, 0, 0, iK % B_K1))>{}];
                    });
                    using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                    using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                    constexpr index_t c_offset = c_thread_desc_.CalculateOffset(
                        make_tuple(WmmaInnerloop + RepeatDiff, iN, 0));
                    // s_nop();
                    wmma_gemm.template Run(
                        a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                        b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                        c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                    // s_nop();
                });

                // Read Consumed Next inner loop A
                a_thread_copy_.Run(
                    a_block_desc_k0_m0_m1_m2_k1,
                    make_tuple(
                        Number<iWmmaK / A_K1>{}, Number<WmmaInnerloop + RepeatDiff>{}, I0, I0, I0),
                    a_block_buf,
                    a_thread_desc_,
                    make_tuple(I0, Number<WmmaInnerloop + RepeatDiff>{}, I0, I0, I0),
                    a_thread_buf);

                // Col Repeatation
                static_for<WmmaInnerloop + 1 + RepeatDiff, MRepeat, 1>{}([&](auto iM) {
                    vector_type<FloatA, WmmaK> a_thread_vec;
                    vector_type<FloatB, WmmaK> b_thread_vec;

                    static_for<0, WmmaK, 1>{}([&](auto iK) {
                        a_thread_vec.template AsType<FloatA>()(iK) =
                            a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                make_tuple(iK / A_K1, iM, 0, 0, iK % A_K1))>{}];
                        b_thread_vec.template AsType<FloatB>()(iK) =
                            b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                                make_tuple(iK / B_K1, WmmaInnerloop, 0, 0, iK % B_K1))>{}];
                    });
                    using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                    using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                    constexpr index_t c_offset =
                        c_thread_desc_.CalculateOffset(make_tuple(iM, WmmaInnerloop, 0));
                    // s_nop();
                    wmma_gemm.template Run(
                        a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                        b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                        c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                    // s_nop();
                });
                // Read Consumed Next inner loop B
                b_thread_copy_.Run(
                    b_block_desc_k0_n0_n1_n2_k1,
                    make_tuple(Number<iWmmaK / B_K1>{}, Number<WmmaInnerloop>{}, I0, I0, I0),
                    b_block_buf,
                    b_thread_desc_,
                    make_tuple(I0, Number<WmmaInnerloop>{}, I0, I0, I0),
                    b_thread_buf);
            });

            // Stage 1: Cut to Repeat Retangle to Square, assume MRepeat > NRepeat
            static_for<0, RepeatDiff, 1>{}([&](auto iCut) {
                static_for<0, NRepeat, 1>{}([&](auto iN) {
                    vector_type<FloatA, WmmaK> a_thread_vec;
                    vector_type<FloatB, WmmaK> b_thread_vec;

                    static_for<0, WmmaK, 1>{}([&](auto iK) {
                        a_thread_vec.template AsType<FloatA>()(iK) =
                            a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                                make_tuple(iK / A_K1, iCut, 0, 0, iK % A_K1))>{}];
                        b_thread_vec.template AsType<FloatB>()(iK) =
                            b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                                make_tuple(iK / B_K1, iN, 0, 0, iK % B_K1))>{}];
                    });
                    using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                    using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                    constexpr index_t c_offset =
                        c_thread_desc_.CalculateOffset(make_tuple(iCut, iN, 0));
                    // s_nop();
                    wmma_gemm.template Run(
                        a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                        b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                        c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                    // s_nop();
                });
                if constexpr(KPerBlock > WmmaK)
                {
                    a_thread_copy_.Run(
                        a_block_desc_k0_m0_m1_m2_k1,
                        make_tuple(Number<(iWmmaK + WmmaK) / A_K1>{}, Number<iCut>{}, I0, I0, I0),
                        a_block_buf,
                        a_thread_desc_,
                        make_tuple(I0, Number<iCut>{}, I0, I0, I0),
                        a_thread_buf);
                }
            });
        });

        // Stage 2: Run FIFO fashion loopover in Square
        static_for<0, NRepeat, 1>{}([&](auto WmmaInnerloop) {
            // Row Repeatation
            static_for<WmmaInnerloop, NRepeat, 1>{}([&](auto iN) {
                vector_type<FloatA, WmmaK> a_thread_vec;
                vector_type<FloatB, WmmaK> b_thread_vec;

                static_for<0, WmmaK, 1>{}([&](auto iK) {
                    a_thread_vec.template AsType<FloatA>()(iK) =
                        a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                            make_tuple(iK / A_K1, WmmaInnerloop + RepeatDiff, 0, 0, iK % A_K1))>{}];
                    b_thread_vec.template AsType<FloatB>()(iK) =
                        b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            make_tuple(iK / B_K1, iN, 0, 0, iK % B_K1))>{}];
                });
                using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                constexpr index_t c_offset =
                    c_thread_desc_.CalculateOffset(make_tuple(WmmaInnerloop + RepeatDiff, iN, 0));
                // s_nop();
                wmma_gemm.template Run(
                    a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                    b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                    c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                // s_nop();
            });

            // Col Repeatation
            static_for<WmmaInnerloop + 1 + RepeatDiff, MRepeat, 1>{}([&](auto iM) {
                vector_type<FloatA, WmmaK> a_thread_vec;
                vector_type<FloatB, WmmaK> b_thread_vec;

                static_for<0, WmmaK, 1>{}([&](auto iK) {
                    a_thread_vec.template AsType<FloatA>()(iK) =
                        a_thread_buf[Number<a_thread_desc_.CalculateOffset(
                            make_tuple(iK / A_K1, iM, 0, 0, iK % A_K1))>{}];
                    b_thread_vec.template AsType<FloatB>()(iK) =
                        b_thread_buf[Number<b_thread_desc_.CalculateOffset(
                            make_tuple(iK / B_K1, WmmaInnerloop, 0, 0, iK % B_K1))>{}];
                });
                using wmma_input_type_a = typename vector_type<FloatA, WmmaK>::type;
                using wmma_input_type_b = typename vector_type<FloatB, WmmaK>::type;

                constexpr index_t c_offset =
                    c_thread_desc_.CalculateOffset(make_tuple(iM, WmmaInnerloop, 0));
                // s_nop();
                wmma_gemm.template Run(
                    a_thread_vec.template AsType<wmma_input_type_a>()(Number<0>{}),
                    b_thread_vec.template AsType<wmma_input_type_b>()(Number<0>{}),
                    c_thread_buf.GetVectorTypeReference(Number<c_offset>{}));
                // s_nop();
            });
        });
    }

    protected:
    // A[M0, M1, M2, K0 = WmmaK]
    static constexpr auto a_thread_desc_ = make_naive_tensor_descriptor_packed(
        make_tuple(Number<WmmaK / A_K1>{}, Number<MRepeat>{}, I1, I1, Number<A_K1>{}));

    // B[N0, N1, N2, K0 = WmmaK]
    static constexpr auto b_thread_desc_ = make_naive_tensor_descriptor_packed(
        make_tuple(Number<WmmaK / B_K1>{}, Number<NRepeat>{}, I1, I1, Number<B_K1>{}));

    // C[M, N, NumRegWMMA]
    static constexpr auto c_thread_desc_ = make_naive_tensor_descriptor_packed(
        make_tuple(Number<MRepeat>{}, Number<NRepeat>{}, wmma_gemm.GetRegSizePerWmma()));

    using AThreadCopy = ThreadwiseTensorSliceTransfer_v4<FloatA,
                                                         FloatA,
                                                         decltype(a_block_desc_k0_m0_m1_m2_k1),
                                                         decltype(a_thread_desc_),
                                                         Sequence<WmmaK / A_K1, 1, 1, 1, A_K1>,
                                                         Sequence<0, 1, 2, 3, 4>,
                                                         4,
                                                         A_K1,
                                                         A_K1>;

    using BThreadCopy = ThreadwiseTensorSliceTransfer_v4<FloatB,
                                                         FloatB,
                                                         decltype(b_block_desc_k0_n0_n1_n2_k1),
                                                         decltype(b_thread_desc_),
                                                         Sequence<WmmaK / B_K1, 1, 1, 1, B_K1>,
                                                         Sequence<0, 1, 2, 3, 4>,
                                                         4,
                                                         B_K1,
                                                         B_K1>;

    AThreadCopy a_thread_copy_{CalculateAThreadOriginDataIndex()};
    BThreadCopy b_thread_copy_{CalculateBThreadOriginDataIndex()};
};

} // namespace ck
