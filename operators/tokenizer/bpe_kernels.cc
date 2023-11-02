// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "bpe_tokenizer.hpp"
#include "bpe_kernels.h"

#include <optional>

std::string BpeModelConf::GetSpecialTokens() const {
  std::string special_tokens = unk_token_;  // unk_token_ is required
  auto add_token = [](std::string& sp, const char* tok) {
    if (tok != nullptr) {
      if (sp.find(tok) == std::string::npos) {
        (sp += "\n") += tok;
      }
    }
  };

  add_token(special_tokens, bos_token_);
  add_token(special_tokens, eos_token_);
  add_token(special_tokens, pad_token_);
  return special_tokens;
}

const char BpeModelConf::kModel_GPT2[] = "GPT2";
const char BpeModelConf::kModel_Roberta[] = "Roberta";
const char BpeModelConf::kModel_CLIP[] = "CLIP";

// Note: the following logic comes from CPython: unicodetype_db.h (_PyUnicode_IsWhitespace)
bool IsUnicodeSpace(char32_t ch) {
  switch (ch) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x001C:
    case 0x001D:
    case 0x001E:
    case 0x001F:
    case 0x0020:
    case 0x0085:
    case 0x00A0:
    case 0x1680:
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200A:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
      return true;
  }
  return false;
}

bool AllSpaceUstring(const ustring& str) {
  return std::all_of(str.begin(), str.end(), [](char32_t ch) { return IsUnicodeSpace(ch); });
}

ustring RemoveConsecutiveSpaces(const ustring& input) {
  ustring result;
  result.reserve(input.size());
  bool lastWasSpace = false;

  for (auto ch : input) {
    if (IsUnicodeSpace(ch)) {
      if (!lastWasSpace) {
        result.push_back(ch);
      }
      lastWasSpace = true;
    } else {
      result.push_back(ch);
      lastWasSpace = false;
    }
  }

  return result;
}

KernelBpeTokenizer::KernelBpeTokenizer(const OrtApi& api, const OrtKernelInfo& info, const BpeModelConf& conf)
    : BaseKernel(api, info),
      bpe_conf_(conf) {
  std::string vocab = ort_.KernelInfoGetAttribute<std::string>(&info, "vocab");
  if (vocab.empty()) {
    ORTX_CXX_API_THROW("vocabulary shouldn't be empty.", ORT_INVALID_ARGUMENT);
  }

  std::string merges = ort_.KernelInfoGetAttribute<std::string>(&info, "merges");
  if (merges.empty()) {
    ORTX_CXX_API_THROW("merges shouldn't be empty.", ORT_INVALID_ARGUMENT);
  }

  if (!TryToGetAttribute<int64_t>("padding_length", padding_length_)) {
    padding_length_ = -1;
  }

  if (padding_length_ != -1 && padding_length_ <= 0) {
    ORTX_CXX_API_THROW("padding_length should be more than 0 or equal -1", ORT_INVALID_ARGUMENT);
  }

  std::stringstream vocabu_stream(vocab);
  std::stringstream merges_stream(merges);
  bbpe_tokenizer_ = std::make_unique<BpeModel>();
  bbpe_tokenizer_->Load(vocabu_stream, merges_stream, conf.unk_token_, conf.GetSpecialTokens().c_str());

  // TODO: need to check if the special token ids are the same as the ones in HFTokenizer
  unk_token_id_ = bbpe_tokenizer_->GetTokenId(conf.unk_token_);
  if (conf.bos_token_ != nullptr) {
    bos_token_id_ = bbpe_tokenizer_->GetTokenId(conf.bos_token_);
  }
  if (conf.eos_token_ != nullptr) {
    eos_token_id_ = bbpe_tokenizer_->GetTokenId(conf.eos_token_);
  }
  if (conf.pad_token_ != nullptr) {
    pad_token_id_ = bbpe_tokenizer_->GetTokenId(conf.pad_token_);
  }
}

