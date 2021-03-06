/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/framework/op_kernel.h"

#include <unordered_map>
#include <vector>

#include "tensorflow/core/framework/attr_value_util.h"
#include "tensorflow/core/framework/log_memory.h"
#include "tensorflow/core/framework/memory_types.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op_def_util.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/notification.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

namespace {

Status MatchSignatureHelper(const DataTypeSlice expected_inputs,
                            const DataTypeSlice expected_outputs,
                            const DataTypeSlice inputs,
                            const DataTypeSlice outputs) {
  bool signature_mismatch = false;

  if (inputs.size() != expected_inputs.size()) signature_mismatch = true;
  for (size_t i = 0; !signature_mismatch && i < inputs.size(); ++i) {
    if (!TypesCompatible(expected_inputs[i], inputs[i])) {
      signature_mismatch = true;
    }
  }

  if (outputs.size() != expected_outputs.size()) signature_mismatch = true;
  for (size_t i = 0; !signature_mismatch && i < outputs.size(); ++i) {
    if (!TypesCompatible(expected_outputs[i], outputs[i])) {
      signature_mismatch = true;
    }
  }

  if (signature_mismatch) {
    return errors::InvalidArgument("Signature mismatch, have: ",
                                   DataTypeSliceString(inputs), "->",
                                   DataTypeSliceString(outputs), " expected: ",
                                   DataTypeSliceString(expected_inputs), "->",
                                   DataTypeSliceString(expected_outputs));
  }
  return Status::OK();
}

}  // namespace

// OpKernel ------------------------------------------------------------------

OpKernel::OpKernel(OpKernelConstruction* context)
    : def_(context->def()),
      input_types_(context->input_types().begin(),
                   context->input_types().end()),
      input_memory_types_(context->input_memory_types().begin(),
                          context->input_memory_types().end()),
      output_types_(context->output_types().begin(),
                    context->output_types().end()),
      output_memory_types_(context->output_memory_types().begin(),
                           context->output_memory_types().end()),
      graph_def_version_(context->graph_def_version()),
      is_internal_(StringPiece(type_string()).starts_with("_")),
      input_name_map_(context->num_inputs()),
      output_name_map_(context->num_outputs()) {
  OP_REQUIRES_OK(context,
                 NameRangesForNode(def_, context->op_def(), &input_name_map_,
                                   &output_name_map_));
}

OpKernel::~OpKernel() {}

Status OpKernel::InputRange(StringPiece input_name, int* start,
                            int* stop) const {
  const auto result = input_name_map_.find(input_name.ToString());
  if (result == input_name_map_.end()) {
    return errors::InvalidArgument("Unknown input name: ", input_name);
  } else {
    *start = result->second.first;
    *stop = result->second.second;
    return Status::OK();
  }
}

Status OpKernel::OutputRange(StringPiece output_name, int* start,
                             int* stop) const {
  const auto result = output_name_map_.find(output_name.ToString());
  if (result == output_name_map_.end()) {
    return errors::InvalidArgument("Unknown output name: ", output_name);
  } else {
    *start = result->second.first;
    *stop = result->second.second;
    return Status::OK();
  }
}

void AsyncOpKernel::Compute(OpKernelContext* context) {
  Notification n;
  ComputeAsync(context, [&n]() { n.Notify(); });
  n.WaitForNotification();
}

// PersistentTensor ----------------------------------------------------------

Tensor* PersistentTensor::AccessTensor(OpKernelConstruction* context) {
  // the caller has to have a valid context
  CHECK(context);
  return &tensor_;
}

Tensor* PersistentTensor::AccessTensor(OpKernelContext* context) {
  context->NotifyUseOfPersistentTensor(tensor_);
  return &tensor_;
}

// OpKernelConstruction ------------------------------------------------------

void OpKernelConstruction::SetStatus(const Status& status) {
  status_->Update(status);
}

Status OpKernelConstruction::MatchSignature(
    const DataTypeSlice expected_inputs, const DataTypeSlice expected_outputs) {
  return MatchSignatureHelper(expected_inputs, expected_outputs, input_types_,
                              output_types_);
}

