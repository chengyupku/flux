//===- generator_utils.h ----------------------------------------- C++ ---===//
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
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <set>
#include <regex>
#include <fstream>
#include <vector>
#include "flux/flux.h"
#include "flux/gemm_meta.h"
#include "flux/gemm_hparams.h"

#include "cutlass/util/command_line.h"

namespace bytedance::flux::generator {

struct Options {
  bool help;
  std::string dir;
  std::string archs;
  std::string output_file;

  Options() : help(false), dir("./registers"), archs(""), output_file("./generated_ops.txt") {}
  void
  parse(int argc, char const **args) {
    cutlass::CommandLine cmd(argc, args);
    if (cmd.check_cmd_line_flag("help")) {
      help = true;
      return;
    }
    cmd.get_cmd_line_argument("dir", dir);
    cmd.get_cmd_line_argument("archs", archs);
    cmd.get_cmd_line_argument("output_file", output_file);
  }

  std::ostream &
  print_usage(std::ostream &out) const {
    out << "generator\n\n"
        << " generate all flux ops, one op per file.\n\n"
        << "Options:\n\n"
        << "   --help           If specified, displays this usage statement\n\n"
        << "   --dir            Store generated op registry files under this dir\n\n"
        << "   --archs          Comma seperated, only specified SM number will be compiled\n\n"
        << "   --output_file    The filepaths of generated ops will be stored into this file";
    return out;
  }
};

inline std::vector<std::string>
parse_semicolon_seperated(std::string str) {
  std::vector<std::string> str_vec;
  for (int i = 0, prev = -1, sz = int(str.size()); i <= sz; ++i) {
    if (i == sz || str[i] == ';') {
      if (prev + 1 < i) {
        str_vec.emplace_back(str.substr(prev + 1, i - prev - 1));
      }
      prev = i;
    }
  }
  return str_vec;
}

inline void
write_if_changed(const std::string &file_path, const std::string &new_content) {
  namespace fs = std::filesystem;
  if (fs::exists(file_path)) {
    std::ifstream file(file_path, std::ios::in);
    if (file) {
      std::string existing_content(
          (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      file.close();

      if (existing_content == new_content) {
        return;
      }
    }
  }

  std::ofstream file(file_path, std::ios::out);
  if (file) {
    file << new_content;
    file.close();
  }
}

inline void
clear_old_files(const std::set<std::string> &all_file_paths) {
  namespace fs = std::filesystem;

  for (const auto &entry : fs::directory_iterator(fs::current_path())) {
    std::string file_path = entry.path().filename().string();
    if (all_file_paths.find(file_path) == all_file_paths.end()) {
      try {
        fs::remove(entry.path());
        std::cout << "File " << file_path << " removed successfully." << std::endl;
      } catch (const fs::filesystem_error &e) {
        std::cout << "Error removing file " << file_path << ": " << e.what() << std::endl;
      }
    }
  }
}

class CodeGen {
 private:
  std::string impl_header;
  std::string impl_name;
  std::string meta_kernel_name;
  std::string make_meta_str;
  std::vector<std::string> make_hparams_str_list;
  std::vector<std::string> hparams_kernel_name_list;

 private:
  std::string
  gen_header() const {
    static const std::string tpl = R"(
// Auto-Generated by generator.cc - Do not edit.
#include "@IMPL_HEADER@"
using namespace bytedance::flux;
using namespace cute;

using _GemmMetaT = decltype(@MAKE_META_STR@);
struct @GEMM_META_ALIAS@ : public _GemmMetaT {
  using _GemmMetaT::_GemmMetaT;
};
using GemmMetaT = @GEMM_META_ALIAS@;

)";
    std::string code = std::regex_replace(tpl, std::regex("@IMPL_HEADER@"), impl_header);
    code = std::regex_replace(code, std::regex("@MAKE_META_STR@"), make_meta_str);
    code = std::regex_replace(code, std::regex("@GEMM_META_ALIAS@"), "_" + meta_kernel_name);
    return code;
  }

  std::string
  gen_body() const {
    static const std::string tpl = R"(
using _GemmHParamsT_@REG_IDX@ = decltype(@MAKE_HPARAMS_STR@);
struct @GEMM_HPARAMS_ALIAS@ : public _GemmHParamsT_@REG_IDX@ {
  using _GemmHParamsT_@REG_IDX@::_GemmHParamsT_@REG_IDX@;
};
using GemmHParamsT_@REG_IDX@ = @GEMM_HPARAMS_ALIAS@;
using KernelBuilder_@REG_IDX@ = @IMPL_NAME@_Kernel<GemmMetaT, GemmHParamsT_@REG_IDX@>;
using GemmKernel_@REG_IDX@ = decltype(KernelBuilder_@REG_IDX@().gemm_kernel());
struct @OP_NAME@: public GemmKernel_@REG_IDX@ {};
using GemmDevice_@REG_IDX@ = @IMPL_NAME@_Device<GemmMetaT, GemmHParamsT_@REG_IDX@, @OP_NAME@>;

)";
    std::stringstream ss;
    for (int i = 0; i < int(make_hparams_str_list.size()); ++i) {
      std::string code = std::regex_replace(tpl, std::regex("@REG_IDX@"), std::to_string(i));
      code = std::regex_replace(code, std::regex("@MAKE_HPARAMS_STR@"), make_hparams_str_list[i]);
      code = std::regex_replace(
          code, std::regex("@GEMM_HPARAMS_ALIAS@"), "_" + hparams_kernel_name_list[i]);
      code = std::regex_replace(code, std::regex("@IMPL_NAME@"), impl_name);
      code = std::regex_replace(
          code,
          std::regex("@OP_NAME@"),
          "flux_" + meta_kernel_name + "_" + hparams_kernel_name_list[i]);
      ss << code;
    }
    return std::move(ss).str();
  }

  std::string
  gen_tail() const {
    static const std::string tpl =
        R"(  OpRegistry::instance().register_creator([]() { return std::make_unique<GemmDevice_@REG_IDX@>(); }, GemmMetaT{},GemmHParamsT_@REG_IDX@{},@REG_IDX@))";
    std::stringstream ss;
    ss << "static bool _dummy_reg [[maybe_unused]] = (\n";
    for (int i = 0; i < int(make_hparams_str_list.size()); ++i) {
      std::string op_code = std::regex_replace(tpl, std::regex("@REG_IDX@"), std::to_string(i));
      ss << op_code << ",\n";
    }
    ss << "true);";
    return std::move(ss).str();
  }

 public:
  CodeGen(
      UnifiedGemmMeta const &meta,
      std::vector<UnifiedGemmHParams> const &hparams_list,
      std::string const &impl_header,
      std::string const &impl_name)
      : impl_header(impl_header), impl_name(impl_name) {
    this->make_meta_str = to_make_constexpr(meta);
    this->meta_kernel_name = to_kernel_name(meta);
    for (auto const &hparams : hparams_list) {
      this->make_hparams_str_list.emplace_back(to_make_constexpr(hparams));
      this->hparams_kernel_name_list.emplace_back(to_kernel_name(hparams));
    }
  }

  std::string
  gen_code() const {
    std::stringstream ss;
    ss << gen_header();
    ss << gen_body();
    ss << gen_tail();
    return std::move(ss).str();
  }

  std::string
  get_filename() const {
    return "flux_" + meta_kernel_name + ".cu";
  }
};

template <class... Ts, class... Us>
auto
build_gen_space(cute::tuple<Ts...> meta_space, cute::tuple<Us...> hparams_space) {
  std::vector<std::pair<UnifiedGemmMeta, std::vector<UnifiedGemmHParams>>> gen_space;
  tuple_for_each(meta_space, [&](auto cmeta) {
    std::vector<UnifiedGemmHParams> hparams_list;
    tuple_for_each(hparams_space, [&](auto _chparams) {
      auto chparams = materialize_hparams(cmeta, _chparams);
      if constexpr (not detail::filter_smem(cmeta, chparams)) {
        return;
      }
      hparams_list.emplace_back(unify_type(chparams));
    });
    gen_space.emplace_back(unify_type(cmeta), std::move(hparams_list));
  });
  return gen_space;
}

inline auto
merge_gen_space(
    std::initializer_list<std::vector<std::pair<UnifiedGemmMeta, std::vector<UnifiedGemmHParams>>>>
        spaces) {
  std::vector<std::pair<UnifiedGemmMeta, std::vector<UnifiedGemmHParams>>> merged;
  for (auto const &space : spaces) {
    for (auto const &val : space) {
      merged.emplace_back(val);
    }
  }
  return merged;
}

// `spaces` is a tuple of [meta-hparams-pairs, impl_header, impl_name]
int
main_template(
    Options const &options,
    std::initializer_list<cute::tuple<
        std::vector<std::pair<UnifiedGemmMeta, std::vector<UnifiedGemmHParams>>>,
        std::string,
        std::string>> const &spaces) {
  std::set<ArchEnum> archs;
  std::vector<std::string> archs_vec = parse_semicolon_seperated(options.archs);
  for (auto arch : archs_vec) {
    int arch_num = std::stoi(arch);
    archs.insert(ArchEnum(arch_num));
  }

  std::ofstream ofile(options.output_file);
  if (!ofile.is_open()) {
    std::cerr << "Error opening file!" << std::endl;
    return 1;
  }

  std::filesystem::current_path(options.dir);
  using namespace bytedance::flux;
  std::set<std::string> all_file_paths;

  for (auto const &space : spaces) {
    auto [meta_item_list, impl_header, impl_name] = space;

    for (auto const &meta_item : meta_item_list) {
      auto [meta, hparams_list] = meta_item;
      ArchEnum arch = meta.arch();
      if (archs.count(arch) <= 0) {
        continue;
      }
      CodeGen gen(meta, hparams_list, impl_header, impl_name);
      std::string filename = gen.get_filename();
      std::string code = gen.gen_code();
      all_file_paths.emplace(filename);
      write_if_changed(filename, code);
    }
  }
  namespace fs = std::filesystem;
  for (auto &file_path : all_file_paths) {
    std::string abs_file_path = fs::absolute(fs::path(file_path)).string();
    ofile << abs_file_path << "\n";
  }
  clear_old_files(all_file_paths);
  ofile.close();
  return 0;
}
}  // namespace bytedance::flux::generator
