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
#include "oneflow/core/framework/op_interpreter.h"
#include "oneflow/core/framework/op_builder.h"
#include "oneflow/core/framework/instructions_builder.h"
#include "oneflow/core/framework/op_arg_util.h"
#include "oneflow/core/framework/scope_util.h"
#include "oneflow/core/framework/session_util.h"
#include "oneflow/core/framework/symbol_storage_util.h"
#include "oneflow/core/operator/operator.h"
#include "oneflow/api/python/job_build/job_build_and_infer.h"
#include "oneflow/core/eager/foreign_boxing_util.h"

namespace oneflow {
namespace one {

static std::shared_ptr<cfg::OpAttribute> AddOpAndInferOpAttribute(
    const BuiltinOpExpr* op_expr, const OpExprInterpContext* ctx,
    const std::unordered_map<std::string, std::string>& ibn2tensor_names = {}) {
  OperatorConf op_conf;
  op_expr->BuildOpConf(&op_conf);
  for (const auto& it : ibn2tensor_names) {
    ReplaceInputLbnInOpCustomizedConf(&op_conf, it.first, it.second);
  }
  int64_t symbol_id = ctx->scope->symbol_id().GetOrThrow();
  op_conf.set_scope_symbol_id(symbol_id);
  if (!op_conf.has_device_tag()) {
    op_conf.set_device_tag(ctx->scope->device_parallel_desc_symbol()->device_tag());
  }
  OpAttribute op_attribute = [&]() {
    auto infer_ctx = GetCurInferCtx().GetOrThrow();
    if (ctx->is_mirrored_strategy_enabled) {
      return infer_ctx->AddAndInferMirroredOp(op_conf).GetOrThrow();
    } else {
      return infer_ctx->AddAndInferConsistentOp(op_conf).GetOrThrow();
    }
  }();
  return std::make_shared<cfg::OpAttribute>(op_attribute);
}

void OpExprInterpreter::ResetSelfState() { self_state_.reset(new OpExprInterpState); }

void LazyInterpreter::Apply(const OpExpr* op_expr, const TensorList& inputs, TensorList& outputs,
                            const OpExprInterpState* state) {
  ResetSelfState();

#define APPLY_IF(op_type)                                             \
  if (const auto* op = dynamic_cast<const op_type##Expr*>(op_expr)) { \
    return Apply_(op, inputs, outputs, state);                        \
  }

  APPLY_IF(FunctionOp);
  APPLY_IF(BuiltinOp);
#undef APPLY_IF

  LOG(FATAL) << "The type " << op_expr->type()
             << " has not been supported in LazyInterpreter::Apply.";
}

void LazyInterpreter::Apply_(const BuiltinOpExpr* op_expr, const TensorList& inputs,
                             TensorList& outputs, const OpExprInterpState* state) {
  CHECK_EQ(inputs.size(), op_expr->input_num());
  std::unordered_map<std::string, std::string> ibn2tensor_names;
  for (int i = 0; i < inputs.size(); ++i) {
    const std::string& ibn = op_expr->indexed_ibns().at(i);
    ibn2tensor_names[ibn] = TensorNameScope::Global()->Lookup(inputs[i]);
  }
  auto op_attribute = AddOpAndInferOpAttribute(op_expr, context(), ibn2tensor_names);
  OpAttribute proto_op_attribute;
  op_attribute->ToProto(&proto_op_attribute);

  // Check outputs num and setup output tensor properties.
  CHECK_EQ(outputs.size(), op_expr->output_num());
  for (int i = 0; i < op_expr->output_num(); ++i) {
    const auto& bn_in_op2blob_desc =
        proto_op_attribute.logical_blob_desc_signature().bn_in_op2blob_desc();
    const std::string& obn = op_expr->indexed_obns().at(i);
    BlobDesc blob_desc(bn_in_op2blob_desc.at(obn));
    // outputs[i]->set_blob_desc(blob_desc);
    TensorNameScope::Global()->Record(outputs[i], op_expr->op_name() + "/" + obn);
  }
}

void LazyInterpreter::Apply_(const FunctionOpExpr* op_expr, const TensorList& inputs,
                             TensorList& outputs, const OpExprInterpState* state) {
  // TODO
}

void EagerInterpreter::Apply(const OpExpr* op_expr, const TensorList& inputs, TensorList& outputs,
                             const OpExprInterpState* state) {
  ResetSelfState();

#define APPLY_IF(op_type)                                             \
  if (const auto* op = dynamic_cast<const op_type##Expr*>(op_expr)) { \
    return Apply_(op, inputs, outputs, state);                        \
  }

  APPLY_IF(UserOp);
  APPLY_IF(VariableOp);
  APPLY_IF(CastToMirroredOp);
  APPLY_IF(CastFromMirroredOp);
  APPLY_IF(DistributeSplitOp);
  APPLY_IF(DistributeCloneOp);
  APPLY_IF(DistributeConcatOp);
  APPLY_IF(DistributeAddOp);
  APPLY_IF(FunctionOp);
#undef APPLY_IF

  LOG(FATAL) << "The type " << op_expr->type()
             << " has not been supported in EagerInterpreter::Apply.";
}

static std::shared_ptr<HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>>
MakeBn2BlobObjectMap(const BuiltinOpExpr* op_expr, const TensorList& inputs,
                     const TensorList& outputs) {
  using Bn2BlobObjectMap = HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>;
  auto* bn2blob_object(new Bn2BlobObjectMap{});
  for (int i = 0; i < inputs.size(); ++i) {
    const auto& ibn = op_expr->indexed_ibns().at(i);
    bn2blob_object->emplace(ibn, inputs[i]->blob_object());
  }
  for (int i = 0; i < outputs.size(); ++i) {
    const auto& obn = op_expr->indexed_obns().at(i);
    bn2blob_object->emplace(obn, outputs[i]->blob_object());
  }
  return std::shared_ptr<Bn2BlobObjectMap>(bn2blob_object);
}

static void NaiveInterpret(const BuiltinOpExpr* op_expr, const TensorList& inputs,
                           TensorList& outputs,
                           const std::shared_ptr<cfg::OpAttribute>& op_attribute,
                           const std::shared_ptr<cfg::ParallelConf>& parallel_conf) {
  auto BuildInstruction = [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    auto bn2blob_object = MakeBn2BlobObjectMap(op_expr, inputs, outputs);
    builder->NoBoxingStatelessCall(op_attribute, parallel_conf, bn2blob_object);
  };
  void(LogicalRun(BuildInstruction).GetOrThrow());
}

void EagerInterpreter::Apply_(const UserOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto op_attribute = AddOpAndInferOpAttribute(op_expr, context());
  auto parallel_conf = std::make_shared<cfg::ParallelConf>(
      context()->scope->device_parallel_desc_symbol()->parallel_conf());
  NaiveInterpret(op_expr, inputs, outputs, op_attribute, parallel_conf);
}

static std::string GetJobNameScopePrefix(const Session* session, const std::string& job_name) {
  // TODO
  return std::string("");
}

static OperatorConf&& GenModelInitOpConf(const OperatorConf& variable_conf) {
  OperatorConf model_init_op_conf;
  return std::move(model_init_op_conf);
}

static OperatorConf&& GenModelIOPathInputOpConf() {
  OperatorConf path_input_op_conf;
  return std::move(path_input_op_conf);
}

static OperatorConf&& GenModelLoadOpConf() {
  OperatorConf model_load_op_conf;
  return std::move(model_load_op_conf);
}

static std::shared_ptr<cfg::OpAttribute> InferOpAttribute(
    const std::shared_ptr<Scope>& scope, OperatorConf& op_conf,
    const HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>& ibn2blob_object) {
  op_conf.set_scope_symbol_id(scope->symbol_id().GetOrThrow());
  OpNodeSignature upstream_signature;
  if (ibn2blob_object.size()) {
    std::shared_ptr<cfg::OpNodeSignature> cfg_upstream_signature(new cfg::OpNodeSignature);
    for (const auto& it : ibn2blob_object) {
      it.second->op_arg_parallel_attr()->DumpToOpNodeSignature(it.first, cfg_upstream_signature);
      it.second->op_arg_blob_attr()->DumpToOpNodeSignature(it.first, cfg_upstream_signature);
    }
    cfg_upstream_signature->ToProto(&upstream_signature);
  }
  const auto&& op = ConstructAndInferOp(op_conf, upstream_signature, *scope).GetPtrOrThrow();
  const auto& op_attribute = op->GetOpAttributeWithoutOpNameAndLbn();
  return std::make_shared<cfg::OpAttribute>(*op_attribute);
}

static std::function<void(const std::shared_ptr<InstructionsBuilder>&)>
BuildModelInitOrIOPathInputInstruction(
    OperatorConf& op_conf,
    const std::shared_ptr<HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>>&
        bn2blob_object) {
  using namespace std::placeholders;
  return [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    auto&& scope = GetCurrentScope().GetPtrOrThrow();
    auto op_attribute = InferOpAttribute(scope, op_conf, {});
    auto parallel_conf =
        std::make_shared<cfg::ParallelConf>(scope->device_parallel_desc_symbol()->parallel_conf());
    const auto* boxing_util = Global<ForeignBoxingUtil>::Get();
    builder->StatelessCall(op_attribute, parallel_conf, bn2blob_object,
                           std::bind(&ForeignBoxingUtil::BoxingTo, boxing_util, _1, _2, _3));
  };
}

static std::function<void(const std::shared_ptr<InstructionsBuilder>&)> BuildFeedPathInstruction(
    const std::shared_ptr<HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>>&
        bn2blob_object) {
  // TODO
  int callback_id = -1;
  return [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    const auto& blob_object = bn2blob_object->at("out");
    builder->FeedBlob(blob_object, callback_id);
    builder->InsertRemoveForeignCallbackInstruction(blob_object->object_id(), callback_id);
  };
}

static std::shared_ptr<compatible_py::BlobObject> EagerRunModelInit(const OperatorConf& op_conf) {
  using Bn2BlobObjectMap = HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>;
  auto&& model_init_conf = GenModelInitOpConf(op_conf);
  std::shared_ptr<Bn2BlobObjectMap> bn2blob_object(new Bn2BlobObjectMap{});

  auto BuildModelInitInstruction =
      BuildModelInitOrIOPathInputInstruction(model_init_conf, bn2blob_object);
  LogicalRun(BuildModelInitInstruction).GetOrThrow();
  return bn2blob_object->at("out_0");
}

static std::shared_ptr<compatible_py::BlobObject> EagerRunModelLoad(
    const OperatorConf& op_conf, const std::string& snapshot_path) {
  using namespace std::placeholders;
  using Bn2BlobObjectMap = HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>;
  Path path(snapshot_path);
  CHECK(path.basename() == "out");
  CHECK(path.dirname() == op_conf.name());

  auto&& path_input_op_conf = GenModelIOPathInputOpConf();

  std::shared_ptr<Bn2BlobObjectMap> bn2blob_object(new Bn2BlobObjectMap{});
  auto BuildModelIOPathInputInstruction =
      BuildModelInitOrIOPathInputInstruction(path_input_op_conf, bn2blob_object);
  auto _BuildFeedPathInstruction = BuildFeedPathInstruction(bn2blob_object);

  std::shared_ptr<Bn2BlobObjectMap> model_load_blob_objects(new Bn2BlobObjectMap{});
  auto&& model_load_op_conf = GenModelLoadOpConf();
  auto BuildModelLoadInstruction = [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    auto&& scope = GetCurrentScope().GetPtrOrThrow();
    const auto& blob_object = bn2blob_object->at("out");
    (*model_load_blob_objects)["path"] = blob_object;
    auto op_attribute = InferOpAttribute(scope, model_load_op_conf, *model_load_blob_objects);
    auto parallel_conf =
        std::make_shared<cfg::ParallelConf>(scope->device_parallel_desc_symbol()->parallel_conf());
    const auto* boxing_util = Global<ForeignBoxingUtil>::Get();
    builder->StatelessCall(op_attribute, parallel_conf, model_load_blob_objects,
                           std::bind(&ForeignBoxingUtil::BoxingTo, boxing_util, _1, _2, _3));
  };

  LogicalRun(BuildModelIOPathInputInstruction).GetOrThrow();
  LogicalRun(_BuildFeedPathInstruction).GetOrThrow();
  LogicalRun(BuildModelLoadInstruction).GetOrThrow();
  return model_load_blob_objects->at("out_0");
}

static void Assign(std::shared_ptr<compatible_py::BlobObject>& target_blob_object,
                   const std::shared_ptr<compatible_py::BlobObject>& blob_object) {
  auto BuildAssignInstruction = [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    const auto* boxing_util = Global<ForeignBoxingUtil>::Get();
    auto new_parallel_desc_symbol = boxing_util->TryReplaceDeviceTag(
        builder, target_blob_object->parallel_desc_symbol(), "cpu");
    auto consumer_op_arg_parallel_attr = std::make_shared<compatible_py::OpArgParallelAttribute>(
        new_parallel_desc_symbol, target_blob_object->op_arg_parallel_attr()->sbp_parallel(),
        target_blob_object->op_arg_parallel_attr()->opt_mirrored_parallel());
    auto tmp_blob_object =
        boxing_util->BoxingTo(builder, blob_object, consumer_op_arg_parallel_attr);
    boxing_util->Assign(builder, target_blob_object, tmp_blob_object);
  };
  LogicalRun(BuildAssignInstruction).GetOrThrow();
}

static void InitVariableOutputBlob(const Session* session, const std::shared_ptr<Tensor>& output,
                                   const OpAttribute& op_attribute) {
  const auto& op_conf = op_attribute.op_conf();
  const auto& snapshot_path = session->snapshot_mgr()->get_snapshot_path(op_conf.name());

  std::shared_ptr<compatible_py::BlobObject> temp_blob_object;
  if (snapshot_path.empty()) {
    temp_blob_object = EagerRunModelInit(op_conf);
  } else {
    temp_blob_object = EagerRunModelLoad(op_conf, snapshot_path);
  }
  auto target_blob_object = output->blob_object();
  Assign(target_blob_object, temp_blob_object);
}

void EagerInterpreter::Apply_(const VariableOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  CHECK_EQ(inputs.size(), 0);
  CHECK_EQ(outputs.size(), 1);
  const std::string job_name = JobBuildAndInferCtx_GetCurrentJobName().GetOrThrow();
  auto session = GetDefaultSession().GetPtrOrThrow();
  const std::string variable_name =
      GetJobNameScopePrefix(session.get(), job_name) + op_expr->op_name();

  // TODO(hjchen2)
  auto op_attribute = AddOpAndInferOpAttribute(op_expr, context());
  auto parallel_conf = std::make_shared<cfg::ParallelConf>(
      context()->scope->device_parallel_desc_symbol()->parallel_conf());
  NaiveInterpret(op_expr, inputs, outputs, op_attribute, parallel_conf);
  OpAttribute proto_op_attribute;
  op_attribute->ToProto(&proto_op_attribute);

  const auto& mirrored_sig_map =
      proto_op_attribute.mirrored_signature().bn_in_op2opt_mirrored_parallel();
  if (mirrored_sig_map.at("out").has_mirrored_parallel()) {
    // outputs[0].reset(new EagerMirroredTensor(...));
  } else {
    // outputs[0].reset(new EagerConsistentTensor(...));
  }
  InitVariableOutputBlob(session.get(), outputs[0], proto_op_attribute);
}

static std::function<void(const std::shared_ptr<InstructionsBuilder>& builder)>
BuildMirroredCastInstruction(const BuiltinOpExpr* op_expr, const TensorList& inputs,
                             TensorList& outputs, const OpExprInterpContext* ctx) {
  auto op_attribute = AddOpAndInferOpAttribute(op_expr, ctx);
  auto BuildInstruction = [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    auto bn2blob_object = MakeBn2BlobObjectMap(op_expr, inputs, outputs);
    const auto& in_blob_object = (*bn2blob_object)["in"];
    const auto& parallel_desc_symbol = in_blob_object->parallel_desc_symbol();
    OpAttribute proto_op_attribute;
    op_attribute->ToProto(&proto_op_attribute);
    const auto& op_arg_parallel_attr =
        compatible_py::GetOpArgParallelAttribute(parallel_desc_symbol, proto_op_attribute, "out")
            .GetOrThrow();
    auto out_blob_object = builder->MakeReferenceBlobObject(
        in_blob_object,
        std::make_shared<compatible_py::OpArgParallelAttribute>(op_arg_parallel_attr));
    *((*bn2blob_object)["out"]) = out_blob_object.GetOrThrow();
  };
  return BuildInstruction;
}

void EagerInterpreter::Apply_(const CastToMirroredOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto BuildInstruction = BuildMirroredCastInstruction(op_expr, inputs, outputs, context());
  LogicalRun(BuildInstruction).GetOrThrow();
}

void EagerInterpreter::Apply_(const CastFromMirroredOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto BuildInstruction = BuildMirroredCastInstruction(op_expr, inputs, outputs, context());
  LogicalRun(BuildInstruction).GetOrThrow();
}

static std::shared_ptr<compatible_py::BlobObject> GetInBlobObject(
    const std::shared_ptr<InstructionsBuilder>& builder, const OpAttribute& op_attribute,
    const std::string& ibn,
    const HashMap<std::string, std::shared_ptr<compatible_py::BlobObject>>& bn2blob_object) {
  const auto& parallel_sig = op_attribute.parallel_signature().bn_in_op2parallel_desc_symbol_id();
  int symbol_id = parallel_sig.at(ibn);
  auto in_op_parallel_desc_sym =
      GetSymbol<cfg::ParallelConf, ParallelDesc>(symbol_id).GetPtrOrThrow();
  const auto& in_op_arg_parallel_attr =
      compatible_py::GetOpArgParallelAttribute(in_op_parallel_desc_sym, op_attribute, ibn)
          .GetPtrOrThrow();
  auto origin_blob_object = bn2blob_object.at(ibn);
  return Global<ForeignBoxingUtil>::Get()->BoxingTo(builder, origin_blob_object,
                                                    in_op_arg_parallel_attr);
};

static std::function<void(const std::shared_ptr<InstructionsBuilder>& builder)>
BuildDistributeSplitOrCloneInstruction(const BuiltinOpExpr* op_expr, const TensorList& inputs,
                                       TensorList& outputs, const OpExprInterpContext* ctx) {
  auto op_attribute = AddOpAndInferOpAttribute(op_expr, ctx);
  OpAttribute proto_op_attribute;
  op_attribute->ToProto(&proto_op_attribute);
  auto BuildInstruction = [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    auto bn2blob_object = MakeBn2BlobObjectMap(op_expr, inputs, outputs);
    auto logical_in_blob_object =
        GetInBlobObject(builder, proto_op_attribute, "in", *bn2blob_object);
    auto physical_out_blob_objects =
        builder->UnpackLogicalBlobToPhysicalBlobs(logical_in_blob_object).GetOrThrow();
    for (int i = 0; i < physical_out_blob_objects.size(); ++i) {
      *((*bn2blob_object)["out_" + std::to_string(i)]) = *(physical_out_blob_objects[i]);
    }
  };
  return BuildInstruction;
}

void EagerInterpreter::Apply_(const DistributeSplitOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto BuildInstruction =
      BuildDistributeSplitOrCloneInstruction(op_expr, inputs, outputs, context());
  LogicalRun(BuildInstruction).GetOrThrow();
}

void EagerInterpreter::Apply_(const DistributeCloneOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto BuildInstruction =
      BuildDistributeSplitOrCloneInstruction(op_expr, inputs, outputs, context());
  LogicalRun(BuildInstruction).GetOrThrow();
}

static std::function<void(const std::shared_ptr<InstructionsBuilder>& builder)>
BuildDistributeConcatAndAddInstruction(const BuiltinOpExpr* op_expr, const TensorList& inputs,
                                       TensorList& outputs, const OpExprInterpContext* ctx) {
  auto op_attribute = AddOpAndInferOpAttribute(op_expr, ctx);
  OpAttribute proto_op_attribute;
  op_attribute->ToProto(&proto_op_attribute);
  auto op_parallel_desc_sym =
      GetSymbol<cfg::ParallelConf, ParallelDesc>(
          proto_op_attribute.parallel_signature().op_parallel_desc_symbol_id())
          .GetPtrOrThrow();
  auto op_arg_parallel_attr =
      compatible_py::GetOpArgParallelAttribute(op_parallel_desc_sym, proto_op_attribute, "out")
          .GetPtrOrThrow();
  auto op_arg_blob_attr =
      compatible_py::GetOpArgBlobAttribute(proto_op_attribute, "out").GetPtrOrThrow();
  auto BuildInstruction = [&](const std::shared_ptr<InstructionsBuilder>& builder) {
    int input_size = op_expr->indexed_ibns().size();
    auto bn2blob_object = MakeBn2BlobObjectMap(op_expr, inputs, outputs);
    std::vector<std::shared_ptr<compatible_py::BlobObject>> in_blob_objects(input_size);
    for (int i = 0; i < input_size; ++i) {
      in_blob_objects[i] =
          GetInBlobObject(builder, proto_op_attribute, "in_" + std::to_string(i), *bn2blob_object);
    }
    auto physical_out_blob_object = builder
                                        ->PackPhysicalBlobsToLogicalBlob(
                                            in_blob_objects, op_arg_parallel_attr, op_arg_blob_attr)
                                        .GetPtrOrThrow();
    *((*bn2blob_object)["out"]) = *physical_out_blob_object;
  };
  return BuildInstruction;
}

void EagerInterpreter::Apply_(const DistributeConcatOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto BuildInstruction =
      BuildDistributeConcatAndAddInstruction(op_expr, inputs, outputs, context());
  LogicalRun(BuildInstruction).GetOrThrow();
}

void EagerInterpreter::Apply_(const DistributeAddOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  auto BuildInstruction =
      BuildDistributeConcatAndAddInstruction(op_expr, inputs, outputs, context());
  LogicalRun(BuildInstruction).GetOrThrow();
}

void EagerInterpreter::Apply_(const FunctionOpExpr* op_expr, const TensorList& inputs,
                              TensorList& outputs, const OpExprInterpState* state) {
  // TODO(hjchen2)
}

void AutogradInterpreter::Apply(const OpExpr* op_expr, const TensorList& inputs,
                                TensorList& outputs, const OpExprInterpState* state) {
  // TODO(hjchen2)
}

}  // namespace one
}  // namespace oneflow
