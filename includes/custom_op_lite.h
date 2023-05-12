#pragma once
#include "onnxruntime_customop.hpp"
#include <optional>
#include <numeric>

namespace Ort {
namespace Custom {

class TensorBase {
 public:
  TensorBase(const OrtW::CustomOpApi& ort_api,
             OrtKernelContext* ctx,
             size_t indice,
             bool is_input) : ort_api_(ort_api),
                              ctx_(ctx),
                              indice_(indice),
                              is_input_(is_input) {}

  virtual ~TensorBase() = default;
  operator bool() const {
    return shape_.has_value();
  }
  const std::vector<int64_t>& Shape() const {
    if (shape_.has_value()) {
      return *shape_;
    } else {
      ORTX_CXX_API_THROW("tensor shape is not yet initialized", ORT_RUNTIME_EXCEPTION);
    }
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1LL, std::multiplies<int64_t>());
    } else {
      ORTX_CXX_API_THROW("tensor shape is not yet initialized", ORT_RUNTIME_EXCEPTION);
    }
  }

 protected:
  const OrtW::CustomOpApi& ort_api_;
  OrtKernelContext* ctx_;
  size_t indice_;
  bool is_input_;
  std::optional<std::vector<int64_t>> shape_;
};

template <typename T>
struct Span {
  const T* data_ = {};
  size_t size_ = {};
  void Assign(const T* data, size_t size) {
    data_ = data;
    size_ = size;
  }
  size_t size() const { return size_; }
  T operator[](size_t indice) const {
    return data_[indice];
  }
  const T* Data() const { return data_; }
};

template <typename T>
class Tensor : public TensorBase {
 public:
  using TT = typename std::remove_reference<T>::type;
  Tensor(const OrtW::CustomOpApi& ort_api,
         OrtKernelContext* ctx,
         size_t indice,
         bool is_input) : TensorBase(ort_api,
                                     ctx,
                                     indice,
                                     is_input) {
    if (is_input) {
      auto input_count = ort_api.KernelContext_GetInputCount(ctx_);
      if (indice >= input_count) {
        ORTX_CXX_API_THROW("invalid indice", ORT_RUNTIME_EXCEPTION);
      }
      const_value_ = ort_api.KernelContext_GetInput(ctx_, indice);
      auto* info = ort_api.GetTensorTypeAndShape(const_value_);
      shape_ = ort_api.GetTensorShape(info);
      ort_api.ReleaseTensorTypeAndShapeInfo(info);
    }
  }
  const TT* Data() const {
    return ort_api_.GetTensorData<TT>(const_value_);
  }
  TT* Allocate(const std::vector<int64_t>& shape) {
    if (!data_) {
      OrtValue* out = ort_api_.KernelContext_GetOutput(ctx_, indice_, shape.data(), shape.size());
      shape_ = shape;
      data_ = ort_api_.GetTensorMutableData<TT>(out);
    }
    return data_;
  }
  const Span<T>& AsSpan() {
    if (!shape_.has_value() || shape_->size() != 1) {
      ORTX_CXX_API_THROW("to get a span, shape must be 1-D", ORT_RUNTIME_EXCEPTION);
    }
    span_.Assign(Data(), (*shape_)[0]);
    return span_;
  }
  const T& AsScalar() {
    if (!shape_.has_value() || shape_->size() != 1 || (*shape_)[0] != 1) {
      ORTX_CXX_API_THROW("to get a scalar, shape must be {1}", ORT_RUNTIME_EXCEPTION);
    }
    return *Data();
  }

 private:
  const OrtValue* const_value_;  // for input
  TT* data_{};                   // for output
  Span<T> span_;
};

template <>
class Tensor<std::string> : public TensorBase {
 public:
  using strings = std::vector<std::string>;

