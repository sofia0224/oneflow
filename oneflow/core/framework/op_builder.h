/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_FRAMEWORK_OP_BUILDER_H_
#define ONEFLOW_CORE_FRAMEWORK_OP_BUILDER_H_

#include <string>
#include <mutex>

#include "oneflow/core/framework/operation.h"

namespace oneflow {
namespace one {

class TensorNameScope {
 public:
  static TensorNameScope* Global();

  const std::string& Lookup(const TensorRef& tensor) const;

  void Record(const TensorRef& tensor, const std::string& name);

 private:
  TensorNameScope() = default;
  virtual ~TensorNameScope() = default;

 private:
  mutable std::mutex mutex_;

  std::string default_tensor_name_ = "";
  // uint64_t(Tensor*) -> the name of the tensor.
  std::unordered_map<uint64_t, std::string> tensor_names_;
};

class OpBuilder {
 public:
  OpBuilder() : operation_(new Operation) {}
  explicit OpBuilder(const std::string& op_type_name);
  virtual ~OpBuilder() = default;

  OpBuilder& Name(const std::string& op_name) {
    operation_->op_name = op_name;
    return *this;
  }

  OpBuilder& Op(const std::string& op_type_name);

  OpBuilder& Input(const std::string& input_name, const std::vector<TensorRef>& input);

  OpBuilder& Output(const std::string& output_name);
  OpBuilder& Output(const std::string& output_name, const int count);

  OpBuilder& Attr(const std::string& attr_name, const AttrValue& attr_value);

  // template <typename T>
  // OpBuilder& Attr(const std::string& attr_name, const T& attr_value);

  std::shared_ptr<Operation>&& Build();

 private:
  std::shared_ptr<Operation> operation_;
};

}  // namespace one
}  // namespace oneflow

#endif  // ONEFLOW_CORE_FRAMEWORK_OP_BUILDER_H_
