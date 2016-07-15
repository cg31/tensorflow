/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/public/tensor_c_api.h"

#include <memory>
#include <vector>

#include "tensorflow/core/framework/log_memory.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/lib/core/coding.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/public/session.h"

// The implementation below is at the top level instead of the
// brain namespace because we are defining 'extern "C"' functions.
using tensorflow::error::Code;
using tensorflow::errors::InvalidArgument;
using tensorflow::gtl::ArraySlice;
using tensorflow::AllocationDescription;
using tensorflow::DataType;
using tensorflow::Env;
using tensorflow::Graph;
using tensorflow::GraphDef;
using tensorflow::mutex;
using tensorflow::mutex_lock;
using tensorflow::NameRangeMap;
using tensorflow::NameRangesForNode;
using tensorflow::NewSession;
using tensorflow::Node;
using tensorflow::NodeDef;
using tensorflow::NodeBuilder;
using tensorflow::OpDef;
using tensorflow::OpRegistry;
using tensorflow::PartialTensorShape;
using tensorflow::Reset;
using tensorflow::RunMetadata;
using tensorflow::RunOptions;
using tensorflow::Session;
using tensorflow::SessionOptions;
using tensorflow::Status;
using tensorflow::Tensor;
using tensorflow::TensorBuffer;
using tensorflow::TensorShape;
using tensorflow::TensorShapeProto;

extern "C" {

// --------------------------------------------------------------------------
struct TF_Status {
  Status status;
};

TF_Status* TF_NewStatus() { return new TF_Status; }

void TF_DeleteStatus(TF_Status* s) { delete s; }

void TF_SetStatus(TF_Status* s, TF_Code code, const char* msg) {
  s->status = Status(static_cast<Code>(code), tensorflow::StringPiece(msg));
}

TF_Code TF_GetCode(const TF_Status* s) {
  return static_cast<TF_Code>(s->status.code());
}

const char* TF_Message(const TF_Status* s) {
  return s->status.error_message().c_str();
}

// --------------------------------------------------------------------------

namespace {
class TF_ManagedBuffer : public TensorBuffer {
 public:
  void* data_;
  size_t len_;
  void (*deallocator_)(void* data, size_t len, void* arg);
  void* deallocator_arg_;

  ~TF_ManagedBuffer() override {
    (*deallocator_)(data_, len_, deallocator_arg_);
  }

  void* data() const override { return data_; }
  size_t size() const override { return len_; }
  TensorBuffer* root_buffer() override { return this; }
  void FillAllocationDescription(AllocationDescription* proto) const override {
    tensorflow::int64 rb = size();
    proto->set_requested_bytes(rb);
    proto->set_allocator_name(tensorflow::cpu_allocator()->Name());
  }
};

void deallocate_realigned_buffer(void* data, size_t len, void* arg) {
  if (tensorflow::LogMemory::IsEnabled()) {
    tensorflow::LogMemory::RecordRawDeallocation(
        "TensorFlow C Api",
        tensorflow::LogMemory::EXTERNAL_TENSOR_ALLOCATION_STEP_ID, data,
        tensorflow::cpu_allocator(), false);
  }
  tensorflow::cpu_allocator()->DeallocateRaw(data);
}
}  // namespace

struct TF_Tensor {
  TF_DataType dtype;
  TensorShape shape;
  TensorBuffer* buffer;
};

TF_Tensor* TF_NewTensor(TF_DataType dtype, const int64_t* dims, int num_dims,
                        void* data, size_t len,
                        void (*deallocator)(void* data, size_t len, void* arg),
                        void* deallocator_arg) {
  std::vector<tensorflow::int64> dimvec(num_dims);
  for (int i = 0; i < num_dims; i++) {
    dimvec[i] = static_cast<tensorflow::int64>(dims[i]);
  }

  TF_ManagedBuffer* buf = new TF_ManagedBuffer;
  buf->len_ = len;
  if (reinterpret_cast<intptr_t>(data) % EIGEN_MAX_ALIGN_BYTES != 0) {
    // Copy the data into a buffer that satisfies Eigen's alignment
    // requirements.
    buf->data_ =
        tensorflow::cpu_allocator()->AllocateRaw(EIGEN_MAX_ALIGN_BYTES, len);
    if (tensorflow::LogMemory::IsEnabled()) {
      tensorflow::LogMemory::RecordRawAllocation(
          "TF_NewTensor",
          tensorflow::LogMemory::EXTERNAL_TENSOR_ALLOCATION_STEP_ID, len,
          buf->data_, tensorflow::cpu_allocator());
    }
    std::memcpy(buf->data_, data, len);
    buf->deallocator_ = deallocate_realigned_buffer;
    buf->deallocator_arg_ = nullptr;
    // Free the original buffer.
    deallocator(data, len, deallocator_arg);
  } else {
    buf->data_ = data;
    buf->deallocator_ = deallocator;
    buf->deallocator_arg_ = deallocator_arg;
  }
  return new TF_Tensor{dtype, TensorShape(dimvec), buf};
}

void TF_DeleteTensor(TF_Tensor* t) {
  t->buffer->Unref();
  delete t;
}

TF_DataType TF_TensorType(const TF_Tensor* t) { return t->dtype; }
int TF_NumDims(const TF_Tensor* t) { return t->shape.dims(); }
int64_t TF_Dim(const TF_Tensor* t, int dim_index) {
  return static_cast<int64_t>(t->shape.dim_size(dim_index));
}
size_t TF_TensorByteSize(const TF_Tensor* t) { return t->buffer->size(); }
void* TF_TensorData(const TF_Tensor* t) { return t->buffer->data(); }

// --------------------------------------------------------------------------
struct TF_SessionOptions {
  SessionOptions options;
};
TF_SessionOptions* TF_NewSessionOptions() { return new TF_SessionOptions; }
void TF_DeleteSessionOptions(TF_SessionOptions* opt) { delete opt; }

void TF_SetTarget(TF_SessionOptions* options, const char* target) {
  options->options.target = target;
}

void TF_SetConfig(TF_SessionOptions* options, const void* proto,
                  size_t proto_len, TF_Status* status) {
  if (!options->options.config.ParseFromArray(proto, proto_len)) {
    status->status =
        tensorflow::errors::InvalidArgument("Unparseable ConfigProto");
  }
}
// --------------------------------------------------------------------------
TF_Buffer* TF_NewBuffer() { return new TF_Buffer{nullptr, 0, nullptr}; }

TF_Buffer* TF_NewBufferFromString(const void* proto, size_t proto_len) {
  void* copy = malloc(proto_len);
  memcpy(copy, proto, proto_len);

  TF_Buffer* buf = new TF_Buffer;
  buf->data = copy;
  buf->length = proto_len;
  buf->data_deallocator = [](void* data, size_t length) { free(data); };
  return buf;
}

void TF_DeleteBuffer(TF_Buffer* buffer) {
  if (buffer->data_deallocator != nullptr) {
    (*buffer->data_deallocator)(const_cast<void*>(buffer->data),
                                buffer->length);
  }
  delete buffer;
}

TF_Buffer TF_GetBuffer(TF_Buffer* buffer) { return *buffer; }

// --------------------------------------------------------------------------
struct TF_Session {
  Session* session;
};

TF_Session* TF_NewSession(const TF_SessionOptions* opt, TF_Status* status) {
  Session* session;
  status->status = NewSession(opt->options, &session);
  if (status->status.ok()) {
    return new TF_Session({session});
  } else {
    DCHECK_EQ(nullptr, session);
    return NULL;
  }
}

void TF_CloseSession(TF_Session* s, TF_Status* status) {
  status->status = s->session->Close();
}

void TF_DeleteSession(TF_Session* s, TF_Status* status) {
  status->status = Status::OK();
  delete s->session;
  delete s;
}

void TF_ExtendGraph(TF_Session* s, const void* proto, size_t proto_len,
                    TF_Status* status) {
  GraphDef g;
  if (!tensorflow::ParseProtoUnlimited(&g, proto, proto_len)) {
    status->status = tensorflow::errors::InvalidArgument("Invalid GraphDef");
    return;
  }
  status->status = s->session->Extend(g);
}

static void DeleteArray(void* data, size_t size, void* arg) {
  DCHECK_EQ(data, arg);
  delete[] reinterpret_cast<char*>(arg);
}

}  // end extern "C"