  Tensor(const OrtW::CustomOpApi& ort_api,
         OrtKernelContext* ctx,
         size_t indice,
         bool is_input) : TensorBase(ort_api,
                                     ctx,
                                     indice,
                                     is_input) {
    if (is_input) {
      auto input_count = ort_api.KernelContext_GetInputCount(ctx_);
      if (indice >= input_count) {
        ORTX_CXX_API_THROW("invalid indice", ORT_RUNTIME_EXCEPTION);
      }

      auto* const_value = ort_api.KernelContext_GetInput(ctx_, indice);
      auto* info = ort_api.GetTensorTypeAndShape(const_value);
      shape_ = ort_api.GetTensorShape(info);
      ort_api.ReleaseTensorTypeAndShapeInfo(info);

      size_t num_chars;
      OrtW::ThrowOnError(ort_api.GetOrtApi(), ort_api.GetOrtApi().GetStringTensorDataLength(const_value, &num_chars));
      std::vector<char> chars(num_chars + 1, '\0');
      auto num_strings = NumberOfElement();
      std::vector<size_t> offsets(NumberOfElement());
      OrtW::ThrowOnError(ort_api.GetOrtApi(), ort_api.GetOrtApi().GetStringTensorContent(const_value,
                                                                                         (void*)chars.data(),
                                                                                         num_chars,
                                                                                         offsets.data(),
                                                                                         offsets.size()));
      auto upper_bound = static_cast<int64_t>(num_strings) - 1;
      input_strings_.resize(num_strings);
      for (int64_t i = upper_bound; i >= 0; --i) {
        if (i < upper_bound) {
          chars[offsets[i + 1]] = '\0';
        }
        input_strings_[i] = chars.data() + offsets[i];
      }
    }
  }
  const strings& Data() const {
    return input_strings_;
  }
  void SetStringOutput(const strings& ss, const std::vector<int64_t>& dims) {
    std::vector<const char*> raw;
    for (const auto& s : ss) {
      raw.push_back(s.data());
    }

    auto* output = ort_api_.KernelContext_GetOutput(ctx_, indice_, dims.data(), dims.size());
    OrtW::ThrowOnError(ort_api_.GetOrtApi(), ort_api_.GetOrtApi().FillStringTensor(output, raw.data(), raw.size()));
  }
  void SetStringOutput(const std::vector<const char*>& ss, const std::vector<int64_t>& dims) {
    auto* output = ort_api_.KernelContext_GetOutput(ctx_, indice_, dims.data(), dims.size());
    OrtW::ThrowOnError(ort_api_.GetOrtApi(), ort_api_.GetOrtApi().FillStringTensor(output, ss.data(), ss.size()));
  }
  const std::string& AsScalar() {
    if (!shape_.has_value() || shape_->size() != 1 || (*shape_)[0] != 1) {
      ORTX_CXX_API_THROW("to get a scalar, shape must be {1}", ORT_RUNTIME_EXCEPTION);
    }
    return input_strings_[0];
  }

 private:
  std::vector<std::string> input_strings_;  // for input
};

using TensorPtr = std::unique_ptr<Custom::TensorBase>;

template <typename... Args>
struct OrtCustomOpT2Base : public OrtCustomOp {
  using CreateFn = void* (*)(const OrtCustomOp*, const OrtApi*, const OrtKernelInfo*);
  using KernelFn = void (*)(void*, OrtKernelContext*);
  using DestroyFn = void (*)(void*);
  using MyType = OrtCustomOpT2Base<Args...>;

  OrtCustomOpT2Base(const char* op_name,
                    const char* execution_provider,
                    CreateFn create_fn,
                    KernelFn compute_fn,
                    DestroyFn destroy_fn) : op_name_(op_name),
                                            execution_provider_(execution_provider) {
    ParseArgs<Args...>();

    OrtCustomOp::KernelCompute = compute_fn;

    OrtCustomOp::version = ORT_API_VERSION;

    OrtCustomOp::CreateKernel = create_fn;

    OrtCustomOp::GetName = [](const OrtCustomOp* this_) { return static_cast<const MyType*>(this_)->op_name_.c_str(); };
    OrtCustomOp::GetExecutionProviderType = [](const OrtCustomOp* this_) { return ((MyType*)this_)->execution_provider_; };

    OrtCustomOp::GetInputTypeCount = [](const OrtCustomOp* this_) {
      auto self = reinterpret_cast<const MyType*>(this_);
      return self->input_types_.size();
    };

    OrtCustomOp::GetInputType = [](const OrtCustomOp* this_, size_t indice) {
      auto self = reinterpret_cast<const MyType*>(this_);
      return self->input_types_[indice];
    };

    OrtCustomOp::GetOutputTypeCount = [](const OrtCustomOp* this_) {
      auto self = reinterpret_cast<const MyType*>(this_);
      return self->output_types_.size();
    };

    OrtCustomOp::GetOutputType = [](const OrtCustomOp* this_, size_t indice) {
      auto self = reinterpret_cast<const MyType*>(this_);
      return self->output_types_[indice];
    };

    OrtCustomOp::KernelDestroy = destroy_fn;

    OrtCustomOp::GetInputCharacteristic = [](const OrtCustomOp*, size_t) { return INPUT_OUTPUT_REQUIRED; };
    OrtCustomOp::GetOutputCharacteristic = [](const OrtCustomOp*, size_t) { return INPUT_OUTPUT_REQUIRED; };
  }