Status OpKernelConstruction::allocate_temp(DataType type,
                                           const TensorShape& shape,
                                           Tensor* out_temp) {
  AllocationAttributes attr;
  attr.allocation_will_be_logged = true;
  Tensor new_temp(allocator_, type, shape, attr);

  if (!new_temp.IsInitialized() && shape.num_elements() > 0) {
    return errors::ResourceExhausted(
        "OOM when allocating temporary tensor with shape", shape.DebugString());
  }
  if (LogMemory::IsEnabled()) {
    LogMemory::RecordTensorAllocation(
        def_->name(), LogMemory::OP_KERNEL_CONSTRUCTION_STEP_ID, new_temp);
  }
  *out_temp = new_temp;
  return Status::OK();
}

Status OpKernelConstruction::allocate_persistent(
    DataType type, const TensorShape& shape, PersistentTensor* out_persistent,
    Tensor** out_tensor) {
  // for now just do the same thing as allocate_temp
  // TODO(misard) add specific memory tracking for persistent tensors
  Tensor persistent;
  Status s = allocate_temp(type, shape, &persistent);
  if (!s.ok()) {
    return s;
  }
  *out_persistent = PersistentTensor(persistent);
  Tensor* allocated = out_persistent->AccessTensor(this);
  if (out_tensor) {
    *out_tensor = allocated;
  }
  return s;
}

// OpKernelContext -----------------------------------------------------------

OpKernelContext::OpKernelContext(Params* params)
    : OpKernelContext(params, params->op_kernel->output_types().size()) {}
OpKernelContext::OpKernelContext(Params* params, int noutputs)
    : params_(params), outputs_(noutputs) {
  Allocator* eigen_gpu_allocator = get_allocator(AllocatorAttributes());
  params_->ensure_eigen_gpu_device();
  params_->device->ReinitializeGpuDevice(this, params_->eigen_gpu_device,
                                         params_->op_device_context,
                                         eigen_gpu_allocator);
  record_tensor_accesses_ = params_->device->RequiresRecordingAccessedTensors();
}

OpKernelContext::~OpKernelContext() {
  for (TensorValue& value : outputs_) {
    if (!value.is_ref()) {
      delete value.tensor;
    }
  }
}

Allocator* OpKernelContext::get_allocator(AllocatorAttributes attr) {
  Allocator* allocator =
      params_->device->GetStepAllocator(attr, step_resource_manager());
  if (params_->track_allocations) {
    mutex_lock lock(mu_);
    for (const auto& wrapped : wrapped_allocators_) {
      if (wrapped.first == allocator) {
        return wrapped.second;
      }
    }
    TrackingAllocator* wrapped_allocator =
        new TrackingAllocator(allocator, attr.track_sizes());
    wrapped_allocators_.push_back(std::make_pair(allocator, wrapped_allocator));
    return wrapped_allocator;
  } else {
    return allocator;
  }
}

void OpKernelContext::SetStatus(const Status& status) {
  status_.Update(status);
}

void OpKernelContext::really_record_tensor_reference(const Tensor& tensor) {
  mutex_lock l(mu_);
  // Keep a reference to the underlying memory around.
  referenced_tensors_.Add(tensor);
}

Status OpKernelContext::input(StringPiece name, const Tensor** tensor) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->InputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued input name '",
                                   name,
                                   "' when single-valued input was "
                                   "expected");
  }
  if ((*params_->inputs)[start].is_ref()) {
    return errors::InvalidArgument("OpKernel used ref input name '", name,
                                   "' when immutable input was expected");
  }
  *tensor = (*params_->inputs)[start].tensor;
  record_tensor_reference(**tensor);
  return Status::OK();
}

Status OpKernelContext::input_ref_mutex(StringPiece name, mutex** out_mutex) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->InputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued input name '",
                                   name,
                                   "' when single-valued input was expected");
  }
  *out_mutex = input_ref_mutex(start);
  return Status::OK();
}

const Tensor& OpKernelContext::input(int index) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, params_->inputs->size());
  DCHECK(!(*params_->inputs)[index].is_ref());
  const Tensor& tensor = *((*params_->inputs)[index].tensor);
  record_tensor_reference(tensor);
  return tensor;
}