namespace tensorflow {
namespace {

// Reset helper for converting character arrays to string vectors.
void TF_Reset_Helper(const TF_SessionOptions* opt, const char** containers,
                     int ncontainers, TF_Status* status) {
  std::vector<tensorflow::string> container_names(ncontainers);
  for (int i = 0; i < ncontainers; i++) {
    container_names[i] = containers[i];
  }

  status->status = Reset(opt->options, container_names);
}

}  // namespace
}  // namespace tensorflow

extern "C" {

void TF_Reset(const TF_SessionOptions* opt, const char** containers,
              int ncontainers, TF_Status* status) {
  tensorflow::TF_Reset_Helper(opt, containers, ncontainers, status);
}

}  // end extern "C"

namespace tensorflow {

// Non-static for testing.
bool TF_Tensor_DecodeStrings(TF_Tensor* src, Tensor* dst, TF_Status* status) {
  const tensorflow::int64 num_elements = src->shape.num_elements();
  const char* input = reinterpret_cast<const char*>(TF_TensorData(src));
  const size_t src_size = TF_TensorByteSize(src);
  if (static_cast<tensorflow::int64>(src_size / sizeof(tensorflow::uint64)) <
      num_elements) {
    status->status = InvalidArgument(
        "Malformed TF_STRING tensor; too short to hold number of elements");
    return false;
  }
  const char* data_start = input + sizeof(tensorflow::uint64) * num_elements;
  const char* limit = input + src_size;

  *dst = Tensor(static_cast<DataType>(src->dtype), src->shape);
  auto dstarray = dst->flat<tensorflow::string>();
  for (tensorflow::int64 i = 0; i < num_elements; i++) {
    tensorflow::uint64 offset =
        reinterpret_cast<const tensorflow::uint64*>(input)[i];
    tensorflow::uint64 len;
    const char* p;
    if (static_cast<ptrdiff_t>(offset) >= (limit - data_start) ||
        !(p = tensorflow::core::GetVarint64Ptr(data_start + offset, limit,
                                               &len)) ||
        (static_cast<ptrdiff_t>(len) > (limit - p))) {
      status->status = InvalidArgument("Malformed TF_STRING tensor; element ",
                                       i, " out of range");
      return false;
    }
    dstarray(i).assign(p, len);
  }
  return true;
}

// Non-static for testing.
TF_Tensor* TF_Tensor_EncodeStrings(const Tensor& src) {
  // Compute bytes needed for encoding.
  size_t size = 0;
  const auto& srcarray = src.flat<tensorflow::string>();
  for (int i = 0; i < srcarray.size(); i++) {
    const tensorflow::string& s = srcarray(i);
    // uint64 starting_offset, varint64 length, string contents
    size += sizeof(tensorflow::uint64) +
            tensorflow::core::VarintLength(s.size()) + s.size();
  }

  // Encode all strings.
  char* base = new char[size];
  char* data_start = base + sizeof(tensorflow::uint64) * srcarray.size();
  char* dst = data_start;  // Where next string is encoded.
  tensorflow::uint64* offsets = reinterpret_cast<tensorflow::uint64*>(base);
  for (int i = 0; i < srcarray.size(); i++) {
    const tensorflow::string& s = srcarray(i);
    *offsets = (dst - data_start);
    offsets++;
    dst = tensorflow::core::EncodeVarint64(dst, s.size());
    memcpy(dst, s.data(), s.size());
    dst += s.size();
  }
  CHECK_EQ(dst, base + size);

  auto dims = src.shape().dim_sizes();
  std::vector<tensorflow::int64> dimvec(dims.size());
  for (size_t i = 0; i < dims.size(); i++) {
    dimvec[i] = dims[i];
  }
  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  return TF_NewTensor(TF_STRING,
                      reinterpret_cast<const int64_t*>(dimvec.data()),
                      dimvec.size(), base, size, DeleteArray, base);
}

class TensorCApi {
 public:
  static TensorBuffer* Buffer(const Tensor& tensor) { return tensor.buf_; }
  static Tensor MakeTensor(TF_DataType type, const TensorShape& shape,
                           TensorBuffer* buf) {
    return Tensor(static_cast<DataType>(type), shape, buf);
  }
};

// Create an empty tensor of type 'dtype'. 'shape' can be arbitrary, but has to
// result in a zero-sized tensor.
static TF_Tensor* EmptyTensor(TF_DataType dtype, const TensorShape& shape) {
  static char empty;
  tensorflow::int64 nelems = 1;
  std::vector<tensorflow::int64> dims;
  for (int i = 0; i < shape.dims(); ++i) {
    dims.push_back(shape.dim_size(i));
    nelems *= shape.dim_size(i);
  }
  CHECK_EQ(nelems, 0);
  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  return TF_NewTensor(dtype, reinterpret_cast<const int64_t*>(dims.data()),
                      shape.dims(), reinterpret_cast<void*>(&empty), 0,
                      [](void*, size_t, void*) {}, nullptr);
}

// Helpers for loading a TensorFlow plugin (a .so file).
Status LoadLibrary(const char* library_filename, void** result,
                   const void** buf, size_t* len);

}  // namespace tensorflow

