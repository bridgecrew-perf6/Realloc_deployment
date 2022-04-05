// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <sstream>

#include "arrow/compute/api_scalar.h"
#include "arrow/compute/kernels/common.h"

namespace arrow {
namespace compute {
namespace internal {

// ----------------------------------------------------------------------
// String transformation base classes

constexpr int64_t kStringTransformError = -1;

struct StringTransformBase {
  virtual ~StringTransformBase() = default;
  virtual Status PreExec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    return Status::OK();
  }

  // Return the maximum total size of the output in codeunits (i.e. bytes)
  // given input characteristics.
  virtual int64_t MaxCodeunits(int64_t ninputs, int64_t input_ncodeunits) {
    return input_ncodeunits;
  }

  virtual Status InvalidInputSequence() {
    return Status::Invalid("Invalid UTF8 sequence in input");
  }
};

/// Kernel exec generator for unary string transforms. Types of template
/// parameter StringTransform need to define a transform method with the
/// following signature:
///
/// int64_t Transform(const uint8_t* input, int64_t input_string_ncodeunits,
///                   uint8_t* output);
///
/// where
///   * `input` is the input sequence (binary or string)
///   * `input_string_ncodeunits` is the length of input sequence in codeunits
///   * `output` is the output sequence (binary or string)
///
/// and returns the number of codeunits of the `output` sequence or a negative
/// value if an invalid input sequence is detected.
template <typename Type, typename StringTransform>
struct StringTransformExecBase {
  using offset_type = typename Type::offset_type;
  using ArrayType = typename TypeTraits<Type>::ArrayType;

  static Status Execute(KernelContext* ctx, StringTransform* transform,
                        const ExecBatch& batch, Datum* out) {
    if (batch[0].kind() == Datum::ARRAY) {
      return ExecArray(ctx, transform, batch[0].array(), out);
    }
    DCHECK_EQ(batch[0].kind(), Datum::SCALAR);
    return ExecScalar(ctx, transform, batch[0].scalar(), out);
  }

  static Status ExecArray(KernelContext* ctx, StringTransform* transform,
                          const std::shared_ptr<ArrayData>& data, Datum* out) {
    ArrayType input(data);
    const int64_t input_ncodeunits = input.total_values_length();
    const int64_t input_nstrings = input.length();
    const int64_t max_output_ncodeunits =
        transform->MaxCodeunits(input_nstrings, input_ncodeunits);
    RETURN_NOT_OK(CheckOutputCapacity(max_output_ncodeunits));

    ArrayData* output = out->mutable_array();
    ARROW_ASSIGN_OR_RAISE(auto values_buffer, ctx->Allocate(max_output_ncodeunits));
    output->buffers[2] = values_buffer;

    // String offsets are preallocated
    offset_type* output_string_offsets = output->GetMutableValues<offset_type>(1);
    uint8_t* output_str = output->buffers[2]->mutable_data();
    offset_type output_ncodeunits = 0;
    output_string_offsets[0] = output_ncodeunits;
    for (int64_t i = 0; i < input_nstrings; i++) {
      if (!input.IsNull(i)) {
        offset_type input_string_ncodeunits;
        const uint8_t* input_string = input.GetValue(i, &input_string_ncodeunits);
        auto encoded_nbytes = static_cast<offset_type>(transform->Transform(
            input_string, input_string_ncodeunits, output_str + output_ncodeunits));
        if (encoded_nbytes < 0) {
          return transform->InvalidInputSequence();
        }
        output_ncodeunits += encoded_nbytes;
      }
      output_string_offsets[i + 1] = output_ncodeunits;
    }
    DCHECK_LE(output_ncodeunits, max_output_ncodeunits);

    // Trim the codepoint buffer, since we may have allocated too much
    return values_buffer->Resize(output_ncodeunits, /*shrink_to_fit=*/true);
  }