Tensor OpKernelContext::mutable_input(int index, bool lock_held) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, params_->inputs->size());
  DCHECK((*params_->inputs)[index].is_ref());
  // return a copy of the Ref acquired while holding the mutex
  if (lock_held) {
    Tensor& tensor = *((*params_->inputs)[index].tensor);
    record_tensor_reference(tensor);
    return tensor;
  } else {
    mutex_lock l(*input_ref_mutex(index));
    Tensor& tensor = *((*params_->inputs)[index].tensor);
    record_tensor_reference(tensor);
    return tensor;
  }
}

void OpKernelContext::replace_ref_input(int index, const Tensor& tensor,
                                        bool lock_held) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, params_->inputs->size());
  DCHECK((*params_->inputs)[index].is_ref());
  // should only modify the tensor while holding the mutex
  if (lock_held) {
    *(*params_->inputs)[index].tensor = tensor;
  } else {
    mutex_lock l(*input_ref_mutex(index));
    *(*params_->inputs)[index].tensor = tensor;
  }
  record_tensor_reference(tensor);
}

void OpKernelContext::forward_ref_input_to_ref_output(int input_index,
                                                      int output_index) {
  DCHECK_GE(input_index, 0);
  DCHECK_LT(input_index, params_->inputs->size());
  DCHECK((*params_->inputs)[input_index].is_ref());
  set_output_ref(output_index, (*params_->inputs)[input_index].mutex_if_ref,
                 (*params_->inputs)[input_index].tensor);
}

void OpKernelContext::delete_ref_input(int index, bool lock_held) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, params_->inputs->size());
  DCHECK((*params_->inputs)[index].is_ref());
  // should only modify the tensor while holding the mutex
  if (lock_held) {
    delete (*params_->inputs)[index].tensor;
  } else {
    mutex_lock l(*input_ref_mutex(index));
    delete (*params_->inputs)[index].tensor;
  }
}

Status OpKernelContext::mutable_input(StringPiece name, Tensor* tensor,
                                      bool lock_held) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->InputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued input name '",
                                   name,
                                   "' when single-valued input was expected");
  }
  if (!(*params_->inputs)[start].is_ref()) {
    return errors::InvalidArgument("OpKernel used immutable input name '", name,
                                   "' when ref input was expected");
  }
  // return a copy of the Ref acquired while holding the mutex
  if (lock_held) {
    *tensor = *(*params_->inputs)[start].tensor;
  } else {
    mutex_lock l(*input_ref_mutex(start));
    *tensor = *(*params_->inputs)[start].tensor;
  }
  record_tensor_reference(*tensor);
  return Status::OK();
}

Status OpKernelContext::replace_ref_input(StringPiece name,
                                          const Tensor& tensor,
                                          bool lock_held) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->InputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued input name '",
                                   name,
                                   "' when single-valued input was expected");
  }
  if (!(*params_->inputs)[start].is_ref()) {
    return errors::InvalidArgument("OpKernel used immutable input name '", name,
                                   "' when ref input was expected");
  }
  replace_ref_input(start, tensor, lock_held);
  return Status::OK();
}

Status OpKernelContext::input_list(StringPiece name, OpInputList* list) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->InputRange(name, &start, &stop));
  *list = OpInputList(this, start, stop);
  return Status::OK();
}

Status OpKernelContext::mutable_input_list(StringPiece name,
                                           OpMutableInputList* list) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->InputRange(name, &start, &stop));
  *list = OpMutableInputList(this, start, stop);
  return Status::OK();
}

Status OpKernelContext::output_list(StringPiece name, OpOutputList* list) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  *list = OpOutputList(this, start, stop);
  return Status::OK();
}

Status OpKernelContext::allocate_output(int index, const TensorShape& shape,
                                        Tensor** output) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, num_outputs());
  AllocatorAttributes attr = output_alloc_attr(index);
  return allocate_output(index, shape, output, attr);
}

Status OpKernelContext::allocate_output(StringPiece name,
                                        const TensorShape& shape,
                                        Tensor** tensor) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued output name '",
                                   name,
                                   "' when single-valued output was "
                                   "expected");
  }
  return allocate_output(start, shape, tensor);
}

Status OpKernelContext::allocate_output(StringPiece name,
                                        const TensorShape& shape,
                                        Tensor** tensor,
                                        AllocatorAttributes attr) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued output name '",
                                   name,
                                   "' when single-valued output was "
                                   "expected");
  }
  return allocate_output(start, shape, tensor, attr);
}