void TF_Run_Helper(Session* session, const char* handle,
                   const TF_Buffer* run_options,
                   // Input tensors
                   const char** c_input_names, TF_Tensor* const* c_inputs,
                   int ninputs,
                   // Output tensors
                   const char** c_output_tensor_names, TF_Tensor** c_outputs,
                   int noutputs,
                   // Target nodes
                   const char** c_target_node_names, int ntargets,
                   TF_Buffer* run_metadata, TF_Status* status) {
  status->status = Status::OK();
  for (int i = 0; i < noutputs; i++) {
    c_outputs[i] = NULL;
  }

  // Initialize inputs.
  std::vector<std::pair<tensorflow::string, Tensor>> inputs(ninputs);
  bool ok = true;
  for (int i = 0; i < ninputs; i++) {
    TF_Tensor* src = c_inputs[i];
    if (ok) {
      inputs[i].first = c_input_names[i];
      if (c_inputs[i]->dtype != TF_STRING) {
        inputs[i].second = tensorflow::TensorCApi::MakeTensor(
            src->dtype, src->shape, src->buffer);
      } else {
        // TF_STRING tensors require copying since Tensor class expects
        // a sequence of string objects.
        ok =
            tensorflow::TF_Tensor_DecodeStrings(src, &inputs[i].second, status);
        // Must keep looping through all inputs even if there is an error
        // so that TF_DeleteTensor() is called unconditionally on all inputs.
      }
    }
    TF_DeleteTensor(src);
  }
  if (!ok) {
    return;
  }

  std::vector<tensorflow::string> output_tensor_names(noutputs);
  std::vector<Tensor> outputs(noutputs);
  std::vector<tensorflow::string> target_node_names(ntargets);
  for (int i = 0; i < noutputs; i++) {
    output_tensor_names[i] = c_output_tensor_names[i];
  }
  for (int i = 0; i < ntargets; i++) {
    target_node_names[i] = c_target_node_names[i];
  }
  Status result;

  if (handle == nullptr) {
    RunOptions run_options_proto;
    if (run_options != nullptr &&
        !run_options_proto.ParseFromArray(run_options->data,
                                          run_options->length)) {
      status->status =
          tensorflow::errors::InvalidArgument("Unparseable RunOptions proto");
      return;
    }
    if (run_metadata != nullptr && run_metadata->data != nullptr) {
      status->status = tensorflow::errors::InvalidArgument(
          "Passing non-empty run_metadata is invalid.");
      return;
    }

    RunMetadata run_metadata_proto;
    result = session->Run(run_options_proto, inputs, output_tensor_names,
                          target_node_names, &outputs, &run_metadata_proto);

    // Serialize back to upstream client, who now owns the new buffer
    if (run_metadata != nullptr) {
      int proto_size = run_metadata_proto.ByteSize();
      void* str_buf = malloc(proto_size);
      run_metadata_proto.SerializeToArray(str_buf, proto_size);
      run_metadata->data = str_buf;
      run_metadata->length = proto_size;
      run_metadata->data_deallocator = [](void* data, size_t length) {
        free(data);
      };
    }
  } else {
    // NOTE(zongheng): PRun does not support RunOptions yet.
    result = session->PRun(handle, inputs, output_tensor_names, &outputs);
  }
  if (!result.ok()) {
    status->status = result;
    return;
  }

  // Store results in c_outputs[]
  for (int i = 0; i < noutputs; i++) {
    const Tensor& src = outputs[i];
    if (!src.IsInitialized() || src.NumElements() == 0) {
      c_outputs[i] = tensorflow::EmptyTensor(
          static_cast<TF_DataType>(src.dtype()), src.shape());
      continue;
    }
    if (src.dtype() != tensorflow::DT_STRING) {
      // Share the underlying buffer.
      TensorBuffer* buf = tensorflow::TensorCApi::Buffer(src);
      buf->Ref();
      c_outputs[i] = new TF_Tensor{static_cast<TF_DataType>(src.dtype()),
                                   src.shape(), buf};
    } else {
      c_outputs[i] = tensorflow::TF_Tensor_EncodeStrings(src);
    }
  }
}