  static Status ExecScalar(KernelContext* ctx, StringTransform* transform,
                           const std::shared_ptr<Scalar>& scalar, Datum* out) {
    const auto& input = checked_cast<const BaseBinaryScalar&>(*scalar);
    if (!input.is_valid) {
      return Status::OK();
    }
    const int64_t data_nbytes = static_cast<int64_t>(input.value->size());
    const int64_t max_output_ncodeunits = transform->MaxCodeunits(1, data_nbytes);
    RETURN_NOT_OK(CheckOutputCapacity(max_output_ncodeunits));

    ARROW_ASSIGN_OR_RAISE(auto value_buffer, ctx->Allocate(max_output_ncodeunits));
    auto* result = checked_cast<BaseBinaryScalar*>(out->scalar().get());
    result->is_valid = true;
    result->value = value_buffer;
    auto encoded_nbytes = static_cast<offset_type>(transform->Transform(
        input.value->data(), data_nbytes, value_buffer->mutable_data()));
    if (encoded_nbytes < 0) {
      return transform->InvalidInputSequence();
    }
    DCHECK_LE(encoded_nbytes, max_output_ncodeunits);
    return value_buffer->Resize(encoded_nbytes, /*shrink_to_fit=*/true);
  }

  static Status CheckOutputCapacity(int64_t ncodeunits) {
    if (ncodeunits > std::numeric_limits<offset_type>::max()) {
      return Status::CapacityError(
          "Result might not fit in a 32bit utf8 array, convert to large_utf8");
    }
    return Status::OK();
  }
};

template <typename Type, typename StringTransform>
struct StringTransformExec : public StringTransformExecBase<Type, StringTransform> {
  using StringTransformExecBase<Type, StringTransform>::Execute;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    StringTransform transform;
    RETURN_NOT_OK(transform.PreExec(ctx, batch, out));
    return Execute(ctx, &transform, batch, out);
  }
};

template <typename Type, typename StringTransform>
struct StringTransformExecWithState
    : public StringTransformExecBase<Type, StringTransform> {
  using State = typename StringTransform::State;
  using StringTransformExecBase<Type, StringTransform>::Execute;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    StringTransform transform(State::Get(ctx));
    RETURN_NOT_OK(transform.PreExec(ctx, batch, out));
    return Execute(ctx, &transform, batch, out);
  }
};