Status OpKernelContext::allocate_tensor(
    DataType type, const TensorShape& shape, Tensor* out_tensor,
    AllocatorAttributes attr, const AllocationAttributes& allocation_attr) {
  Allocator* a = get_allocator(attr);
  AllocationAttributes logged_attr(allocation_attr);
  logged_attr.allocation_will_be_logged = true;
  Tensor new_tensor(a, type, shape, logged_attr);

  if (!new_tensor.IsInitialized() && shape.num_elements() > 0) {
    return errors::ResourceExhausted("OOM when allocating tensor with shape",
                                     shape.DebugString());
  }
  if (LogMemory::IsEnabled()) {
    LogMemory::RecordTensorAllocation(params_->op_kernel->name(),
                                      params_->step_id, new_tensor);
  }
  *out_tensor = new_tensor;
  record_tensor_reference(new_tensor);
  return Status::OK();
}

Status OpKernelContext::allocate_output(int index, const TensorShape& shape,
                                        Tensor** output,
                                        AllocatorAttributes attr) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, outputs_.size());
  const DataType type = params_->op_kernel->output_type(index);
  DCHECK(!IsRefType(type));
  DCHECK(mutable_output(index) == nullptr);
  Tensor* output_tensor = new Tensor();
  Status s = allocate_tensor(type, shape, output_tensor, attr);
  if (s.ok()) {
    outputs_[index] = TensorValue(output_tensor);
    *output = outputs_[index].tensor;
  }
  return s;
}

Status OpKernelContext::allocate_temp(
    DataType type, const TensorShape& shape, Tensor* out_temp,
    AllocatorAttributes allocator_attr,
    const AllocationAttributes& allocation_attr) {
  Status s =
      allocate_tensor(type, shape, out_temp, allocator_attr, allocation_attr);
  return s;
}

Status OpKernelContext::allocate_persistent(DataType type,
                                            const TensorShape& shape,
                                            PersistentTensor* out_persistent,
                                            Tensor** out_tensor,
                                            AllocatorAttributes attr) {
  // TODO(misard) add specific memory tracking for persistent tensors
  Tensor persistent;
  Status s = allocate_tensor(type, shape, &persistent, attr);
  if (s.ok()) {
    *out_persistent = PersistentTensor(persistent);
    if (out_tensor) {
      *out_tensor = out_persistent->AccessTensor(this);
    }
  }
  return s;
}

Status OpKernelContext::set_output(StringPiece name, const Tensor& tensor) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued output name '",
                                   name,
                                   "' when single-valued output was "
                                   "expected");
  }
  set_output(start, tensor);
  return Status::OK();
}

void OpKernelContext::set_output(int index, const Tensor& tensor) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, outputs_.size());
  DCHECK(!IsRefType(params_->op_kernel->output_type(index)));
  DCHECK_EQ(mutable_output(index), nullptr);
  record_tensor_reference(tensor);
  outputs_[index] = TensorValue(new Tensor(tensor));
}

void OpKernelContext::set_output_ref(int index, mutex* mu,
                                     Tensor* tensor_for_ref) {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, outputs_.size());
  DCHECK(IsRefType(params_->op_kernel->output_type(index)));
  record_tensor_reference(*tensor_for_ref);
  outputs_[index] = TensorValue(mu, tensor_for_ref);
}

Status OpKernelContext::set_output_ref(StringPiece name, mutex* mu,
                                       Tensor* tensor_for_ref) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued output name '",
                                   name,
                                   "' when single-valued output was "
                                   "expected");
  }
  set_output_ref(start, mu, tensor_for_ref);
  return Status::OK();
}

Status OpKernelContext::mutable_output(StringPiece name, Tensor** tensor) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued output name '",
                                   name,
                                   "' when single-valued output was "
                                   "expected");
  }
  *tensor = mutable_output(start);
  return Status::OK();
}

Status OpKernelContext::release_output(StringPiece name, TensorValue* value) {
  int start, stop;
  TF_RETURN_IF_ERROR(params_->op_kernel->OutputRange(name, &start, &stop));
  if (stop != start + 1) {
    return errors::InvalidArgument("OpKernel used list-valued output name '",
                                   name,
                                   "' when single-valued output was "
                                   "expected");
  }
  *value = release_output(start);
  return Status::OK();
}