extern "C" {

void TF_Run(TF_Session* s, const TF_Buffer* run_options,
            // Input tensors
            const char** c_input_names, TF_Tensor** c_inputs, int ninputs,
            // Output tensors
            const char** c_output_tensor_names, TF_Tensor** c_outputs,
            int noutputs,
            // Target nodes
            const char** c_target_node_names, int ntargets,
            TF_Buffer* run_metadata, TF_Status* status) {
  TF_Run_Helper(s->session, nullptr, run_options, c_input_names, c_inputs,
                ninputs, c_output_tensor_names, c_outputs, noutputs,
                c_target_node_names, ntargets, run_metadata, status);
}

void TF_PRunSetup(TF_Session* s,
                  // Input names
                  const char** c_input_names, int ninputs,
                  // Output names
                  const char** c_output_tensor_names, int noutputs,
                  // Target nodes
                  const char** c_target_node_names, int ntargets, char** handle,
                  TF_Status* status) {
  status->status = Status::OK();

  std::vector<tensorflow::string> input_names(ninputs);
  std::vector<tensorflow::string> output_tensor_names(noutputs);
  std::vector<tensorflow::string> target_node_names(ntargets);
  for (int i = 0; i < ninputs; i++) {
    input_names[i] = c_input_names[i];
  }
  for (int i = 0; i < noutputs; i++) {
    output_tensor_names[i] = c_output_tensor_names[i];
  }
  for (int i = 0; i < ntargets; i++) {
    target_node_names[i] = c_target_node_names[i];
  }
  tensorflow::string new_handle;
  Status result;
  result = s->session->PRunSetup(input_names, output_tensor_names,
                                 target_node_names, &new_handle);
  if (result.ok()) {
    *handle = new char[new_handle.size() + 1];
    memcpy(*handle, new_handle.c_str(), new_handle.size() + 1);
  } else {
    status->status = result;
  }
}

void TF_PRun(TF_Session* s, const char* handle,
             // Input tensors
             const char** c_input_names, TF_Tensor** c_inputs, int ninputs,
             // Output tensors
             const char** c_output_tensor_names, TF_Tensor** c_outputs,
             int noutputs,
             // Target nodes
             const char** c_target_node_names, int ntargets,
             TF_Status* status) {
  TF_Run_Helper(s->session, handle, nullptr, c_input_names, c_inputs, ninputs,
                c_output_tensor_names, c_outputs, noutputs, c_target_node_names,
                ntargets, nullptr, status);
}

struct TF_Library {
  void* lib_handle;
  TF_Buffer op_list;
};

TF_Library* TF_LoadLibrary(const char* library_filename, TF_Status* status) {
  TF_Library* lib_handle = new TF_Library;
  status->status = tensorflow::LoadLibrary(
      library_filename, &lib_handle->lib_handle, &lib_handle->op_list.data,
      &lib_handle->op_list.length);
  if (!status->status.ok()) {
    delete lib_handle;
    return nullptr;
  }
  return lib_handle;
}

TF_Buffer TF_GetOpList(TF_Library* lib_handle) { return lib_handle->op_list; }

}  // end extern "C"

// --------------------------------------------------------------------------
// New Graph and Session API

// Structures -----------------------------------------------------------------