std::vector<int64_t> KernelBpeTokenizer::Tokenize(ustring& input,
                                                  int64_t max_length,
                                                  bool compute_offset_mapping,
                                                  std::list<OffsetMappingType>& offset_map) const {
  std::vector<int64_t> res;
  std::list<std::pair<uint32_t, uint32_t>> byte_list;

  // HF implements a cache for BPE:
  // https://github.com/huggingface/transformers/blob/6f316016877197014193b9463b2fd39fa8f0c8e4/src/transformers/models/gpt2/tokenization_gpt2.py#L216C6-L216C6

  // We use a LRU cache algorithm for the same in C++ in order to save compute.

  // Current cache capacity is set to a relatively small 500 in order to support mobile platforms.
  LRUCache bpe_cache = LRUCache(500);

  bool clean_up_spaces = false;
  if (ModelName() == BpeModelConf::kModel_CLIP) {
    clean_up_spaces = true;
    // Merges consecutive '\s+' for CLIP
    /*
      text = re.sub(r"\s+", " ", text)
      text = text.strip()
    */
    ustring str = RemoveConsecutiveSpaces(input);
    if (IsUnicodeSpace(str.front())) {
      str.erase(str.begin());
    }
    if (IsUnicodeSpace(str.back())) {
      str.pop_back();
    }
    // remove newlines as CLIP ignores them (treats them as whitespace which is then cleaned)
    str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
    str.erase(std::remove(str.begin(), str.end(), '\r'), str.end());
    input = str;
  }

  if (AllSpaceUstring(input) && ModelName() == BpeModelConf::kModel_CLIP) {
    // Add BOS and EOS token to result
    res.push_back(bos_token_id_);
    res.push_back(eos_token_id_);
    return res;
  }

  if (ModelName() != BpeModelConf::kModel_GPT2) {
    // Add BOS token to result
    res.push_back(bos_token_id_);
  }
  if (ModelName() == BpeModelConf::kModel_CLIP) {
    // Convert to lowercase
    std::transform(input.begin(), input.end(), input.begin(), [](char32_t c) { return static_cast<char32_t>(ToLower(c)); });
  }

  // Parse input
  auto special_token_split_res = bbpe_tokenizer_->SplitBySpecialTokens(input);
  TokenWithRegularExp regcmp;

  for (auto& seg_id : special_token_split_res) {
    if (static_cast<int64_t>(res.size()) >= max_length) break;

    if (seg_id.second != -1) {
      res.push_back(seg_id.second);
      continue;
    }

    auto cur_input = std::move(seg_id.first);
    // Note: keep ptr to make sure the string_view is valid in the following process
    const char32_t* ptr = cur_input.c_str();
    regcmp.Set(ptr);

    size_t offset = 0;
    OffsetMappingType offset_mapping;

    if (compute_offset_mapping) {
      if (ModelName() != BpeModelConf::kModel_GPT2) {
        // Add offset mapping for BOS token
        offset_mapping.push_back(std::make_pair(0, 0));
      }
    }

    while (static_cast<int64_t>(res.size()) < max_length) {
      auto [b, tok] = regcmp.GetNextToken();
      
      if (!b) break;

      std::string utf8_token = std::string(ustring(tok));

      size_t space_dif = 0;

      if (compute_offset_mapping) {
        // Handle special case for offset mapping
        if (utf8_token.at(0) == ' ') {
          offset++;
          space_dif = -1;  // account for spaces used in offset map algorithm in bpe(byte_list_)
        }
      }

      // Skip Byte-level BPE computations if input IDs and offset mapping have already been computed
      // for this token, i.e. output is in the cache.
      if (bpe_cache.already_tokenized(utf8_token)) {
        byte_list.clear();
        for (auto out : bpe_cache.get_output(utf8_token)) {
          byte_list.push_back(out);
        }
      } else {
        // Get byte encodings prior to performing BPE
        byte_list.clear();

        if (clean_up_spaces) {
          // Whitespace clean
          utf8_token.erase(std::remove(utf8_token.begin(), utf8_token.end(), ' '), utf8_token.end());

          for (int i = 0; i < utf8_token.length(); i++) {
            if (i == utf8_token.length() - 1) {
              std::string boundary(1, utf8_token[i]);
              byte_list.push_back(std::make_pair(bbpe_tokenizer_->GetTokenId(boundary + "</w>"), 1));
            } else {
              byte_list.push_back(std::make_pair(bbpe_tokenizer_->ByteEncoder()[static_cast<unsigned char>(utf8_token[i])], 1));
            }
          }
        } else {
          for (char& cp : utf8_token) {
            byte_list.push_back(std::make_pair(bbpe_tokenizer_->ByteEncoder()[static_cast<unsigned char>(cp)], 1));
          }
        }

        // Perform BPE
        bbpe_tokenizer_->bpe(byte_list);

        // Add output to cache
        bpe_cache.add(utf8_token, byte_list);
      }
      // Add output to result
      for (auto p : byte_list) {
        if (static_cast<int64_t>(res.size()) >= max_length) {
          break;
        }

        res.push_back(p.first);

        if (compute_offset_mapping) {
          if (clean_up_spaces) {
            offset_mapping.emplace_back(std::make_pair(offset, ort_extensions::narrow<size_t>(offset + p.second)));
            offset += p.second;
          } else {
            offset_mapping.emplace_back(std::make_pair(offset, ort_extensions::narrow<size_t>(offset + (size_t)p.second + space_dif)));
            offset += ((size_t)p.second + space_dif);
          }
        }
      }
    }

    if (compute_offset_mapping) {
      if (ModelName() != BpeModelConf::kModel_GPT2) {
        // Add offset mapping for EOS token
        offset_mapping.emplace_back(std::make_pair(0, 0));
      }
      // Add offset mappings for input in this instance to list of offset mappings for all inputs
      offset_map.emplace_back(offset_mapping);
    }
  }

  if (ModelName() != BpeModelConf::kModel_GPT2) {
    // Add EOS token to result
    res.push_back(eos_token_id_);
  }
  return res;
}

