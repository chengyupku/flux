//===- gen_gemm_rs.cc -------------------------------------------- C++ ---===//
//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
#include "flux/flux.h"
#include "./generator_utils.h"
#include "flux/gemm_hparams.h"
#include "flux/gemm_meta.h"

namespace bytedance::flux::generator {
using namespace cute;

struct GemmV2ReduceScatter_Space {
  static constexpr auto AllGemmMeta_FP16 = tuple_filter(
      make_space_gemm_meta(
          cute::make_tuple(
              make_gemm_dtype_config(_FP16{}),
              make_gemm_dtype_config(_BF16{}),
              make_gemm_dtype_config(_FP16{}, _FP16{}, _Void{}, _FP16{}),
              make_gemm_dtype_config(_BF16{}, _BF16{}, _Void{}, _BF16{})),
          cute::make_tuple(_Sm80{}, _Sm89{}),
          cute::make_tuple(_ReduceScatter{}),
          cute::make_tuple(_RCR{}, _RRR{}),
          cute::make_tuple(_GemmV2{}),
          cute::make_tuple(None{}),
          tuple_transform(
              tuple_cartesian_product(
                  cute::make_tuple(_True{}, _False{}),
                  cute::make_tuple(_IntraNode{}, _AcrossNode{}, _IntraNodePcie{})),
              [](auto tup) { return to_reduce_scatter_meta(tup); })),
      [](const auto tup) {
        auto meta = to_gemm_meta(tup);
        auto rs_meta = to_reduce_scatter_meta(meta.comm_spec());
        // PCI-e don't support global_red with atomic: and with higher latency
        if constexpr (
            rs_meta.comm_kind() == _IntraNodePcie{} && rs_meta.fuse_reduction() == _True{}) {
          return false;
        }
        // CUDA arch < 90 don't support red instruction.
        auto dt_conf = to_gemm_dtype_config(make_gemm_dtype_config(meta.dtype()));
        if constexpr (dt_conf.a() == _BF16{} && rs_meta.fuse_reduction() == _True{}) {
          return false;
        }
        return true;
      });

  static constexpr auto AllGemmHParams_FP16 = make_space_gemm_hparams(
      cute::make_tuple(Auto{}),
      cute::make_tuple(Auto{}),
      cute::make_tuple(
          Shape<_128, _128, _32>{}, Shape<_128, _128, _64>{}, Shape<_128, _256, _32>{}),
      cute::make_tuple(Auto{}),
      cute::make_tuple(cute::_3{}, cute::_4{}));

  static constexpr auto AllGemmMeta_FP8 = make_space_gemm_meta(
      cute::make_tuple(
          make_gemm_dtype_config(_E4M3{}, _E4M3{}, _Void{}, _BF16{}),
          make_gemm_dtype_config(_E4M3{}, _E4M3{}, _BF16{}, _BF16{}),
          make_gemm_dtype_config(_E5M2{}, _E5M2{}, _Void{}, _BF16{}),
          make_gemm_dtype_config(_E5M2{}, _E5M2{}, _BF16{}, _BF16{})),
      cute::make_tuple(_Sm89{}),
      cute::make_tuple(_ReduceScatter{}),
      cute::make_tuple(_RCR{}),
      cute::make_tuple(_GemmV2{}),
      cute::make_tuple(make_gemm_v2_meta(_True{}), make_gemm_v2_meta(_False{})),
      cute::make_tuple(make_reduce_scatter_meta(_False{}, _IntraNodePcie{})));

  static constexpr auto AllGemmHParams_FP8 = make_space_gemm_hparams(
      cute::make_tuple(Auto{}),
      cute::make_tuple(Auto{}),
      cute::make_tuple(Auto{}),
      cute::make_tuple(Auto{}),
      cute::make_tuple(Auto{}),
      cute::make_tuple(Auto{}));

  static constexpr auto AllGemmMeta_S8 = make_space_gemm_meta(
      cute::make_tuple(
          make_gemm_dtype_config(_S8{}, _S8{}, _BF16{}, _BF16{}, _S32{}),
          make_gemm_dtype_config(_S8{}, _S8{}, _Void{}, _BF16{}, _S32{})),
      cute::make_tuple(_Sm80{}, _Sm89{}),
      cute::make_tuple(_ReduceScatter{}),
      cute::make_tuple(_RCR{}),
      cute::make_tuple(_GemmV2{}),
      cute::make_tuple(make_gemm_v2_meta(_False{})),
      cute::make_tuple(
          make_reduce_scatter_meta(_False{}, _IntraNode{}),
          make_reduce_scatter_meta(_False{}, _IntraNodePcie{})));

  static constexpr auto AllGemmHParams_S8 = tuple_cat(
      make_space_gemm_hparams(
          cute::make_tuple(
              make_gemm_v2_hparams(Shape<_64, _32, _128>{}, Shape<_16, _8, _32>{}, _StreamkSK{}),
              make_gemm_v2_hparams(Shape<_64, _32, _128>{}, Shape<_16, _8, _32>{}, _StreamkDP{})),
          cute::make_tuple(Auto{}),
          cute::make_tuple(
              Shape<_128, _64, _128>{}, Shape<_128, _128, _128>{}, Shape<_64, _64, _128>{}),
          cute::make_tuple(Auto{}),
          cute::make_tuple(cute::_3{}, cute::_4{}),
          cute::make_tuple(_RasterAlongM{}, _RasterAlongN{})),
      make_space_gemm_hparams(
          cute::make_tuple(
              make_gemm_v2_hparams(Shape<_64, _64, _128>{}, Shape<_16, _8, _32>{}, _StreamkSK{}),
              make_gemm_v2_hparams(Shape<_64, _64, _128>{}, Shape<_16, _8, _32>{}, _StreamkDP{})),
          cute::make_tuple(Auto{}),
          cute::make_tuple(cute::Shape<cute::_128, cute::_128, cute::_128>{}),
          cute::make_tuple(Auto{}),
          cute::make_tuple(cute::_4{}),
          cute::make_tuple(_RasterAlongM{}, _RasterAlongN{})));