extern "C" {

struct TF_Graph {
  TF_Graph()
      : graph(OpRegistry::Global()), num_sessions(0), delete_requested(false) {}
  mutex mu;  // protects all of the following
  Graph graph;
  std::unordered_map<tensorflow::string, Node*> name_map;

  // TF_Graph may only / must be deleted when
  //   num_sessions == 0 && delete_requested == true

  // num_sessions incremented by TF_NewSessionWithGraph, and decremented by
  // TF_DeleteSessionWithGraph.
  int num_sessions;
  bool delete_requested;  // set true by TF_DeleteGraph
};

struct TF_NodeDescription {
  TF_NodeDescription(TF_Graph* g, const char* op_type, const char* node_name)
      : node_builder(node_name, op_type, g->graph.op_registry()), graph(g) {}

  NodeBuilder node_builder;
  TF_Graph* graph;
};

struct TF_Node {
  Node node;
};

struct TF_SessionWithGraph {
  TF_SessionWithGraph(Session* s, TF_Graph* g)
      : session(s), graph(g), last_num_graph_nodes(0) {}
  Session* session;
  TF_Graph* graph;
  mutex mu;
  int last_num_graph_nodes;
};

}  // end extern "C"

// Helper functions -----------------------------------------------------------

namespace {

TF_Node* ToNode(Node* node) {
  return static_cast<TF_Node*>(static_cast<void*>(node));
}

tensorflow::string PortName(const TF_Port& port) {
  return tensorflow::strings::StrCat(port.node->node.name(), ":", port.index);
}

}  // namespace

// TF_NodeDescription functions -----------------------------------------------

extern "C" {

TF_NodeDescription* TF_NewNode(TF_Graph* graph, const char* op_type,
                               const char* node_name) {
  mutex_lock l(graph->mu);
  return new TF_NodeDescription(graph, op_type, node_name);
}

void TF_SetDevice(TF_NodeDescription* desc, const char* device) {
  desc->node_builder.Device(device);
}

void TF_AddInput(TF_NodeDescription* desc, TF_Port input) {
  desc->node_builder.Input(&input.node->node, input.index);
}

void TF_AddInputList(TF_NodeDescription* desc, const TF_Port* inputs,
                     int num_inputs) {
  std::vector<NodeBuilder::NodeOut> input_list;
  input_list.reserve(num_inputs);
  for (int i = 0; i < num_inputs; ++i) {
    input_list.emplace_back(&inputs[i].node->node, inputs[i].index);
  }
  desc->node_builder.Input(input_list);
}

void TF_AddControlInput(TF_NodeDescription* desc, TF_Node* input) {
  desc->node_builder.ControlInput(&input->node);
}

void TF_SetAttrString(TF_NodeDescription* desc, const char* attr_name,
                      const void* value, int length) {
  tensorflow::StringPiece s(static_cast<const char*>(value), length);
  desc->node_builder.Attr(attr_name, s);
}

void TF_SetAttrStringList(TF_NodeDescription* desc, const char* attr_name,
                          const void* const* values, const int* lengths,
                          int num_values) {
  std::vector<tensorflow::StringPiece> v;
  v.reserve(num_values);
  for (int i = 0; i < num_values; ++i) {
    v.emplace_back(static_cast<const char*>(values[i]), lengths[i]);
  }
  desc->node_builder.Attr(attr_name, v);
}

void TF_SetAttrInt(TF_NodeDescription* desc, const char* attr_name,
                   int64_t value) {
  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  desc->node_builder.Attr(attr_name, static_cast<tensorflow::int64>(value));
}

void TF_SetAttrIntList(TF_NodeDescription* desc, const char* attr_name,
                       const int64_t* values, int num_values) {
  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  desc->node_builder.Attr(
      attr_name,
      ArraySlice<const tensorflow::int64>(
          reinterpret_cast<const tensorflow::int64*>(values), num_values));
}

void TF_SetAttrFloat(TF_NodeDescription* desc, const char* attr_name,
                     float value) {
  desc->node_builder.Attr(attr_name, value);
}

void TF_SetAttrFloatList(TF_NodeDescription* desc, const char* attr_name,
                         const float* values, int num_values) {
  desc->node_builder.Attr(attr_name,
                          ArraySlice<const float>(values, num_values));
}

void TF_SetAttrBool(TF_NodeDescription* desc, const char* attr_name,
                    unsigned char value) {
  desc->node_builder.Attr(attr_name, static_cast<bool>(value));
}

void TF_SetAttrBoolList(TF_NodeDescription* desc, const char* attr_name,
                        const unsigned char* values, int num_values) {
  bool* b = new bool[num_values];
  for (int i = 0; i < num_values; ++i) {
    b[i] = values[i];
  }
  desc->node_builder.Attr(attr_name, ArraySlice<const bool>(b, num_values));
}

void TF_SetAttrType(TF_NodeDescription* desc, const char* attr_name,
                    TF_DataType value) {
  desc->node_builder.Attr(attr_name, static_cast<DataType>(value));
}

void TF_SetAttrTypeList(TF_NodeDescription* desc, const char* attr_name,
                        const TF_DataType* values, int num_values) {
  desc->node_builder.Attr(
      attr_name, ArraySlice<const DataType>(
                     reinterpret_cast<const DataType*>(values), num_values));
}

void TF_SetAttrShape(TF_NodeDescription* desc, const char* attr_name,
                     const int64_t* dims, int num_dims) {
  PartialTensorShape shape;
  if (num_dims >= 0) {
    static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                  "64-bit int types should match in size");
    shape = PartialTensorShape(ArraySlice<tensorflow::int64>(
        reinterpret_cast<const tensorflow::int64*>(dims), num_dims));
  }
  desc->node_builder.Attr(attr_name, shape);
}