bool OpKernelContext::ValidateInputsAreSameShape(OpKernel* op) {
  const auto& inputs = *params_->inputs;
  for (size_t i = 1; i < inputs.size(); ++i) {
    if (!inputs[0]->IsSameSize(*(inputs[i].tensor))) {
      SetStatus(errors::InvalidArgument(
          "Inputs to operation ", op->name(), " of type ", op->type_string(),
          " must have the same size and shape.  Input 0: ",
          inputs[0]->shape().DebugString(), " != input ", i, ": ",
          inputs[i]->shape().DebugString()));
      return false;
    }
  }
  return true;
}

Status OpKernelContext::MatchSignature(const DataTypeSlice expected_inputs,
                                       const DataTypeSlice expected_outputs) {
  DataTypeVector inputs;
  for (const TensorValue& t : *params_->inputs) {
    inputs.push_back(t.is_ref() ? MakeRefType(t->dtype()) : t->dtype());
  }
  DataTypeVector outputs = params_->op_kernel->output_types();
  return MatchSignatureHelper(expected_inputs, expected_outputs, inputs,
                              outputs);
}

// OpKernel registration ------------------------------------------------------

struct KernelRegistration {
  KernelRegistration(const KernelDef& d, StringPiece c,
                     kernel_factory::OpKernelRegistrar::Factory f)
      : def(d), kernel_class_name(c.ToString()), factory(f) {}
  const KernelDef def;
  const string kernel_class_name;
  const kernel_factory::OpKernelRegistrar::Factory factory;
};

// This maps from 'op_type' + DeviceType to the set of KernelDefs and
// factory functions for instantiating the OpKernel that matches the
// KernelDef.
typedef std::unordered_multimap<string, KernelRegistration> KernelRegistry;

void* GlobalKernelRegistry() {
  static KernelRegistry* global_kernel_registry = new KernelRegistry;
  return global_kernel_registry;
}

static KernelRegistry* GlobalKernelRegistryTyped() {
  return reinterpret_cast<KernelRegistry*>(GlobalKernelRegistry());
}

static string Key(StringPiece op_type, DeviceType device_type,
                  StringPiece label) {
  return strings::StrCat(op_type, ":", DeviceTypeString(device_type), ":",
                         label);
}

namespace kernel_factory {

void OpKernelRegistrar::InitInternal(const KernelDef* kernel_def,
                                     StringPiece kernel_class_name,
                                     Factory factory) {
  const string key =
      Key(kernel_def->op(), DeviceType(kernel_def->device_type()),
          kernel_def->label());
  GlobalKernelRegistryTyped()->insert(std::make_pair(
      key, KernelRegistration(*kernel_def, kernel_class_name, factory)));
  delete kernel_def;
}

}  // namespace kernel_factory