  static auto
  get_space() {
    return merge_gen_space({
        build_gen_space(AllGemmMeta_FP16, AllGemmHParams_FP16),
        build_gen_space(AllGemmMeta_FP8, AllGemmHParams_FP8),
        build_gen_space(AllGemmMeta_S8, AllGemmHParams_S8),
    });
  }
};

struct GemmV3ReduceScatter_Space {
  static constexpr auto AllGemmMeta_FP16 = tuple_filter(
      make_space_gemm_meta(
          cute::make_tuple(
              make_gemm_dtype_config(_FP16{}),
              make_gemm_dtype_config(_BF16{}),
              make_gemm_dtype_config(_FP16{}, _FP16{}, _Void{}, _FP16{}),
              make_gemm_dtype_config(_BF16{}, _BF16{}, _Void{}, _BF16{})),
          cute::make_tuple(_Sm90{}),
          cute::make_tuple(_ReduceScatter{}),
          cute::make_tuple(_RCR{}, _RRR{}),
          cute::make_tuple(_GemmV3{}),
          cute::make_tuple(make_gemm_v3_meta(_True{}), make_gemm_v3_meta(_False{})),
          tuple_transform(
              tuple_cartesian_product(
                  cute::make_tuple(_True{}, _False{}),
                  cute::make_tuple(_IntraNode{}, _AcrossNode{})),
              [](auto tup) { return to_reduce_scatter_meta(tup); })),
      [](auto meta_tuple) {
        constexpr auto meta = to_gemm_meta(decltype(meta_tuple){});
        constexpr auto rs_meta = to_reduce_scatter_meta(meta.comm_spec());
        return not(meta.arch() == _Sm80{} and meta.gemm_layout() == _RRR{}) and
               not(rs_meta.fuse_reduction() == _False{} and rs_meta.comm_kind() == _AcrossNode{});
      });

  static constexpr auto AllGemmHParams_FP16 = make_space_gemm_hparams(cute::make_tuple(
      make_gemm_v3_hparams(Shape<_2, _1, _1>{}), make_gemm_v3_hparams(Shape<_1, _2, _1>{})));

  static constexpr auto AllGemmMeta_S8 = make_space_gemm_meta(
      cute::make_tuple(
          make_gemm_dtype_config(_S8{}, _S8{}, _BF16{}, _BF16{}, _S32{}),
          make_gemm_dtype_config(_S8{}, _S8{}, _Void{}, _BF16{}, _S32{})),
      cute::make_tuple(_Sm90{}),
      cute::make_tuple(_ReduceScatter{}),
      cute::make_tuple(_RCR{}),
      cute::make_tuple(_GemmV3{}),
      cute::make_tuple(make_gemm_v3_meta(_True{}), make_gemm_v3_meta(_False{})),
      cute::make_tuple(make_reduce_scatter_meta(_False{}, _IntraNode{})));

  static constexpr auto AllGemmHParams_S8 = tuple_cat(
      make_space_gemm_hparams(
          cute::make_tuple(
              make_gemm_v3_hparams(Shape<_1, _2, _1>{}, _PingPong{}),
              make_gemm_v3_hparams(Shape<_2, _1, _1>{}, _PingPong{})),
          cute::make_tuple(Auto{}),
          cute::make_tuple(Shape<_128, _128, _128>{}),
          cute::make_tuple(_GemmDefault{}),
          cute::make_tuple(cute::_4{}),
          cute::make_tuple(_RasterAlongM{})),
      make_space_gemm_hparams(
          cute::make_tuple(
              make_gemm_v3_hparams(Shape<_2, _1, _1>{}, _PingPong{}),
              make_gemm_v3_hparams(Shape<_1, _2, _1>{}, _PingPong{})),
          cute::make_tuple(Auto{}),
          cute::make_tuple(Shape<_64, _128, _128>{}),
          cute::make_tuple(_GemmDefault{}),
          cute::make_tuple(cute::_2{}, cute::_4{}, cute::_8{}),
          cute::make_tuple(_RasterAlongM{})));

  static auto
  get_space() {
    return merge_gen_space({
        build_gen_space(AllGemmMeta_FP16, AllGemmHParams_FP16),
        build_gen_space(AllGemmMeta_S8, AllGemmHParams_S8),
    });
  }
};

}  // namespace bytedance::flux::generator

int
main(int argc, char const **args) {
  using namespace bytedance::flux::generator;
  Options options;
  options.parse(argc, args);

  if (options.help) {
    options.print_usage(std::cout) << std::endl;
    return 0;
  }
  std::cout << "Running gemm_rs generator...\n";
  return main_template(
      options,
      {
          cute::make_tuple(
              GemmV2ReduceScatter_Space::get_space(),
              std::string("gemm_rs/gemm_v2_reduce_scatter.hpp"),
              std::string("GemmV2ReduceScatter")),
          cute::make_tuple(
              GemmV3ReduceScatter_Space::get_space(),
              std::string("gemm_rs/gemm_v3_reduce_scatter.hpp"),
              std::string("GemmV3ReduceScatter")),
      });
}