void TF_SetAttrShapeList(TF_NodeDescription* desc, const char* attr_name,
                         const int64_t* const* dims, const int* num_dims,
                         int num_shapes) {
  std::vector<PartialTensorShape> shapes;
  shapes.reserve(num_shapes);
  for (int i = 0; i < num_shapes; ++i) {
    if (num_dims[i] < 0) {
      shapes.emplace_back();
    } else {
      static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                    "64-bit int types should match in size");
      shapes.emplace_back(ArraySlice<tensorflow::int64>(
          reinterpret_cast<const tensorflow::int64*>(dims[i]), num_dims[i]));
    }
  }
  desc->node_builder.Attr(attr_name, shapes);
}

void TF_SetAttrTensorShapeProto(TF_NodeDescription* desc, const char* attr_name,
                                void* proto, int proto_len, TF_Status* status) {
  TensorShapeProto shape;
  if (shape.ParseFromArray(proto, proto_len)) {
    desc->node_builder.Attr(attr_name, shape);
    status->status = Status::OK();
  } else {
    status->status =
        tensorflow::errors::InvalidArgument("Unparseable TensorShapeProto");
  }
}

void TF_SetAttrTensorShapeProtoList(TF_NodeDescription* desc,
                                    const char* attr_name,
                                    const void* const* protos,
                                    const int* proto_lens, int num_shapes,
                                    TF_Status* status) {
  std::vector<TensorShapeProto> shapes;
  shapes.resize(num_shapes);
  for (int i = 0; i < num_shapes; ++i) {
    if (!shapes[i].ParseFromArray(protos[i], proto_lens[i])) {
      status->status = tensorflow::errors::InvalidArgument(
          "Unparseable TensorShapeProto at index ", i);
      return;
    }
  }
  desc->node_builder.Attr(attr_name, shapes);
  status->status = Status::OK();
}

void TF_SetAttrTensor(TF_NodeDescription* desc, const char* attr_name,
                      TF_Tensor* value, TF_Status* status) {
  status->status = Status::OK();
  Tensor t;
  bool ok = true;

  if (value->dtype != TF_STRING) {
    t = tensorflow::TensorCApi::MakeTensor(value->dtype, value->shape,
                                           value->buffer);
  } else {
    // TF_STRING tensors require copying since Tensor class expects
    // a sequence of string objects.
    ok = tensorflow::TF_Tensor_DecodeStrings(value, &t, status);
  }

  TF_DeleteTensor(value);
  if (ok) desc->node_builder.Attr(attr_name, t);
}

void TF_SetAttrTensorList(TF_NodeDescription* desc, const char* attr_name,
                          TF_Tensor* const* values, int num_values,
                          TF_Status* status) {
  status->status = Status::OK();
  std::vector<Tensor> t;
  t.reserve(num_values);
  bool ok = true;

  for (int i = 0; i < num_values; ++i) {
    if (ok) {
      if (values[i]->dtype != TF_STRING) {
        t.emplace_back(tensorflow::TensorCApi::MakeTensor(
            values[i]->dtype, values[i]->shape, values[i]->buffer));
      } else {
        t.emplace_back(::tensorflow::DT_STRING);
        // TF_STRING tensors require copying since Tensor class expects
        // a sequence of string objects.
        ok = tensorflow::TF_Tensor_DecodeStrings(values[i], &t.back(), status);
      }
    }
    // We always delete value[i], even when there is an error,
    // as promised in the API.
    TF_DeleteTensor(values[i]);
  }

  if (ok) desc->node_builder.Attr(attr_name, t);
}

void TF_SetAttrToAttrValueProto(TF_NodeDescription* desc, const char* attr_name,
                                const void* proto, size_t proto_len,
                                TF_Status* status) {
  tensorflow::AttrValue attr_value;
  if (attr_value.ParseFromArray(proto, proto_len)) {
    desc->node_builder.Attr(attr_name, attr_value);
    status->status = Status::OK();
  } else {
    status->status =
        tensorflow::errors::InvalidArgument("Unparseable AttrValue proto");
  }
}

TF_Node* TF_FinishNode(TF_NodeDescription* desc, TF_Status* status) {
  Node* ret = nullptr;
  mutex_lock l(desc->graph->mu);

  if (desc->graph->name_map.count(desc->node_builder.node_name())) {
    status->status = tensorflow::errors::InvalidArgument(
        "Duplicate node name in graph: '", desc->node_builder.node_name(), "'");
  } else {
    status->status = desc->node_builder.Finalize(&desc->graph->graph, &ret);
    if (status->status.ok()) {
      desc->graph->name_map[ret->name()] = ret;
    }
  }

  delete desc;

  return ToNode(ret);
}

// TF_Node functions ----------------------------------------------------------

const char* TF_NodeName(TF_Node* node) { return node->node.name().c_str(); }

const char* TF_NodeOpType(TF_Node* node) {
  return node->node.type_string().c_str();
}

const char* TF_NodeDevice(TF_Node* node) {
  return node->node.def().device().c_str();
}

int TF_NodeNumOutputs(TF_Node* node) { return node->node.num_outputs(); }

TF_DataType TF_NodeOutputType(TF_Port node_out) {
  return static_cast<TF_DataType>(
      node_out.node->node.output_type(node_out.index));
}