template <template <typename> class ExecFunctor>
void MakeUnaryStringBatchKernel(
    std::string name, FunctionRegistry* registry, const FunctionDoc* doc,
    MemAllocation::type mem_allocation = MemAllocation::PREALLOCATE) {
  auto func = std::make_shared<ScalarFunction>(name, Arity::Unary(), doc);
  for (const auto& ty : StringTypes()) {
    auto exec = GenerateVarBinaryToVarBinary<ExecFunctor>(ty);
    ScalarKernel kernel{{ty}, ty, std::move(exec)};
    kernel.mem_allocation = mem_allocation;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  DCHECK_OK(registry->AddFunction(std::move(func)));
}

template <template <typename> class ExecFunctor>
void MakeUnaryStringBatchKernelWithState(
    std::string name, FunctionRegistry* registry, const FunctionDoc* doc,
    MemAllocation::type mem_allocation = MemAllocation::PREALLOCATE) {
  auto func = std::make_shared<ScalarFunction>(name, Arity::Unary(), doc);
  {
    using t32 = ExecFunctor<StringType>;
    ScalarKernel kernel{{utf8()}, utf8(), t32::Exec, t32::State::Init};
    kernel.mem_allocation = mem_allocation;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  {
    using t64 = ExecFunctor<LargeStringType>;
    ScalarKernel kernel{{large_utf8()}, large_utf8(), t64::Exec, t64::State::Init};
    kernel.mem_allocation = mem_allocation;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  DCHECK_OK(registry->AddFunction(std::move(func)));
}

// ----------------------------------------------------------------------
// Predicates and classification

// Defined in scalar_string_utf8.cc.
void EnsureUtf8LookupTablesFilled();

static FunctionDoc StringPredicateDoc(std::string summary, std::string description) {
  return FunctionDoc{std::move(summary), std::move(description), {"strings"}};
}

static FunctionDoc StringClassifyDoc(std::string class_summary, std::string class_desc,
                                     bool non_empty) {
  std::string summary, description;
  {
    std::stringstream ss;
    ss << "Classify strings as " << class_summary;
    summary = ss.str();
  }
  {
    std::stringstream ss;
    if (non_empty) {
      ss
          << ("For each string in `strings`, emit true iff the string is non-empty\n"
              "and consists only of ");
    } else {
      ss
          << ("For each string in `strings`, emit true iff the string consists only\n"
              "of ");
    }
    ss << class_desc << ".  Null strings emit null.";
    description = ss.str();
  }
  return StringPredicateDoc(std::move(summary), std::move(description));
}

template <typename Type, typename Predicate>
struct StringPredicateFunctor {
  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    Status st = Status::OK();
    EnsureUtf8LookupTablesFilled();
    if (batch[0].kind() == Datum::ARRAY) {
      const ArrayData& input = *batch[0].array();
      ArrayIterator<Type> input_it(input);
      ArrayData* out_arr = out->mutable_array();
      ::arrow::internal::GenerateBitsUnrolled(
          out_arr->buffers[1]->mutable_data(), out_arr->offset, input.length,
          [&]() -> bool {
            util::string_view val = input_it();
            return Predicate::Call(ctx, reinterpret_cast<const uint8_t*>(val.data()),
                                   val.size(), &st);
          });
    } else {
      const auto& input = checked_cast<const BaseBinaryScalar&>(*batch[0].scalar());
      if (input.is_valid) {
        bool boolean_result = Predicate::Call(
            ctx, input.value->data(), static_cast<size_t>(input.value->size()), &st);
        // UTF decoding can lead to issues
        if (st.ok()) {
          out->value = std::make_shared<BooleanScalar>(boolean_result);
        }
      }
    }
    return st;
  }
};

template <typename Predicate>
void AddUnaryStringPredicate(std::string name, FunctionRegistry* registry,
                             const FunctionDoc* doc) {
  auto func = std::make_shared<ScalarFunction>(name, Arity::Unary(), doc);
  for (const auto& ty : StringTypes()) {
    auto exec = GenerateVarBinaryToVarBinary<StringPredicateFunctor, Predicate>(ty);
    DCHECK_OK(func->AddKernel({ty}, boolean(), std::move(exec)));
  }
  DCHECK_OK(registry->AddFunction(std::move(func)));
}

// ----------------------------------------------------------------------
// Slicing

struct StringSliceTransformBase : public StringTransformBase {
  using State = OptionsWrapper<SliceOptions>;

  const SliceOptions* options;

  Status PreExec(KernelContext* ctx, const ExecBatch& batch, Datum* out) override {
    options = &State::Get(ctx);
    if (options->step == 0) {
      return Status::Invalid("Slice step cannot be zero");
    }
    return Status::OK();
  }
};

struct ReplaceStringSliceTransformBase : public StringTransformBase {
  using State = OptionsWrapper<ReplaceSliceOptions>;

  const ReplaceSliceOptions* options;

  explicit ReplaceStringSliceTransformBase(const ReplaceSliceOptions& options)
      : options{&options} {}

  int64_t MaxCodeunits(int64_t ninputs, int64_t input_ncodeunits) override {
    return ninputs * options->replacement.size() + input_ncodeunits;
  }
};

// ----------------------------------------------------------------------
// Splitting

template <typename Options>
struct StringSplitFinderBase {
  virtual ~StringSplitFinderBase() = default;
  virtual Status PreExec(const Options& options) { return Status::OK(); }

  // Derived classes should also define these methods:
  //   static bool Find(const uint8_t* begin, const uint8_t* end,
  //                    const uint8_t** separator_begin,
  //                    const uint8_t** separator_end,
  //                    const SplitPatternOptions& options);
  //
  //   static bool FindReverse(const uint8_t* begin, const uint8_t* end,
  //                           const uint8_t** separator_begin,
  //                           const uint8_t** separator_end,
  //                           const SplitPatternOptions& options);
};

template <typename Type, typename ListType, typename SplitFinder,
          typename Options = typename SplitFinder::Options>
struct StringSplitExec {
  using string_offset_type = typename Type::offset_type;
  using list_offset_type = typename ListType::offset_type;
  using ArrayType = typename TypeTraits<Type>::ArrayType;
  using ArrayListType = typename TypeTraits<ListType>::ArrayType;
  using ListScalarType = typename TypeTraits<ListType>::ScalarType;
  using ScalarType = typename TypeTraits<Type>::ScalarType;
  using BuilderType = typename TypeTraits<Type>::BuilderType;
  using ListOffsetsBuilderType = TypedBufferBuilder<list_offset_type>;
  using State = OptionsWrapper<Options>;

  // Keep the temporary storage accross individual values, to minimize reallocations
  std::vector<util::string_view> parts;
  Options options;

  explicit StringSplitExec(const Options& options) : options(options) {}

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    return StringSplitExec{State::Get(ctx)}.Execute(ctx, batch, out);
  }

  Status Execute(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    SplitFinder finder;
    RETURN_NOT_OK(finder.PreExec(options));
    if (batch[0].kind() == Datum::ARRAY) {
      return Execute(ctx, &finder, batch[0].array(), out);
    }
    DCHECK_EQ(batch[0].kind(), Datum::SCALAR);
    return Execute(ctx, &finder, batch[0].scalar(), out);
  }

  Status Execute(KernelContext* ctx, SplitFinder* finder,
                 const std::shared_ptr<ArrayData>& data, Datum* out) {
    const ArrayType input(data);

    BuilderType builder(input.type(), ctx->memory_pool());
    // A slight overestimate of the data needed
    RETURN_NOT_OK(builder.ReserveData(input.total_values_length()));
    // The minimum amount of strings needed
    RETURN_NOT_OK(builder.Resize(input.length() - input.null_count()));

    ArrayData* output_list = out->mutable_array();
    // List offsets were preallocated
    auto* list_offsets = output_list->GetMutableValues<list_offset_type>(1);
    DCHECK_NE(list_offsets, nullptr);
    // Initial value
    *list_offsets++ = 0;
    for (int64_t i = 0; i < input.length(); ++i) {
      if (!input.IsNull(i)) {
        RETURN_NOT_OK(SplitString(input.GetView(i), finder, &builder));
        if (ARROW_PREDICT_FALSE(builder.length() >
                                std::numeric_limits<list_offset_type>::max())) {
          return Status::CapacityError("List offset does not fit into 32 bit");
        }
      }
      *list_offsets++ = static_cast<list_offset_type>(builder.length());
    }
    // Assign string array to list child data
    std::shared_ptr<Array> string_array;
    RETURN_NOT_OK(builder.Finish(&string_array));
    output_list->child_data.push_back(string_array->data());
    return Status::OK();
  }

  Status Execute(KernelContext* ctx, SplitFinder* finder,
                 const std::shared_ptr<Scalar>& scalar, Datum* out) {
    const auto& input = checked_cast<const ScalarType&>(*scalar);
    auto result = checked_cast<ListScalarType*>(out->scalar().get());
    if (input.is_valid) {
      result->is_valid = true;
      BuilderType builder(input.type, ctx->memory_pool());
      util::string_view s(*input.value);
      RETURN_NOT_OK(SplitString(s, finder, &builder));
      RETURN_NOT_OK(builder.Finish(&result->value));
    }
    return Status::OK();
  }

  Status SplitString(const util::string_view& s, SplitFinder* finder,
                     BuilderType* builder) {
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(s.data());
    const uint8_t* end = begin + s.length();

    int64_t max_splits = options.max_splits;
    // if there is no max splits, reversing does not make sense (and is probably less
    // efficient), but is useful for testing
    if (options.reverse) {
      // note that i points 1 further than the 'current'
      const uint8_t* i = end;
      // we will record the parts in reverse order
      parts.clear();
      if (max_splits > -1) {
        parts.reserve(max_splits + 1);
      }
      while (max_splits != 0) {
        const uint8_t *separator_begin, *separator_end;
        // find with whatever algo the part we will 'cut out'
        if (finder->FindReverse(begin, i, &separator_begin, &separator_end, options)) {
          parts.emplace_back(reinterpret_cast<const char*>(separator_end),
                             i - separator_end);
          i = separator_begin;
          max_splits--;
        } else {
          // if we cannot find a separator, we're done
          break;
        }
      }
      parts.emplace_back(reinterpret_cast<const char*>(begin), i - begin);
      // now we do the copying
      for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        RETURN_NOT_OK(builder->Append(*it));
      }
    } else {
      const uint8_t* i = begin;
      while (max_splits != 0) {
        const uint8_t *separator_begin, *separator_end;
        // find with whatever algo the part we will 'cut out'
        if (finder->Find(i, end, &separator_begin, &separator_end, options)) {
          // the part till the beginning of the 'cut'
          RETURN_NOT_OK(
              builder->Append(i, static_cast<string_offset_type>(separator_begin - i)));
          i = separator_end;
          max_splits--;
        } else {
          // if we cannot find a separator, we're done
          break;
        }
      }
      // trailing part
      RETURN_NOT_OK(builder->Append(i, static_cast<string_offset_type>(end - i)));
    }
    return Status::OK();
  }
};

using StringSplitState = OptionsWrapper<SplitOptions>;

}  // namespace internal
}  // namespace compute
}  // namespace arrow