namespace {

// Helper for AttrsMatch().
bool InTypeList(DataType dt, const AttrValue& type_list) {
  for (int in_list : type_list.list().type()) {
    if (dt == in_list) return true;
  }
  return false;
}

// Returns whether the attrs in the NodeDef satisfy the constraints in
// the kernel_def.  Returns an error if attrs in kernel_def are not
// found, or have a mismatching type.
Status AttrsMatch(const NodeDef& node_def, const KernelDef& kernel_def,
                  bool* match) {
  *match = false;
  AttrSlice attrs(node_def);
  for (const auto& constraint : kernel_def.constraint()) {
    if (constraint.allowed_values().list().type_size() == 0) {
      return errors::Unimplemented(
          "KernelDef '", kernel_def.ShortDebugString(),
          " has constraint on attr '", constraint.name(),
          "' with unsupported type: ",
          SummarizeAttrValue(constraint.allowed_values()));
    }

    const AttrValue* found = attrs.Find(constraint.name());
    if (found) {
      if (found->type() != DT_INVALID) {
        if (!InTypeList(found->type(), constraint.allowed_values())) {
          return Status::OK();
        }
      } else {
        if (!AttrValueHasType(*found, "list(type)").ok()) {
          return errors::InvalidArgument(
              "KernelDef '", kernel_def.ShortDebugString(),
              "' has constraint on attr '", constraint.name(),
              "' that has value '", SummarizeAttrValue(*found),
              "' that does not have type 'type' or 'list(type)' in NodeDef "
              "'",
              SummarizeNodeDef(node_def), "'");
        }

        for (int t : found->list().type()) {
          if (!InTypeList(static_cast<DataType>(t),
                          constraint.allowed_values())) {
            return Status::OK();
          }
        }
      }
    } else {
      return errors::InvalidArgument(
          "OpKernel '", kernel_def.op(), "' has constraint on attr '",
          constraint.name(), "' not in NodeDef '", SummarizeNodeDef(node_def),
          "', KernelDef: '", kernel_def.ShortDebugString(), "'");
    }
  }
  *match = true;
  return Status::OK();
}

Status FindKernelRegistration(DeviceType device_type, const NodeDef& node_def,
                              const KernelRegistration** reg) {
  *reg = nullptr;
  string label;  // Label defaults to empty if not found in NodeDef.
  GetNodeAttr(node_def, "_kernel", &label);
  const string key = Key(node_def.op(), device_type, label);
  auto regs = GlobalKernelRegistryTyped()->equal_range(key);
  for (auto iter = regs.first; iter != regs.second; ++iter) {
    // If there is a kernel registered for the op and device_type,
    // check that the attrs match.
    bool match;
    TF_RETURN_IF_ERROR(AttrsMatch(node_def, iter->second.def, &match));
    if (match) {
      if (*reg != nullptr) {
        return errors::InvalidArgument(
            "Multiple OpKernel registrations match NodeDef '",
            SummarizeNodeDef(node_def), "': '", (*reg)->def.ShortDebugString(),
            "' and '", iter->second.def.ShortDebugString(), "'");
      }
      *reg = &iter->second;
    }
  }
  return Status::OK();
}

}  // namespace

Status FindKernelDef(DeviceType device_type, const NodeDef& node_def,
                     const KernelDef** def, string* kernel_class_name) {
  const KernelRegistration* reg = nullptr;
  TF_RETURN_IF_ERROR(FindKernelRegistration(device_type, node_def, &reg));
  if (reg == nullptr) {
    return errors::NotFound("No registered '", node_def.op(), "' OpKernel for ",
                            DeviceTypeString(device_type),
                            " devices compatible with node ",
                            SummarizeNodeDef(node_def));
  }
  if (def != nullptr) *def = &reg->def;
  if (kernel_class_name != nullptr) *kernel_class_name = reg->kernel_class_name;
  return Status::OK();
}

Status SupportedDeviceTypesForNode(
    const std::vector<DeviceType>& prioritized_types, const NodeDef& def,
    DeviceTypeVector* device_types) {
  // TODO(zhifengc): Changes the callers (SimplePlacer and
  // DynamicPlacer) to consider the possibility that 'def' is call to
  // a user-defined function and only calls this
  // SupportedDeviceTypesForNode for primitive ops.
  Status s;
  const OpDef* op_def = OpRegistry::Global()->LookUp(def.op(), &s);
  if (op_def) {
    for (const DeviceType& device_type : prioritized_types) {
      const KernelRegistration* reg = nullptr;
      TF_RETURN_IF_ERROR(FindKernelRegistration(device_type, def, &reg));
      if (reg != nullptr) device_types->push_back(device_type);
    }
  } else {
    // Assumes that all device types support this node.
    for (const DeviceType& device_type : prioritized_types) {
      device_types->push_back(device_type);
    }
  }
  return Status::OK();
}

std::unique_ptr<OpKernel> CreateOpKernel(
    DeviceType device_type, DeviceBase* device, Allocator* allocator,
    const NodeDef& node_def, int graph_def_version, Status* status) {
  OpKernel* kernel = nullptr;
  *status = CreateOpKernel(device_type, device, allocator, nullptr, node_def,
                           graph_def_version, &kernel);
  return std::unique_ptr<OpKernel>(kernel);
}