int TF_NodeOutputListLength(TF_Node* node, const char* arg_name,
                            TF_Status* status) {
  NameRangeMap name_ranges;
  status->status = NameRangesForNode(node->node.def(), node->node.op_def(),
                                     nullptr, &name_ranges);
  if (!status->status.ok()) return -1;
  auto iter = name_ranges.find(arg_name);
  if (iter == name_ranges.end()) {
    status->status = tensorflow::errors::InvalidArgument(
        "Input arg '", arg_name, "' not found");
    return -1;
  }
  return iter->second.second - iter->second.first;
}

int TF_NodeNumInputs(TF_Node* node) { return node->node.num_inputs(); }

TF_DataType TF_NodeInputType(TF_Port node_in) {
  return static_cast<TF_DataType>(node_in.node->node.input_type(node_in.index));
}

int TF_NodeInputListLength(TF_Node* node, const char* arg_name,
                           TF_Status* status) {
  NameRangeMap name_ranges;
  status->status = NameRangesForNode(node->node.def(), node->node.op_def(),
                                     &name_ranges, nullptr);
  if (!status->status.ok()) return -1;
  auto iter = name_ranges.find(arg_name);
  if (iter == name_ranges.end()) {
    status->status = tensorflow::errors::InvalidArgument(
        "Input arg '", arg_name, "' not found");
    return -1;
  }
  return iter->second.second - iter->second.first;
}

TF_Port TF_NodeInput(TF_Port node_in) {
  for (const auto* edge : node_in.node->node.in_edges()) {
    if (edge->dst_input() == node_in.index) {
      return {ToNode(edge->src()), edge->src_output()};
    }
  }
  return {nullptr, -1};
}

int TF_NodeOutputNumConsumers(TF_Port node_out) {
  int count = 0;
  for (const auto* edge : node_out.node->node.out_edges()) {
    if (edge->src_output() == node_out.index) {
      ++count;
    }
  }
  return count;
}

int TF_NodeOutputConsumers(TF_Port node_out, TF_Port* consumers,
                           int max_consumers) {
  int count = 0;
  for (const auto* edge : node_out.node->node.out_edges()) {
    if (edge->src_output() == node_out.index) {
      if (count < max_consumers) {
        consumers[count] = {ToNode(edge->dst()), edge->dst_input()};
      }
      ++count;
    }
  }
  return count;
}

int TF_NodeNumControlInputs(TF_Node* node) {
  int count = 0;
  for (const auto* edge : node->node.in_edges()) {
    if (edge->IsControlEdge()) {
      ++count;
    }
  }
  return count;
}

int TF_NodeGetControlInputs(TF_Node* node, TF_Node** control_inputs,
                            int max_control_inputs) {
  int count = 0;
  for (const auto* edge : node->node.in_edges()) {
    if (edge->IsControlEdge()) {
      if (count < max_control_inputs) {
        control_inputs[count] = ToNode(edge->src());
      }
      ++count;
    }
  }
  return count;
}

int TF_NodeNumControlOutputs(TF_Node* node) {
  int count = 0;
  for (const auto* edge : node->node.out_edges()) {
    if (edge->IsControlEdge()) {
      ++count;
    }
  }
  return count;
}

int TF_NodeGetControlOutputs(TF_Node* node, TF_Node** control_outputs,
                             int max_control_outputs) {
  int count = 0;
  for (const auto* edge : node->node.out_edges()) {
    if (edge->IsControlEdge()) {
      if (count < max_control_outputs) {
        control_outputs[count] = ToNode(edge->dst());
      }
      ++count;
    }
  }
  return count;
}

void TF_NodeToNodeDef(TF_Node* node, TF_Buffer* output_node_def,
                      TF_Status* status) {
  if (output_node_def->data != nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "Passing non-empty output_node_def is invalid.");
    return;
  }

  const NodeDef& def = node->node.def();
  const auto proto_size = def.ByteSize();
  void* str_buf = malloc(proto_size);
  def.SerializeToArray(str_buf, proto_size);
  output_node_def->data = str_buf;
  output_node_def->length = proto_size;
  output_node_def->data_deallocator = [](void* data, size_t length) {
    free(data);
  };
  status->status = Status::OK();
}

// TF_Graph functions ---------------------------------------------------------

TF_Graph* TF_NewGraph() { return new TF_Graph; }

void TF_DeleteGraph(TF_Graph* g) {
  g->mu.lock();
  g->delete_requested = true;
  const bool del = g->num_sessions == 0;
  g->mu.unlock();
  if (del) delete g;
}

TF_Node* TF_GraphNodeByName(TF_Graph* graph, const char* node_name) {
  mutex_lock l(graph->mu);
  auto iter = graph->name_map.find(node_name);
  if (iter == graph->name_map.end()) {
    return nullptr;
  } else {
    return ToNode(iter->second);
  }
}

TF_Node* TF_GraphNextNode(TF_Graph* graph, size_t* pos) {
  if (*pos == 0) {
    // Advance past the first sentinal nodes in every graph (the source & sink).
    *pos += 2;
  } else {
    // Advance to the next node.
    *pos += 1;
  }

  mutex_lock l(graph->mu);
  while (*pos < graph->graph.num_node_ids()) {
    Node* node = graph->graph.FindNodeId(*pos);
    // FindNodeId() returns nullptr for nodes that have been deleted.
    // We aren't currently allowing nodes to be deleted, but it is safer
    // to still check.
    if (node != nullptr) return reinterpret_cast<TF_Node*>(node);
    *pos += 1;
  }

  // No more nodes.
  return nullptr;
}

