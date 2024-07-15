################################################################################
#
# Copyright 2023 ByteDance Ltd. and/or its affiliates. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

import argparse
from functools import partial
import os
import torch
import time
import datetime
import numpy as np
import flux
from typing import List
import itertools
import dataclasses
from flux import pynvshmem

RANK = int(os.environ.get("RANK", 0))
LOCAL_RANK = int(os.environ.get("LOCAL_RANK", 0))
LOCAL_WORLD_SIZE = int(os.environ.get("LOCAL_WORLD_SIZE", 1))
WORLD_SIZE = int(os.environ.get("WORLD_SIZE", 1))
NNODES = WORLD_SIZE // LOCAL_WORLD_SIZE
torch.cuda.set_device(LOCAL_RANK)

os.environ["NCCL_DEBUG"] = "ERROR"
os.environ["CUBLAS_WORKSPACE_CONFIG"] = ":16:8"
torch.use_deterministic_algorithms(True, warn_only=True)
torch.set_printoptions(precision=8)
torch.manual_seed(3 + RANK)
torch.cuda.manual_seed_all(3 + RANK)
torch.backends.cudnn.deterministic = True
torch.backends.cudnn.benchmark = False
torch.backends.cuda.matmul.allow_tf32 = False
np.random.seed(3 + RANK)

torch.distributed.init_process_group(
    backend="nccl", world_size=WORLD_SIZE, rank=RANK, timeout=datetime.timedelta(seconds=1800)
)
# use all ranks as tp group
TP_GROUP = torch.distributed.new_group(ranks=list(range(WORLD_SIZE)), backend="nccl")
print = partial(print, flush=True)


@dataclasses.dataclass
class TuningConfig:
    M: int
    N: int
    K: int
    transpose_weight: bool
    dtype: str
    has_bias: bool


def gen_tuning_space():
    space: List[TuningConfig] = []
    ## Training shapes
    # space_M = [4096]
    # space_N = [10240, 24576, 8192, 57344, 28672]
    # space_K = [8192]
    space_M = [64, 256, 512, 1024, 2048, 4096, 8192]
    space_N = [16384, 49152]
    space_K = [12288]
    space_transpose_weight = [True, False]
    space_dtype = [torch.bfloat16, torch.float16]
    space_has_bias = [False, True]
    for M, N, K, transpose_weight, dtype, has_bias in itertools.product(
        space_M,
        space_N,
        space_K,
        space_transpose_weight,
        space_dtype,
        space_has_bias,
    ):
        config = TuningConfig(
            M=M,
            N=N,
            K=K,
            transpose_weight=transpose_weight,
            dtype=dtype,
            has_bias=has_bias,
        )
        space.append(config)
    return space


def get_torch_output(input: torch.Tensor, weight: torch.Tensor, bias: torch.Tensor):
    local_M = input.size(0)
    M = local_M * TP_GROUP.size()

    torch.distributed.barrier()
    full_input = torch.zeros(
        (M, input.size(1)),
        dtype=input.dtype,
        device=torch.cuda.current_device(),
        requires_grad=False,
    )
    torch.distributed.all_gather_into_tensor(full_input, input, group=TP_GROUP)
    output = torch.matmul(full_input, weight.t())
    if bias is not None:
        output += bias
    torch.distributed.barrier()
    return output


def run_flux_profiling(
    prof_ctx: flux.ProfilingContext,
    input: torch.Tensor,
    weight: torch.Tensor,
    bias: torch.Tensor,
    config: TuningConfig,
):
    local_M = input.size(0)
    M = local_M * TP_GROUP.size()
    K = input.size(1)

    if config.transpose_weight:
        w = weight.t().contiguous()
        N = w.size(1)
    else:
        w = weight
        N = w.size(0)

    ag_gemm_kernel = flux.AGKernel(
        TP_GROUP,
        NNODES,
        M,
        N,
        K,
        input.dtype,
        transpose_weight=config.transpose_weight,
        local_copy=True,
    )

    output = ag_gemm_kernel.profiling(input, w, bias=bias, prof_ctx=prof_ctx)
    return output


def tune_one_config(prof_ctx: flux.ProfilingContext, config: TuningConfig):
    assert config.M % TP_GROUP.size() == 0
    assert config.N % TP_GROUP.size() == 0
    local_M = config.M // TP_GROUP.size()
    local_N = config.N // TP_GROUP.size()

    # input: [M, K], weight: [N, K]
    input = (
        torch.rand((local_M, config.K), dtype=config.dtype).cuda() / 100 * ((TP_GROUP.rank() + 1))
    )
    weight = (
        torch.rand((local_N, config.K), dtype=config.dtype).cuda() / 100 * ((TP_GROUP.rank() + 1))
    )

    bias = None
    if config.has_bias:
        bias = torch.rand((config.M, local_N), dtype=input.dtype, device=input.device)

    torch_output = get_torch_output(input, weight, bias)
    torch.distributed.barrier()
    flux_output = run_flux_profiling(prof_ctx, input, weight, bias, config)
    torch.distributed.barrier()
    torch.cuda.current_stream().synchronize()

    if config.dtype == torch.bfloat16:
        atol, rtol = 0.02, 0.02
    else:
        atol, rtol = 0.01, 0.01

    for rank in range(WORLD_SIZE):
        if rank == RANK:
            flux.torch_allclose(flux_output.cpu(), torch_output.cpu(), atol=atol, rtol=rtol)
        torch.distributed.barrier()

    time.sleep(1)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output_dir", default="", type=str, help="Directory to store generated files"
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    if args.output_dir and not os.path.isdir(args.output_dir):
        raise Exception(f"{args.output_dir} not exist")

    torch.cuda.set_device(LOCAL_RANK)
    flux.init_flux_shm(TP_GROUP)
    torch.cuda.synchronize()

    arch: int = flux.get_arch()
    name: str = f"config_ag_gemm_kernel_sm{arch}_tp{WORLD_SIZE}_nnodes{NNODES}"
    prof_ctx = flux.ProfilingContext(name)
    config_space = gen_tuning_space()
    for i, config in enumerate(config_space):
        if TP_GROUP.rank() == 0:
            print(f"==== #{i + 1}/{len(config_space)} Tuning for {config}")
        tune_one_config(prof_ctx=prof_ctx, config=config)
        if TP_GROUP.rank() == 0:
            print(prof_ctx.get_latest_prof_result())

    if TP_GROUP.rank() == 0:
        if os.path.isdir(args.output_dir):
            code_path = os.path.join(args.output_dir, f"{name}.cu")
            result_path = os.path.join(args.output_dir, f"{name}.prof.log")

            with open(code_path, "w") as fout:
                print(prof_ctx.get_code(), file=fout)

            with open(result_path, "w") as fout:
                for record in prof_ctx.get_all_prof_results():
                    print(record, file=fout)
        else:
            print("Generated Code:")
            print(prof_ctx.get_code())
            print()

            print("Profiling Results:")
            for record in prof_ctx.get_all_prof_results():
                print(record)