Status CreateOpKernel(DeviceType device_type, DeviceBase* device,
                      Allocator* allocator, FunctionLibraryRuntime* flib,
                      const NodeDef& node_def, int graph_def_version,
                      OpKernel** kernel) {
  VLOG(1) << "Instantiating kernel for node: " << SummarizeNodeDef(node_def);

  // Look up the Op registered for this op name.
  Status s;
  const OpDef* op_def = OpRegistry::Global()->LookUp(node_def.op(), &s);
  if (op_def == nullptr) return s;

  // Validate node_def against OpDef.
  s = ValidateNodeDef(node_def, *op_def);
  if (!s.ok()) return s;

  // Look up kernel registration.
  const KernelRegistration* registration;
  s = FindKernelRegistration(device_type, node_def, &registration);
  if (!s.ok()) {
    errors::AppendToMessage(&s, " when instantiating ", node_def.op());
    return s;
  }
  if (registration == nullptr) {
    s.Update(errors::NotFound("No registered '", node_def.op(),
                              "' OpKernel for ", DeviceTypeString(device_type),
                              " devices compatible with node ",
                              SummarizeNodeDef(node_def)));
    return s;
  }

  // Get signature from the OpDef & NodeDef
  DataTypeVector inputs;
  DataTypeVector outputs;
  s.Update(InOutTypesForNode(node_def, *op_def, &inputs, &outputs));
  if (!s.ok()) {
    errors::AppendToMessage(&s, " for node: ", SummarizeNodeDef(node_def));
    return s;
  }

  // We are creating a kernel for an op registered in
  // OpRegistry::Global(), we consult the kernel registry to decide
  // the kernel's input and output memory types.
  MemoryTypeVector input_memory_types;
  MemoryTypeVector output_memory_types;
  TF_RETURN_IF_ERROR(MemoryTypesForNode(OpRegistry::Global(), device_type,
                                        node_def, &input_memory_types,
                                        &output_memory_types));

  // Everything needed for OpKernel construction.
  OpKernelConstruction context(
      device_type, device, allocator, &node_def, op_def, flib, inputs,
      input_memory_types, outputs, output_memory_types, graph_def_version, &s);
  *kernel = (*registration->factory)(&context);
  if (!s.ok()) {
    delete *kernel;
    *kernel = nullptr;
  }
  return s;
}

namespace {

bool FindArgInOp(StringPiece arg_name,
                 const protobuf::RepeatedPtrField<OpDef::ArgDef>& args) {
  for (const auto& arg : args) {
    if (arg_name == arg.name()) {
      return true;
    }
  }
  return false;
}

}  // namespace

Status ValidateKernelRegistrations(const OpRegistryInterface& op_registry) {
  Status unused_status;
  for (const auto& key_registration : *GlobalKernelRegistryTyped()) {
    const KernelDef& kernel_def(key_registration.second.def);
    const OpDef* op_def = op_registry.LookUp(kernel_def.op(), &unused_status);
    if (op_def == nullptr) {
      // TODO(josh11b): Make this a hard error.
      LOG(ERROR) << "OpKernel ('" << kernel_def.ShortDebugString()
                 << "') for unknown op: " << kernel_def.op();
      continue;
    }
    for (const auto& host_memory_arg : kernel_def.host_memory_arg()) {
      if (!FindArgInOp(host_memory_arg, op_def->input_arg()) &&
          !FindArgInOp(host_memory_arg, op_def->output_arg())) {
        return errors::InvalidArgument("HostMemory arg '", host_memory_arg,
                                       "' not found in OpDef: ",
                                       SummarizeOpDef(*op_def));
      }
    }
  }
  return Status::OK();
}

template <>
const Eigen::ThreadPoolDevice& OpKernelContext::eigen_device() const {
  return eigen_cpu_device();
}

template <>
const Eigen::GpuDevice& OpKernelContext::eigen_device() const {
  return eigen_gpu_device();
}

void OpKernelConstruction::CtxFailure(Status s) {
  VLOG(1) << s;
  SetStatus(s);
}

void OpKernelConstruction::CtxFailureWithWarning(Status s) {
  LOG(WARNING) << s;
  SetStatus(s);
}

void OpKernelContext::CtxFailure(Status s) {
  VLOG(1) << s;
  SetStatus(s);
}

void OpKernelContext::CtxFailureWithWarning(Status s) {
  LOG(WARNING) << s;
  SetStatus(s);
}

}  // namespace tensorflow