  /////////////////////////////  create input tuple ///////////////////////////////

  std::tuple<Args...> CreateInputTupleInvoker(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    return CreateInputTuple<0, 0, Args...>(ort_api, context);
  }

  template <size_t ith_input, size_t ith_output, typename... Ts>
  typename std::enable_if<sizeof...(Ts) == 0, std::tuple<>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    return std::make_tuple();
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, OrtKernelContext*>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    std::tuple<T> current = std::tuple<OrtKernelContext*>{context};
    auto next = CreateInputTuple<ith_input, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  // tensor inputs
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<float>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<float>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<int32_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int32_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<int64_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int64_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<uint8_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<uint8_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<double>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<double>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<std::string>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<std::string>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Tensor<bool>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<bool>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  // span inputs
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Span<float>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<float>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<float>*>(tensors_.back().get())->AsSpan()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Span<int32_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int32_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<int32_t>*>(tensors_.back().get())->AsSpan()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Span<int64_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int64_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<int64_t>*>(tensors_.back().get())->AsSpan()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Span<uint8_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<uint8_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<uint8_t>*>(tensors_.back().get())->AsSpan()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Span<double>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<double>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<double>*>(tensors_.back().get())->AsSpan()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const Custom::Span<bool>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<bool>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<bool>*>(tensors_.back().get())->AsSpan()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  // scalar inputs
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, float>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<float>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<float>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, int32_t>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int32_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<int32_t>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, int64_t>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int64_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<int64_t>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, uint8_t>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<uint8_t>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<uint8_t>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, double>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<double>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<double>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, bool>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<bool>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<bool>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, const std::string&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<std::string>>(ort_api, context, ith_input, true));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<std::string>*>(tensors_.back().get())->AsScalar()};
    auto next = CreateInputTuple<ith_input + 1, ith_output, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  // tensor outputs
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<float>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<float>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<int32_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int32_t>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<int64_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<int64_t>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<uint8_t>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<uint8_t>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<double>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<double>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<std::string>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<std::string>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  typename std::enable_if<std::is_same<T, Custom::Tensor<bool>&>::value, std::tuple<T, Ts...>>::type
  CreateInputTuple(const OrtW::CustomOpApi& ort_api, OrtKernelContext* context) {
    tensors_.push_back(std::make_unique<Custom::Tensor<bool>>(ort_api, context, ith_output, false));
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors_.back())};
    auto next = CreateInputTuple<ith_input, ith_output + 1, Ts...>(ort_api, context);
    return std::tuple_cat(current, next);
  }

  /////////////////////////////  parse args ///////////////////////////////

  template <typename... Ts>
  typename std::enable_if<0 == sizeof...(Ts)>::type
  ParseArgs() {
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, OrtKernelContext*>::value>::type
  ParseArgs() {
    ParseArgs<Ts...>();
  }

  // tensor inputs
  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<float>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<int32_t>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<int64_t>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<uint8_t>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<double>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<std::string>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Tensor<bool>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
    ParseArgs<Ts...>();
  }

  // span inputs
  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Span<float>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Span<int32_t>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Span<int64_t>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Span<uint8_t>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Span<double>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const Custom::Span<bool>&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
    ParseArgs<Ts...>();
  }

  // scalar inputs
  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, float>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, int32_t>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, int64_t>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, uint8_t>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, double>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, const std::string&>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, bool>::value>::type
  ParseArgs() {
    input_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
    ParseArgs<Ts...>();
  }

  // outputs
  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<float>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<int32_t>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<int64_t>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<uint8_t>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<double>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<std::string>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING);
    ParseArgs<Ts...>();
  }

  template <typename T, typename... Ts>
  typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<bool>&>::value>::type
  ParseArgs() {
    output_types_.push_back(ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
    ParseArgs<Ts...>();
  }

  ////////////////////////////// members //////////////////////////////

  const std::string op_name_;
  const char* execution_provider_;

  std::vector<TensorPtr> tensors_;
  std::vector<ONNXTensorElementDataType> input_types_;
  std::vector<ONNXTensorElementDataType> output_types_;

  const OrtApi* ort_api_{nullptr};
};  // class OrtCustomOpLite

template <typename... Args>
struct OrtCustomOpT2 : public OrtCustomOpT2Base<Args...> {
  using ComputeFn = void (*)(Args...);
  using MyType = OrtCustomOpT2<Args...>;

  struct ComputeState {
    const OrtApi* ort_api_;
    ComputeFn compute_fn_;
    const OrtCustomOp* this_;
  };