void KernelBpeTokenizer::Compute(const ortc::Tensor<std::string>& input,
                                 ortc::Tensor<int64_t>& tokenize_output,
                                 std::optional<ortc::Tensor<int64_t>*> attention_mask,
                                 std::optional<ortc::Tensor<int64_t>*> offset_mapping) const {
  // Setup inputs
  std::vector<std::string> str_input{input.Data()};
  std::list<OffsetMappingType> offset_map;
  const auto& input_dim = input.Shape();

  std::vector<std::vector<int64_t>> tokenize_results;

  // Only compute offset mapping if optional output for it exists.
  bool compute_offset_mapping = false;
  if (offset_mapping.has_value()) {
    compute_offset_mapping = true;
  }

  for (auto& str : str_input) {
    ustring ustr = ustring(str);
    tokenize_results.emplace_back(
        Tokenize(
            ustr,
            padding_length_ < 0 ? std::numeric_limits<uint32_t>::max() : padding_length_,
            compute_offset_mapping,
            offset_map));
  }

  size_t max_length = 0;
  if (padding_length_ == -1) {
    for (auto& res : tokenize_results) {
      max_length = std::max(max_length, res.size());
    }
  } else {
    max_length = static_cast<size_t>(padding_length_);
  }

  std::vector<int64_t> output_dim = input_dim;
  output_dim.push_back(max_length);

  std::vector<int64_t> offset_dim = output_dim;
  offset_dim.push_back(2);  // tuple of offsets for each input id

  auto* token = tokenize_output.Allocate(output_dim);
  if (attention_mask.has_value()) {
    auto* mask = (*attention_mask)->Allocate(output_dim);
    int idx = 0;
    for (auto& res : tokenize_results) {
      for (int64_t id : res) {
        mask[idx] = 1;
        idx++;
      }

      for (size_t i = res.size(); i < max_length; i++) {
        mask[idx] = 0;
        idx++;
      }
    }
  }
  if (offset_mapping.has_value()) {
    auto* offset = (*offset_mapping)->Allocate(offset_dim);
    int idx2 = 0;
    for (auto& res : offset_map) {
      for (auto& mapping : res) {
        offset[idx2] = mapping.first;
        idx2++;
        offset[idx2] = mapping.second;
        idx2++;
      }
    }
  }
  int idx = 0;
  for (auto& res : tokenize_results) {
    for (int64_t id : res) {
      token[idx] = id;
      idx++;
    }

    for (size_t i = res.size(); i < max_length; i++) {
      token[idx] = pad_token_id_;
      idx++;
    }
  }
}

static const auto kGPT2Confinguration = BpeModelConf();
GPT2Tokenizer::GPT2Tokenizer(const OrtApi& api, const OrtKernelInfo& info)
    : KernelBpeTokenizer(api, info, kGPT2Confinguration) {}

static const auto kRobertaConfiguration = BpeModelConf{
    BpeModelConf::kModel_Roberta,  // name
    "<unk>",                       // unk_token
    "<s>",                         // bos_token
    "</s>",                        // eos_token
    "<pad>"};                      // pad_token

RobertaTokenizer::RobertaTokenizer(const OrtApi& api, const OrtKernelInfo& info)
    : KernelBpeTokenizer(api, info, kRobertaConfiguration) {}

static const auto kCLIPConfiguration = BpeModelConf{
    BpeModelConf::kModel_CLIP,  // name
    "<|endoftext|>",            // unk_token
    "<|startoftext|>",          // bos_token
    "<|endoftext|>",            // eos_token
    "<|endoftext|>"};           // pad_token

CLIPTokenizer::CLIPTokenizer(const OrtApi& api, const OrtKernelInfo& info)
    : KernelBpeTokenizer(api, info, kCLIPConfiguration) {}
