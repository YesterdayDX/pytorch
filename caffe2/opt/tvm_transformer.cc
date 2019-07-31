#include "caffe2/opt/tvm_transformer.h"
#include "caffe2/opt/backend_cutting.h"

C10_DEFINE_bool(
    caffe2_tvm_profiling_based_jit,
    true,
    "Use profiling based jit for TVM transform");

C10_DEFINE_int32(
    caffe2_tvm_min_ops,
    1,
    "Minimal number of supported ops for the subgraph to be lowered to TVM");

namespace caffe2 {

NetDef TvmTransformer::buildTvmOp(
    const caffe2::NetDef& net,
    const std::unordered_set<std::string>& weights,
    const ShapeInfoMap& shape_hints) {
  if (opts_.min_ops > net.op_size()) {
    return net;
  }
  caffe2::NetDef net_opt;
  auto* op = net_opt.add_op();
  op->set_type("TVMJit");

  // Remove the second output of Concat/Reshape from external_output.
  // And figure out what primary inputs of the net is sequence look-ups
  std::unordered_set<std::string> split_infos;
  std::unordered_set<std::string> input_set(
      net.external_input().begin(), net.external_input().end());
  std::unordered_set<std::string> seq_input_set;
  for (const auto& op0 : net.op()) {
    if ((op0.type() == "Concat" || op0.type() == "Reshape") &&
        op0.output_size() == 2) {
      split_infos.emplace(op0.output(1));
    } else if (
        op0.type() == "SparseLengthsSum" ||
        op0.type() == "SparseLengthsSumFused8BitRowwise") {
      // The indices input of SparseLengthSum should be of SEQ type
      if (op0.input_size() > 1 && input_set.count(op0.input(1))) {
        seq_input_set.emplace(op0.input(1));
      }
    } else if (
        op0.type() == "SparseLengthsWeightedSum" ||
        op0.type() == "SparseLengthsWeightedSumFused8BitRowwise") {
      // The weight and indices inputs of SparseLengthWeightedSum should be of
      // SEQ type
      if (op0.input_size() > 1 && input_set.count(op0.input(1))) {
        seq_input_set.emplace(op0.input(1));
      }
      if (op0.input_size() > 2 && input_set.count(op0.input(2))) {
        seq_input_set.emplace(op0.input(2));
      }
    }
  }

  // C2 operator bind input/output by position (they can be rewritten by e.g.
  // Memonger) while TVM runtime bind them by name. Therefore, we need to record
  // the input/output names.
  auto* input_arg = op->add_arg();
  input_arg->set_name("inputs");
  auto* output_arg = op->add_arg();
  output_arg->set_name("outputs");

  // We expose both inputs and weights as inputs of TVMJitOp
  for (const auto& i : net.external_input()) {
    net_opt.add_external_input(i);
    op->add_input(i);
    input_arg->add_strings(i);
  }
  for (const auto& i : net.external_output()) {
    if (split_infos.count(i)) {
      continue;
    }
    net_opt.add_external_output(i);
    op->add_output(i);
    output_arg->add_strings(i);
  }

  // Record the referred weights
  auto* w_arg = op->add_arg();
  std::unordered_set<std::string> referred_weights;
  for (const auto& op0 : net.op()) {
    for (const auto& i : op0.input()) {
      if (weights.count(i)) {
        referred_weights.emplace(i);
      }
    }
  }
  w_arg->set_name("weights");
  for (const auto& w : referred_weights) {
    w_arg->add_strings(w);
  }

  // Add input shape info in "input_shape_info" argument of the net
  if (!opts_.profiling_based_jit) {
    auto* shape_arg = op->add_arg();
    shape_arg->set_name("input_shape_info");
    for (const auto& i : net_opt.external_input()) {
      shape_arg->mutable_tensors()->Add()->CopyFrom(
          wrapShapeInfoIntoTensorProto(i, shape_hints.at(i)));
    }
  }

  // Add original net as a fallback
  auto* original_net_arg = op->add_arg();
  original_net_arg->set_name("original_net");
  original_net_arg->mutable_n()->CopyFrom(net);

  // Add model id
  AddArgument("model_id", model_id_, op);

  // Add op id
  AddArgument("tvm_op_id", tvm_op_id_++, op);

  // Add nominal batch size
  AddArgument("nominal_batch_size", opts_.bound_shape_spec.max_batch_size, op);

  // Add nominal sequence size
  AddArgument("nominal_seq_size", opts_.bound_shape_spec.max_seq_size, op);

  // Indices of the input blobs with sequence type
  auto* seq_input_indices_arg = op->add_arg();
  seq_input_indices_arg->set_name("seq_input_indices");
  int64_t input_idx = 0;
  for (const auto& input : net_opt.external_input()) {
    if (seq_input_set.count(input)) {
      seq_input_indices_arg->add_ints(input_idx);
    }
    ++input_idx;
  }

  if (opts_.debug) {
    AddArgument("debug", 1, op);
  }

  if (opts_.profiling_based_jit) {
    AddArgument("profiling_based_jit", 1, op);
  }

  return net_opt;
}

// Cutting off the runnable part and replace with TVMJitOPs. Asssume the nets
// were topologically sorted
void TvmTransformer::transform(
    Workspace* ws,
    NetDef* pred_net,
    const std::vector<std::string>& weight_names,
    const std::unordered_map<std::string, TensorShape>& input_shape_hints,
    const std::unordered_set<int>& blacklisted_ops) {
  CAFFE_ENFORCE(ws);
  CAFFE_ENFORCE(pred_net, "Predict net cannot be nullptr");

  // Save the args of the net so that we can copy it to opt net later
  std::vector<Argument> args;
  for (const auto& arg : pred_net->arg()) {
    args.emplace_back(arg);
  }

  // Get model id and reset TVM op id to 0
  model_id_ = getModelId(*pred_net);
  tvm_op_id_ = 0;

  std::unordered_set<std::string> weights(
      weight_names.begin(), weight_names.end());

  // SSA Rewrite the net
  auto shape_hints_mapped =
      ssaRewriteAndMapNames(ws, pred_net, input_shape_hints);

  // Populate shape info
  Workspace mapped_ws(ws, input_mapping_);
  ShapeInfoMap shape_hints;
  if (!opts_.profiling_based_jit) {
    shape_hints = inferShapes(
        &mapped_ws, pred_net, shape_hints_mapped, opts_.bound_shape_spec);
  }

  if (opts_.debug) {
    dumpNet(*pred_net, shape_hints, "debug_ssa_net.pb_txt");
  }

  // We are ready to transform the net
  NetDef net_opt =
      applyTvmTransform(pred_net, weights, blacklisted_ops, shape_hints);

  // Copy the properties
  for (const auto& arg : args) {
    net_opt.add_arg()->CopyFrom(arg);
  }
  net_opt.mutable_device_option()->CopyFrom(pred_net->device_option());
  pred_net->Swap(&net_opt);
  if (opts_.debug) {
    dumpNet(*pred_net, shape_hints, "debug_full_opt_net.pb_txt");
  }
}

NetDef TvmTransformer::applyTvmTransform(
    NetDef* pred_net,
    const std::unordered_set<std::string>& weights,
    const std::unordered_set<int>& blacklisted_ops,
    const ShapeInfoMap& shape_hints) {
  auto profiling_based_jit = opts_.profiling_based_jit;
  auto tvm_supports = [&blacklisted_ops, &shape_hints, &profiling_based_jit](
                          const caffe2::OperatorDef& op) {
    const static std::unordered_set<std::string> supported_ops{
        "Add",        "Sum",
        "FC",         "FCTransposed",
        "Flatten",    "Relu",
        "Sigmoid",    "Softmax",
        "Split",      "EnsureCPUOutput",
        "Reshape",    "ExpandDims",
        "Concat",     "BatchMatMul",
        "MatMul",     "BatchGather",
        "DotProduct", "Transpose",
        "Mul",        "Tanh",
        "Logit",      "Cast"};

    try {
      // If the op position is black listed, return false
      int pos =
          ArgumentHelper::GetSingleArgument<OperatorDef, int>(op, kNetPos, -1);
      if (blacklisted_ops.count(pos)) {
        LOG(INFO) << "Blacklisting op" << op.type() << " at position " << pos;
        return false;
      }

      // If we don't have proper shape info for the op, we cannot compile it
      // properly, return false
      if (!profiling_based_jit) {
        for (const auto& i : op.input()) {
          if (shape_hints.find(i) == shape_hints.end()) {
            LOG(INFO) << "Skipping op " << op.type()
                      << " due to missing shape info for input " << i;
            return false;
          }
        }
      }

      // If TVM c2 frontend doesn't support this op, return false
      // TODO: This should be something like TVMC2Frontend::supports(op);
      return (supported_ops.count(op.type()) != 0);
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Caught exception when querying op " << op.type()
                 << ", what: " << ex.what();
      return false;
    }
  };
  auto tvm_op_converter =
      [this, &weights, &shape_hints](const caffe2::NetDef& net) {
        return buildTvmOp(net, weights, shape_hints);
      };

  return opt::OptimizeForBackend(*pred_net, tvm_supports, tvm_op_converter);
}

void tvmTransform(
    NetDef* net,
    Workspace* ws,
    const std::vector<std::string>& input_names,
    const std::vector<std::string>& output_names,
    const std::vector<std::string>& weight_names,
    const std::unordered_map<std::string, TensorShape>& shape_hints,
    const std::unordered_set<int>& blacklisted_ops,
    size_t max_batch_size,
    size_t max_seq_size,
    bool debug) {
  TvmTransformOptions opts;
  opts.bound_shape_spec.max_batch_size = max_batch_size;
  opts.bound_shape_spec.max_seq_size = max_seq_size;
  opts.debug = debug;
  opts.profiling_based_jit = FLAGS_caffe2_tvm_profiling_based_jit;
  opts.min_ops = FLAGS_caffe2_tvm_min_ops;
  TvmTransformer ts(opts);

  // Clean up the external input/output of the net
  net->mutable_external_input()->Clear();
  net->mutable_external_output()->Clear();
  for (const auto& i : input_names) {
    net->add_external_input(i);
  }
  for (const auto& w : weight_names) {
    net->add_external_input(w);
  }
  for (const auto& o : output_names) {
    net->add_external_output(o);
  }

  ts.transform(ws, net, weight_names, shape_hints, blacklisted_ops);
}
} // namespace caffe2