  OrtCustomOpT2(const char* op_name,
                const char* execution_provider,
                ComputeFn compute_fn) : OrtCustomOpT2Base<Args...>(
                                            op_name,
                                            execution_provider,
                                            [](const OrtCustomOp* this_, const OrtApi* ort, const OrtKernelInfo* info) {
                                              auto self = const_cast<MyType*>(reinterpret_cast<const MyType*>(this_));
                                              return static_cast<void*>(new ComputeState{ort, self->compute_fn_, self});
                                            },
                                            [](void* op_kernel, OrtKernelContext* context) {
                                              auto state = reinterpret_cast<ComputeState*>(op_kernel);
                                              if (!state->ort_api_) {
                                                ORTX_CXX_API_THROW("ort api is not set.", ORT_FAIL);
                                              }
                                              OrtW::CustomOpApi ort_api(*state->ort_api_);
                                              auto self = const_cast<MyType*>(reinterpret_cast<const MyType*>(state->this_));
                                              auto t = self->CreateInputTupleInvoker(ort_api, context);
                                              std::apply([state](Args const&... t_args) { state->compute_fn_(t_args...); }, t);
                                            },
                                            [](void* op_kernel) { delete reinterpret_cast<ComputeState*>(op_kernel); }),
                                        compute_fn_(compute_fn) {
  }
  //////// members ////
  ComputeFn compute_fn_;
};

template <typename T, typename... Args>
struct OrtCustomOpT2Struct : public OrtCustomOpT2Base<Args...> {
  using StructComputeFn = void (T::*)(Args...);
  using MyType = OrtCustomOpT2Struct<T, Args...>;

  struct KernelState {
    ~KernelState() {
      if (kernel_)
        delete kernel_;
    }

    T* kernel_;
    const OrtApi* ort_api_;
    StructComputeFn struct_compute_fn_;
    const OrtCustomOp* this_;
  };

  OrtCustomOpT2Struct(const char* op_name,
                      const char* execution_provider,
                      StructComputeFn compute_fn) : OrtCustomOpT2Base<Args...>(
                                                        op_name,
                                                        execution_provider,
                                                        [](const OrtCustomOp* this_, const OrtApi* ort, const OrtKernelInfo* info) {
                                                          auto self = const_cast<MyType*>(reinterpret_cast<const MyType*>(this_));
                                                          return static_cast<void*>(new KernelState{
                                                              new T(*ort, *info),
                                                              ort,
                                                              self->struct_compute_fn_,
                                                              this_});
                                                        },
                                                        [](void* op_kernel, OrtKernelContext* context) {
                                                          auto* state = reinterpret_cast<KernelState*>(op_kernel);
                                                          if (!state->ort_api_) {
                                                            ORTX_CXX_API_THROW("ort api is not set.", ORT_FAIL);
                                                          }
                                                          OrtW::CustomOpApi ort_api(*state->ort_api_);
                                                          auto self = const_cast<MyType*>(reinterpret_cast<const MyType*>(state->this_));
                                                          auto t = self->CreateInputTupleInvoker(ort_api, context);
                                                          std::apply([state](Args const&... t_args) { (state->kernel_->*(state->struct_compute_fn_))(t_args...); }, t);
                                                        },
                                                        [](void* op_kernel) {
                                                          auto state = reinterpret_cast<KernelState*>(op_kernel);
                                                          delete state;
                                                        }),
                                                    struct_compute_fn_(compute_fn) {
  }
  ///// members ///
  StructComputeFn struct_compute_fn_;
};

template <typename... Args>
OrtCustomOp* CreateCustomOpT2(const char* op_name,
                              const char* execution_provider,
                              void (*custom_compute_fn)(Args...)) {
  using OrtCustomOpTPtr = OrtCustomOpT2<Args...>;
  return std::make_unique<OrtCustomOpTPtr>(op_name, execution_provider, custom_compute_fn).release();
}

template <typename... Args>
OrtCustomOp* CreateCustomOpT2(const char* op_name,
                              void (*custom_compute_fn)(Args...)) {
  using OrtCustomOpTPtr = OrtCustomOpT2<Args...>;
  return std::make_unique<OrtCustomOpTPtr>(op_name, nullptr, custom_compute_fn).release();
}

template <typename T, typename... Args>
OrtCustomOp* CreateCustomOpT2(const char* op_name,
                              void (T::*custom_compute_fn)(Args...)) {
  using OrtCustomOpTStructPtr = OrtCustomOpT2Struct<T, Args...>;
  return std::make_unique<OrtCustomOpTStructPtr>(op_name, nullptr, custom_compute_fn).release();
}

}  // namespace Custom
}  // namespace Ort