void TF_GraphToGraphDef(TF_Graph* graph, TF_Buffer* output_graph_def,
                        TF_Status* status) {
  if (output_graph_def->data != nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "Passing non-empty output_graph_def is invalid.");
    return;
  }

  GraphDef def;
  {
    mutex_lock l(graph->mu);
    graph->graph.ToGraphDef(&def);
  }

  const auto proto_size = def.ByteSize();
  void* str_buf = malloc(proto_size);
  def.SerializeToArray(str_buf, proto_size);
  output_graph_def->data = str_buf;
  output_graph_def->length = proto_size;
  output_graph_def->data_deallocator = [](void* data, size_t length) {
    free(data);
  };
  status->status = Status::OK();
}

// TF_SessionWithGraph functions ----------------------------------------------

TF_SessionWithGraph* TF_NewSessionWithGraph(TF_Graph* graph,
                                            const TF_SessionOptions* opt,
                                            TF_Status* status) {
  Session* session;
  status->status = NewSession(opt->options, &session);
  if (status->status.ok()) {
    if (graph != nullptr) {
      mutex_lock l(graph->mu);
      graph->num_sessions += 1;
    }
    return new TF_SessionWithGraph(session, graph);
  } else {
    DCHECK_EQ(nullptr, session);
    return NULL;
  }
}

void TF_CloseSessionWithGraph(TF_SessionWithGraph* s, TF_Status* status) {
  status->status = s->session->Close();
}

void TF_DeleteSessionWithGraph(TF_SessionWithGraph* s, TF_Status* status) {
  status->status = Status::OK();
  TF_Graph* const graph = s->graph;
  if (graph != nullptr) {
    graph->mu.lock();
    graph->num_sessions -= 1;
    const bool del = graph->delete_requested && graph->num_sessions == 0;
    graph->mu.unlock();
    if (del) delete graph;
  }
  delete s->session;
  delete s;
}

void TF_SessionRun(TF_SessionWithGraph* session,
                   // RunOptions
                   const TF_Buffer* run_options,
                   // Input tensors
                   const TF_Port* inputs, TF_Tensor* const* input_values,
                   int ninputs,
                   // Output tensors
                   const TF_Port* outputs, TF_Tensor** output_values,
                   int noutputs,
                   // Target nodes
                   const TF_Node* const* target_nodes, int ntargets,
                   // RunMetadata
                   TF_Buffer* run_metadata,
                   // Output status
                   TF_Status* status) {
  // TODO(josh11b,mrry): Change Session to be able to use a Graph*
  // directly, instead of requiring us to serialize to a GraphDef and
  // call Session::Extend().
  if (session->graph != nullptr) {
    mutex_lock session_lock(session->mu);
    session->graph->mu.lock();
    const Graph& graph = session->graph->graph;
    const auto num_nodes = graph.num_node_ids();
    if (session->last_num_graph_nodes < num_nodes) {
      GraphDef graph_def;
      graph_def.mutable_versions()->CopyFrom(graph.versions());
      // Fill graph_def with nodes with ids in the range
      // [session->last_num_graph_nodes, num_nodes), that is the nodes
      // added since the last TF_SessionRun() call.
      for (auto id = session->last_num_graph_nodes; id < num_nodes; ++id) {
        Node* const node = graph.FindNodeId(id);
        if (node != nullptr && node->IsOp()) {
          NodeDef* const node_def = graph_def.add_node();
          *node_def = node->def();
        }
      }
      session->graph->mu.unlock();
      // TODO(josh11b): Also send the function library if needed.
      status->status = session->session->Extend(graph_def);
      if (!status->status.ok()) {
        // Contract is we always delete input_values[i].
        for (int i = 0; i < ninputs; i++) {
          TF_DeleteTensor(input_values[i]);
        }
        return;
      }
      // Note: session->session is not modified if Extend() fails, so
      // we only set last_num_graph_nodes if it succeeds.
      session->last_num_graph_nodes = num_nodes;
    } else {
      session->graph->mu.unlock();
    }
  }

  // Convert from TF_Port and TF_Node* to const char* names.
  // TODO(josh11b): Should skip string -> const char* conversion here by
  // changing TF_Run_Helper().
  const int num_io = ninputs + noutputs;
  tensorflow::string* io_names = new tensorflow::string[num_io];
  const char** c_names = new const char*[num_io + ntargets];
  int index = 0;
  for (int i = 0; i < ninputs; ++i) {
    io_names[index] = PortName(inputs[i]);
    c_names[index] = io_names[index].c_str();
    ++index;
  }
  for (int i = 0; i < noutputs; ++i) {
    io_names[index] = PortName(outputs[i]);
    c_names[index] = io_names[index].c_str();
    ++index;
  }
  for (int i = 0; i < ntargets; ++i) {
    c_names[index] = target_nodes[i]->node.name().c_str();
    ++index;
  }
  TF_Run_Helper(session->session, nullptr, run_options, c_names, input_values,
                ninputs, c_names + ninputs, output_values, noutputs,
                c_names + num_io, ntargets, run_metadata, status);
  delete[] c_names;
  delete[] io_names;
}

}  // end extern "C"
