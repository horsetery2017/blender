#include "DNA_modifier_types.h"

#include "BKE_virtual_node_tree_cxx.h"
#include "BKE_multi_functions.h"
#include "BKE_tuple.h"
#include "BKE_multi_function_network.h"

#include "BLI_math_cxx.h"
#include "BLI_string_map.h"
#include "BLI_owned_resources.h"
#include "BLI_stack_cxx.h"

#include "DEG_depsgraph_query.h"

using BKE::CPPType;
using BKE::GenericArrayRef;
using BKE::GenericMutableArrayRef;
using BKE::GenericVectorArray;
using BKE::GenericVirtualListListRef;
using BKE::GenericVirtualListRef;
using BKE::MFBuilderFunctionNode;
using BKE::MFBuilderInputSocket;
using BKE::MFBuilderNode;
using BKE::MFBuilderOutputSocket;
using BKE::MFBuilderPlaceholderNode;
using BKE::MFBuilderSocket;
using BKE::MFContext;
using BKE::MFDataType;
using BKE::MFFunctionNode;
using BKE::MFInputSocket;
using BKE::MFNetwork;
using BKE::MFNetworkBuilder;
using BKE::MFNode;
using BKE::MFOutputSocket;
using BKE::MFParams;
using BKE::MFParamsBuilder;
using BKE::MFParamType;
using BKE::MFPlaceholderNode;
using BKE::MFSignature;
using BKE::MFSignatureBuilder;
using BKE::MFSocket;
using BKE::MultiFunction;
using BKE::TupleRef;
using BKE::VirtualLink;
using BKE::VirtualNode;
using BKE::VirtualNodeTree;
using BKE::VirtualSocket;
using BLI::Array;
using BLI::ArrayRef;
using BLI::float3;
using BLI::IndexRange;
using BLI::Map;
using BLI::OwnedResources;
using BLI::Stack;
using BLI::StringMap;
using BLI::StringRef;
using BLI::TemporaryVector;
using BLI::Vector;

extern "C" {
void MOD_functiondeform_do(FunctionDeformModifierData *fdmd, float (*vertexCos)[3], int numVerts);
}

static MFDataType get_type_by_socket(const VirtualSocket &vsocket)
{
  StringRef idname = vsocket.idname();

  if (idname == "fn_FloatSocket") {
    return MFDataType::ForSingle<float>();
  }
  else if (idname == "fn_VectorSocket") {
    return MFDataType::ForSingle<float3>();
  }
  else if (idname == "fn_IntegerSocket") {
    return MFDataType::ForSingle<int32_t>();
  }
  else if (idname == "fn_FloatListSocket") {
    return MFDataType::ForVector<float>();
  }
  else if (idname == "fn_VectorListSocket") {
    return MFDataType::ForVector<float3>();
  }
  else if (idname == "fn_IntegerListSocket") {
    return MFDataType::ForVector<int32_t>();
  }
  return MFDataType();
}

static const CPPType &get_cpp_type_by_name(StringRef name)
{
  if (name == "Float") {
    return BKE::GET_TYPE<float>();
  }
  else if (name == "Vector") {
    return BKE::GET_TYPE<float3>();
  }
  else if (name == "Integer") {
    return BKE::GET_TYPE<int32_t>();
  }

  BLI_assert(false);
  return BKE::GET_TYPE<float>();
}

class VTreeMFNetwork {
 private:
  const VirtualNodeTree &m_vtree;
  std::unique_ptr<MFNetwork> m_network;
  Array<const MFSocket *> m_socket_map;

 public:
  VTreeMFNetwork(const VirtualNodeTree &vtree,
                 std::unique_ptr<MFNetwork> network,
                 Array<const MFSocket *> socket_map)
      : m_vtree(vtree), m_network(std::move(network)), m_socket_map(std::move(socket_map))
  {
  }

  const VirtualNodeTree &vtree()
  {
    return m_vtree;
  }

  const MFNetwork &network()
  {
    return *m_network;
  }

  const MFSocket &lookup_socket(const VirtualSocket &vsocket)
  {
    return *m_socket_map[vsocket.id()];
  }
};

class VTreeMFNetworkBuilder {
 private:
  const VirtualNodeTree &m_vtree;
  Vector<MFBuilderSocket *> m_socket_map;
  Vector<MFDataType> m_type_by_vsocket;
  std::unique_ptr<MFNetworkBuilder> m_builder;

 public:
  VTreeMFNetworkBuilder(const VirtualNodeTree &vtree)
      : m_vtree(vtree),
        m_socket_map(vtree.socket_count(), nullptr),
        m_builder(BLI::make_unique<MFNetworkBuilder>())
  {
    m_type_by_vsocket = Vector<MFDataType>(m_vtree.socket_count());
    for (const VirtualNode *vnode : m_vtree.nodes()) {
      for (const VirtualSocket *vsocket : vnode->inputs()) {
        MFDataType data_type = get_type_by_socket(*vsocket);
        m_type_by_vsocket[vsocket->id()] = data_type;
      }
      for (const VirtualSocket *vsocket : vnode->outputs()) {
        MFDataType data_type = get_type_by_socket(*vsocket);
        m_type_by_vsocket[vsocket->id()] = data_type;
      }
    }
  }

  const VirtualNodeTree &vtree() const
  {
    return m_vtree;
  }

  MFBuilderFunctionNode &add_function(const MultiFunction &function,
                                      ArrayRef<uint> input_param_indices,
                                      ArrayRef<uint> output_param_indices)
  {
    return m_builder->add_function(function, input_param_indices, output_param_indices);
  }

  MFBuilderFunctionNode &add_function(const MultiFunction &function,
                                      ArrayRef<uint> input_param_indices,
                                      ArrayRef<uint> output_param_indices,
                                      const VirtualNode &vnode)
  {
    MFBuilderFunctionNode &node = m_builder->add_function(
        function, input_param_indices, output_param_indices);
    this->map_sockets_exactly(vnode, node);
    return node;
  }

  MFBuilderPlaceholderNode &add_placeholder(const VirtualNode &vnode)
  {
    Vector<MFDataType> input_types;
    for (const VirtualSocket *vsocket : vnode.inputs()) {
      MFDataType data_type = this->try_get_data_type(*vsocket);
      if (!data_type.is_none()) {
        input_types.append(data_type);
      }
    }

    Vector<MFDataType> output_types;
    for (const VirtualSocket *vsocket : vnode.outputs()) {
      MFDataType data_type = this->try_get_data_type(*vsocket);
      if (!data_type.is_none()) {
        output_types.append(data_type);
      }
    }

    MFBuilderPlaceholderNode &node = m_builder->add_placeholder(input_types, output_types);
    this->map_data_sockets(vnode, node);
    return node;
  }

  MFBuilderPlaceholderNode &add_placeholder(ArrayRef<MFDataType> input_types,
                                            ArrayRef<MFDataType> output_types)
  {
    return m_builder->add_placeholder(input_types, output_types);
  }

  void add_link(MFBuilderOutputSocket &from, MFBuilderInputSocket &to)
  {
    m_builder->add_link(from, to);
  }

  MFDataType try_get_data_type(const VirtualSocket &vsocket) const
  {
    return m_type_by_vsocket[vsocket.id()];
  }

  bool is_data_socket(const VirtualSocket &vsocket) const
  {
    return !m_type_by_vsocket[vsocket.id()].is_none();
  }

  void map_sockets_exactly(const VirtualNode &vnode, MFBuilderNode &node)
  {
    BLI_assert(vnode.inputs().size() == node.inputs().size());
    BLI_assert(vnode.outputs().size() == node.outputs().size());

    for (uint i = 0; i < vnode.inputs().size(); i++) {
      m_socket_map[vnode.inputs()[i]->id()] = node.inputs()[i];
    }
    for (uint i = 0; i < vnode.outputs().size(); i++) {
      m_socket_map[vnode.outputs()[i]->id()] = node.outputs()[i];
    }
  }

  void map_data_sockets(const VirtualNode &vnode, MFBuilderNode &node)
  {
    uint data_inputs = 0;
    for (const VirtualSocket *vsocket : vnode.inputs()) {
      if (this->is_data_socket(*vsocket)) {
        this->map_sockets(*vsocket, *node.inputs()[data_inputs]);
        data_inputs++;
      }
    }

    uint data_outputs = 0;
    for (const VirtualSocket *vsocket : vnode.outputs()) {
      if (this->is_data_socket(*vsocket)) {
        this->map_sockets(*vsocket, *node.outputs()[data_outputs]);
        data_outputs++;
      }
    }
  }

  void map_sockets(const VirtualSocket &vsocket, MFBuilderSocket &socket)
  {
    BLI_assert(m_socket_map[vsocket.id()] == nullptr);
    m_socket_map[vsocket.id()] = &socket;
  }

  bool vsocket_is_mapped(const VirtualSocket &vsocket) const
  {
    return m_socket_map[vsocket.id()] != nullptr;
  }

  bool data_sockets_are_mapped(ArrayRef<const VirtualSocket *> vsockets) const
  {
    for (const VirtualSocket *vsocket : vsockets) {
      if (this->is_data_socket(*vsocket)) {
        if (!this->vsocket_is_mapped(*vsocket)) {
          return false;
        }
      }
    }
    return true;
  }

  bool data_sockets_of_vnode_are_mapped(const VirtualNode &vnode) const
  {
    if (!this->data_sockets_are_mapped(vnode.inputs())) {
      return false;
    }
    if (!this->data_sockets_are_mapped(vnode.outputs())) {
      return false;
    }
    return true;
  }

  bool has_data_sockets(const VirtualNode &vnode) const
  {
    for (const VirtualSocket *vsocket : vnode.inputs()) {
      if (this->is_data_socket(*vsocket)) {
        return true;
      }
    }
    for (const VirtualSocket *vsocket : vnode.outputs()) {
      if (this->is_data_socket(*vsocket)) {
        return true;
      }
    }
    return false;
  }

  bool is_input_linked(const VirtualSocket &vsocket) const
  {
    auto &socket = this->lookup_input_socket(vsocket);
    return socket.as_input().origin() != nullptr;
  }

  MFBuilderOutputSocket &lookup_output_socket(const VirtualSocket &vsocket) const
  {
    BLI_assert(vsocket.is_output());
    MFBuilderSocket *socket = m_socket_map[vsocket.id()];
    BLI_assert(socket != nullptr);
    return socket->as_output();
  }

  MFBuilderInputSocket &lookup_input_socket(const VirtualSocket &vsocket) const
  {
    BLI_assert(vsocket.is_input());
    MFBuilderSocket *socket = m_socket_map[vsocket.id()];
    BLI_assert(socket != nullptr);
    return socket->as_input();
  }

  std::unique_ptr<VTreeMFNetwork> build()
  {
    Array<int> socket_ids(m_vtree.socket_count(), -1);
    for (uint vsocket_id = 0; vsocket_id < m_vtree.socket_count(); vsocket_id++) {
      MFBuilderSocket *builder_socket = m_socket_map[vsocket_id];
      if (builder_socket != nullptr) {
        socket_ids[vsocket_id] = builder_socket->id();
      }
    }

    auto network = BLI::make_unique<MFNetwork>(std::move(m_builder));

    Array<const MFSocket *> socket_map(m_vtree.socket_count(), nullptr);
    for (uint vsocket_id = 0; vsocket_id < m_vtree.socket_count(); vsocket_id++) {
      int id = socket_ids[vsocket_id];
      if (id != -1) {
        socket_map[vsocket_id] = &network->socket_by_id(socket_ids[vsocket_id]);
      }
    }

    return BLI::make_unique<VTreeMFNetwork>(m_vtree, std::move(network), std::move(socket_map));
  }
};

using InsertVNodeFunction = std::function<void(
    VTreeMFNetworkBuilder &builder, OwnedResources &resources, const VirtualNode &vnode)>;
using InsertUnlinkedInputFunction = std::function<MFBuilderOutputSocket &(
    VTreeMFNetworkBuilder &builder, OwnedResources &resources, const VirtualSocket &vsocket)>;
using InsertImplicitConversionFunction =
    std::function<std::pair<MFBuilderInputSocket *, MFBuilderOutputSocket *>(
        VTreeMFNetworkBuilder &builder, OwnedResources &resources)>;

static void INSERT_vector_math(VTreeMFNetworkBuilder &builder,
                               OwnedResources &resources,
                               const VirtualNode &vnode)
{
  auto function = BLI::make_unique<BKE::MultiFunction_AddFloat3s>();
  builder.add_function(*function, {0, 1}, {2}, vnode);
  resources.add(std::move(function), "vector math function");
}

static void INSERT_float_math(VTreeMFNetworkBuilder &builder,
                              OwnedResources &resources,
                              const VirtualNode &vnode)
{
  auto function = BLI::make_unique<BKE::MultiFunction_AddFloats>();
  builder.add_function(*function, {0, 1}, {2}, vnode);
  resources.add(std::move(function), "float math function");
}

static void INSERT_combine_vector(VTreeMFNetworkBuilder &builder,
                                  OwnedResources &resources,
                                  const VirtualNode &vnode)
{
  auto function = BLI::make_unique<BKE::MultiFunction_CombineVector>();
  builder.add_function(*function, {0, 1, 2}, {3}, vnode);
  resources.add(std::move(function), "combine vector function");
}

static void INSERT_separate_vector(VTreeMFNetworkBuilder &builder,
                                   OwnedResources &resources,
                                   const VirtualNode &vnode)
{
  auto function = BLI::make_unique<BKE::MultiFunction_SeparateVector>();
  builder.add_function(*function, {0}, {1, 2, 3}, vnode);
  resources.add(std::move(function), "separate vector function");
}

static void INSERT_append_to_list(VTreeMFNetworkBuilder &builder,
                                  OwnedResources &resources,
                                  const VirtualNode &vnode)
{
  PointerRNA rna = vnode.rna();
  char *type_name = RNA_string_get_alloc(&rna, "active_type", nullptr, 0);
  const CPPType &type = get_cpp_type_by_name(type_name);
  MEM_freeN(type_name);

  auto function = BLI::make_unique<BKE::MultiFunction_AppendToList>(type);
  builder.add_function(*function, {0, 1}, {0}, vnode);
  resources.add(std::move(function), "append to list function");
}

static void INSERT_list_length(VTreeMFNetworkBuilder &builder,
                               OwnedResources &resources,
                               const VirtualNode &vnode)
{
  PointerRNA rna = vnode.rna();
  char *type_name = RNA_string_get_alloc(&rna, "active_type", nullptr, 0);
  const CPPType &type = get_cpp_type_by_name(type_name);
  MEM_freeN(type_name);

  auto function = BLI::make_unique<BKE::MultiFunction_ListLength>(type);
  builder.add_function(*function, {0}, {1}, vnode);
  resources.add(std::move(function), "list length function");
}

static StringMap<InsertVNodeFunction> get_node_inserters()
{
  StringMap<InsertVNodeFunction> inserters;
  inserters.add_new("fn_FloatMathNode", INSERT_float_math);
  inserters.add_new("fn_VectorMathNode", INSERT_vector_math);
  inserters.add_new("fn_CombineVectorNode", INSERT_combine_vector);
  inserters.add_new("fn_SeparateVectorNode", INSERT_separate_vector);
  inserters.add_new("fn_AppendToListNode", INSERT_append_to_list);
  inserters.add_new("fn_ListLengthNode", INSERT_list_length);
  return inserters;
}

static MFBuilderOutputSocket &INSERT_vector_socket(VTreeMFNetworkBuilder &builder,
                                                   OwnedResources &resources,
                                                   const VirtualSocket &vsocket)
{
  PointerRNA rna = vsocket.rna();
  float3 value;
  RNA_float_get_array(&rna, "value", value);

  auto function = BLI::make_unique<BKE::MultiFunction_ConstantValue<float3>>(value);
  auto &node = builder.add_function(*function, {}, {0});

  resources.add(std::move(function), "vector socket");
  return *node.outputs()[0];
}

static MFBuilderOutputSocket &INSERT_float_socket(VTreeMFNetworkBuilder &builder,
                                                  OwnedResources &resources,
                                                  const VirtualSocket &vsocket)
{
  PointerRNA rna = vsocket.rna();
  float value = RNA_float_get(&rna, "value");

  auto function = BLI::make_unique<BKE::MultiFunction_ConstantValue<float>>(value);
  auto &node = builder.add_function(*function, {}, {0});

  resources.add(std::move(function), "float socket");
  return *node.outputs()[0];
}

static MFBuilderOutputSocket &INSERT_int_socket(VTreeMFNetworkBuilder &builder,
                                                OwnedResources &resources,
                                                const VirtualSocket &vsocket)
{
  PointerRNA rna = vsocket.rna();
  int value = RNA_int_get(&rna, "value");

  auto function = BLI::make_unique<BKE::MultiFunction_ConstantValue<int>>(value);
  auto &node = builder.add_function(*function, {}, {0});

  resources.add(std::move(function), "int socket");
  return *node.outputs()[0];
}

template<typename T>
static MFBuilderOutputSocket &INSERT_empty_list_socket(VTreeMFNetworkBuilder &builder,
                                                       OwnedResources &resources,
                                                       const VirtualSocket &UNUSED(vsocket))
{
  auto function = BLI::make_unique<BKE::MultiFunction_EmptyList<T>>();
  auto &node = builder.add_function(*function, {}, {0});

  resources.add(std::move(function), "empty list socket");
  return *node.outputs()[0];
}

static StringMap<InsertUnlinkedInputFunction> get_unlinked_input_inserter()
{
  StringMap<InsertUnlinkedInputFunction> inserters;
  inserters.add_new("fn_VectorSocket", INSERT_vector_socket);
  inserters.add_new("fn_FloatSocket", INSERT_float_socket);
  inserters.add_new("fn_IntegerSocket", INSERT_int_socket);
  inserters.add_new("fn_VectorListSocket", INSERT_empty_list_socket<float3>);
  inserters.add_new("fn_FloatListSocket", INSERT_empty_list_socket<float>);
  inserters.add_new("fn_IntegerListSocket", INSERT_empty_list_socket<int32_t>);
  return inserters;
}

template<typename FromT, typename ToT>
static std::pair<MFBuilderInputSocket *, MFBuilderOutputSocket *> INSERT_convert(
    VTreeMFNetworkBuilder &builder, OwnedResources &resources)
{
  auto function = BLI::make_unique<BKE::MultiFunction_Convert<FromT, ToT>>();
  auto &node = builder.add_function(*function, {0}, {1});
  resources.add(std::move(function), "converter function");
  return {node.inputs()[0], node.outputs()[0]};
}

static Map<std::pair<std::string, std::string>, InsertImplicitConversionFunction>
get_conversion_inserters()
{
  Map<std::pair<std::string, std::string>, InsertImplicitConversionFunction> inserters;
  inserters.add_new({"fn_IntegerSocket", "fn_FloatSocket"}, INSERT_convert<int, float>);
  return inserters;
}

static bool insert_nodes(VTreeMFNetworkBuilder &builder, OwnedResources &resources)
{
  const VirtualNodeTree &vtree = builder.vtree();
  auto inserters = get_node_inserters();

  for (const VirtualNode *vnode : vtree.nodes()) {
    StringRef idname = vnode->idname();
    InsertVNodeFunction *inserter = inserters.lookup_ptr(idname);

    if (inserter != nullptr) {
      (*inserter)(builder, resources, *vnode);
      BLI_assert(builder.data_sockets_of_vnode_are_mapped(*vnode));
    }
    else if (builder.has_data_sockets(*vnode)) {
      builder.add_placeholder(*vnode);
    }
  }

  return true;
}

static bool insert_links(VTreeMFNetworkBuilder &builder, OwnedResources &resources)
{
  auto conversion_inserters = get_conversion_inserters();

  for (const VirtualSocket *to_vsocket : builder.vtree().inputs_with_links()) {
    if (to_vsocket->links().size() > 1) {
      continue;
    }
    BLI_assert(to_vsocket->links().size() == 1);

    if (!builder.is_data_socket(*to_vsocket)) {
      continue;
    }

    const VirtualSocket *from_vsocket = to_vsocket->links()[0];
    if (!builder.is_data_socket(*from_vsocket)) {
      return false;
    }

    auto &from_socket = builder.lookup_output_socket(*from_vsocket);
    auto &to_socket = builder.lookup_input_socket(*to_vsocket);

    if (from_socket.type() == to_socket.type()) {
      builder.add_link(from_socket, to_socket);
    }
    else {
      InsertImplicitConversionFunction *inserter = conversion_inserters.lookup_ptr(
          {from_vsocket->idname(), to_vsocket->idname()});
      if (inserter == nullptr) {
        return false;
      }
      auto new_sockets = (*inserter)(builder, resources);
      builder.add_link(from_socket, *new_sockets.first);
      builder.add_link(*new_sockets.second, to_socket);
    }
  }

  return true;
}

static bool insert_unlinked_inputs(VTreeMFNetworkBuilder &builder, OwnedResources &resources)
{
  Vector<const VirtualSocket *> unlinked_data_inputs;
  for (const VirtualNode *vnode : builder.vtree().nodes()) {
    for (const VirtualSocket *vsocket : vnode->inputs()) {
      if (builder.is_data_socket(*vsocket)) {
        if (!builder.is_input_linked(*vsocket)) {
          unlinked_data_inputs.append(vsocket);
        }
      }
    }
  }

  auto inserters = get_unlinked_input_inserter();

  for (const VirtualSocket *vsocket : unlinked_data_inputs) {
    InsertUnlinkedInputFunction *inserter = inserters.lookup_ptr(vsocket->idname());

    if (inserter == nullptr) {
      return false;
    }
    MFBuilderOutputSocket &from_socket = (*inserter)(builder, resources, *vsocket);
    MFBuilderInputSocket &to_socket = builder.lookup_input_socket(*vsocket);
    builder.add_link(from_socket, to_socket);
  }

  return true;
}

class MultiFunction_FunctionTree : public BKE::MultiFunction {
 private:
  Vector<const MFOutputSocket *> m_inputs;
  Vector<const MFInputSocket *> m_outputs;

 public:
  MultiFunction_FunctionTree(Vector<const MFOutputSocket *> inputs,
                             Vector<const MFInputSocket *> outputs)
      : m_inputs(std::move(inputs)), m_outputs(std::move(outputs))
  {
    MFSignatureBuilder signature;
    for (auto socket : m_inputs) {
      BLI_assert(socket->node().is_placeholder());

      MFDataType type = socket->type();
      switch (type.category()) {
        case MFDataType::Single:
          signature.readonly_single_input("Input", type.type());
          break;
        case MFDataType::Vector:
          signature.readonly_vector_input("Input", type.base_type());
          break;
        case MFDataType::None:
          BLI_assert(false);
          break;
      }
    }
    for (auto socket : m_outputs) {
      BLI_assert(socket->node().is_placeholder());

      MFDataType type = socket->type();
      switch (type.category()) {
        case MFDataType::Single:
          signature.single_output("Output", type.type());
          break;
        case MFDataType::Vector:
          signature.vector_output("Output", type.base_type());
          break;
        case MFDataType::None:
          BLI_assert(false);
          break;
      }
    }
    this->set_signature(signature);
  }

  class Storage {
   private:
    Vector<GenericVectorArray *> m_vector_arrays;
    Vector<GenericMutableArrayRef> m_arrays;
    Map<uint, GenericVectorArray *> m_vector_per_socket;
    Map<uint, GenericVirtualListRef> m_virtual_list_for_inputs;
    Map<uint, GenericVirtualListListRef> m_virtual_list_list_for_inputs;

   public:
    Storage() = default;

    ~Storage()
    {
      for (GenericVectorArray *vector_array : m_vector_arrays) {
        delete vector_array;
      }
      for (GenericMutableArrayRef array : m_arrays) {
        MEM_freeN(array.buffer());
      }
    }

    void take_array_ref_ownership(GenericMutableArrayRef array)
    {
      m_arrays.append(array);
    }

    void take_vector_array_ownership(GenericVectorArray *vector_array)
    {
      m_vector_arrays.append(vector_array);
    }

    void take_vector_array_ownership__not_twice(GenericVectorArray *vector_array)
    {
      if (!m_vector_arrays.contains(vector_array)) {
        m_vector_arrays.append(vector_array);
      }
    }

    void set_virtual_list_for_input__non_owning(const MFInputSocket &socket,
                                                GenericVirtualListRef list)
    {
      m_virtual_list_for_inputs.add_new(socket.id(), list);
    }

    void set_virtual_list_list_for_input__non_owning(const MFInputSocket &socket,
                                                     GenericVirtualListListRef list)
    {
      m_virtual_list_list_for_inputs.add_new(socket.id(), list);
    }

    void set_vector_array_for_input__non_owning(const MFInputSocket &socket,
                                                GenericVectorArray *vector_array)
    {
      m_vector_per_socket.add_new(socket.id(), vector_array);
    }

    GenericVirtualListRef get_virtual_list_for_input(const MFInputSocket &socket) const
    {
      return m_virtual_list_for_inputs.lookup(socket.id());
    }

    GenericVirtualListListRef get_virtual_list_list_for_input(const MFInputSocket &socket) const
    {
      return m_virtual_list_list_for_inputs.lookup(socket.id());
    }

    GenericVectorArray &get_vector_array_for_input(const MFInputSocket &socket) const
    {
      return *m_vector_per_socket.lookup(socket.id());
    }

    bool input_is_computed(const MFInputSocket &socket) const
    {
      switch (socket.type().category()) {
        case MFDataType::Single:
          return m_virtual_list_for_inputs.contains(socket.id());
        case MFDataType::Vector:
          return m_virtual_list_list_for_inputs.contains(socket.id()) ||
                 m_vector_per_socket.contains(socket.id());
        case MFDataType::None:
          break;
      }
      BLI_assert(false);
      return false;
    }
  };

  void call(ArrayRef<uint> mask_indices, MFParams &params, MFContext &context) const override
  {
    if (mask_indices.size() == 0) {
      return;
    }

    Storage storage;
    this->copy_inputs_to_storage(params, storage);
    this->evaluate_network_to_compute_outputs(mask_indices, context, storage);
    this->copy_computed_values_to_outputs(mask_indices, params, storage);
  }

 private:
  BLI_NOINLINE void copy_inputs_to_storage(MFParams &params, Storage &storage) const
  {
    for (uint i = 0; i < m_inputs.size(); i++) {
      const MFOutputSocket &socket = *m_inputs[i];
      switch (socket.type().category()) {
        case MFDataType::Single: {
          GenericVirtualListRef input_list = params.readonly_single_input(i, "Input");
          for (const MFInputSocket *target : socket.targets()) {
            storage.set_virtual_list_for_input__non_owning(*target, input_list);
          }
          break;
        }
        case MFDataType::Vector: {
          GenericVirtualListListRef input_list_list = params.readonly_vector_input(i, "Input");
          for (const MFInputSocket *target : socket.targets()) {
            const MFNode &target_node = target->node();
            if (target_node.is_function()) {
              const MFFunctionNode &target_function_node = target_node.as_function();
              uint param_index = target_function_node.input_param_indices()[target->index()];
              MFParamType param_type =
                  target_function_node.function().signature().param_types()[param_index];

              if (param_type.is_readonly_vector_input()) {
                storage.set_virtual_list_list_for_input__non_owning(*target, input_list_list);
              }
              else if (param_type.is_mutable_vector()) {
                GenericVectorArray *vector_array = new GenericVectorArray(param_type.base_type(),
                                                                          input_list_list.size());
                for (uint i = 0; i < input_list_list.size(); i++) {
                  vector_array->extend_single__copy(i, input_list_list[i]);
                }
                storage.set_vector_array_for_input__non_owning(*target, vector_array);
                storage.take_vector_array_ownership(vector_array);
              }
              else {
                BLI_assert(false);
              }
            }
            else {
              storage.set_virtual_list_list_for_input__non_owning(*target, input_list_list);
            }
          }
          break;
        }
        case MFDataType::None: {
          BLI_assert(false);
          break;
        }
      }
    }
  }

  BLI_NOINLINE void evaluate_network_to_compute_outputs(ArrayRef<uint> mask_indices,
                                                        MFContext &global_context,
                                                        Storage &storage) const
  {
    Stack<const MFSocket *> sockets_to_compute;

    for (const MFInputSocket *input_socket : m_outputs) {
      sockets_to_compute.push(input_socket);
    }

    while (!sockets_to_compute.empty()) {
      for (const MFSocket *socket : sockets_to_compute) {
        std::cout << socket->id() << ", ";
      }
      std::cout << "\n";

      const MFSocket &socket = *sockets_to_compute.peek();

      if (socket.is_input()) {
        const MFInputSocket &input_socket = socket.as_input();
        if (storage.input_is_computed(input_socket)) {
          sockets_to_compute.pop();
        }
        else {
          const MFOutputSocket &origin = input_socket.origin();
          sockets_to_compute.push(&origin);
        }
      }
      else {
        const MFOutputSocket &output_socket = socket.as_output();
        const MFFunctionNode &function_node = output_socket.node().as_function();

        uint not_computed_inputs_amount = 0;
        for (const MFInputSocket *input_socket : function_node.inputs()) {
          if (!storage.input_is_computed(*input_socket)) {
            not_computed_inputs_amount++;
            sockets_to_compute.push(input_socket);
          }
        }

        bool all_inputs_are_computed = not_computed_inputs_amount == 0;
        if (all_inputs_are_computed) {
          this->compute_and_forward_outputs(mask_indices, global_context, function_node, storage);
          sockets_to_compute.pop();
        }
      }
    }
  }

  BLI_NOINLINE void compute_and_forward_outputs(ArrayRef<uint> mask_indices,
                                                MFContext &global_context,
                                                const MFFunctionNode &function_node,
                                                Storage &storage) const
  {
    uint array_size = mask_indices.last() + 1;

    MFParamsBuilder params_builder;
    params_builder.start_new(function_node.function().signature(), array_size);

    Vector<std::pair<const MFOutputSocket *, GenericMutableArrayRef>> single_outputs_to_forward;
    Vector<std::pair<const MFOutputSocket *, GenericVectorArray *>> vector_outputs_to_forward;

    ArrayRef<MFParamType> param_types = function_node.function().signature().param_types();

    for (uint param_index = 0; param_index < param_types.size(); param_index++) {
      MFParamType param_type = param_types[param_index];
      switch (param_type.category()) {
        case MFParamType::None: {
          BLI_assert(false);
          break;
        }
        case MFParamType::ReadonlySingleInput: {
          uint input_socket_index = function_node.input_param_indices().first_index(param_index);
          const MFInputSocket &input_socket = *function_node.inputs()[input_socket_index];
          GenericVirtualListRef values = storage.get_virtual_list_for_input(input_socket);
          params_builder.add_readonly_single_input(values);
          break;
        }
        case MFParamType::ReadonlyVectorInput: {
          uint input_socket_index = function_node.input_param_indices().first_index(param_index);
          const MFInputSocket &input_socket = *function_node.inputs()[input_socket_index];
          GenericVirtualListListRef values = storage.get_virtual_list_list_for_input(input_socket);
          params_builder.add_readonly_vector_input(values);
          break;
        }
        case MFParamType::SingleOutput: {
          uint output_socket_index = function_node.output_param_indices().first_index(param_index);
          const MFOutputSocket &output_socket = *function_node.outputs()[output_socket_index];
          GenericMutableArrayRef values_destination = this->allocate_array(
              output_socket.type().type(), array_size);
          params_builder.add_single_output(values_destination);
          single_outputs_to_forward.append({&output_socket, values_destination});
          break;
        }
        case MFParamType::VectorOutput: {
          uint output_socket_index = function_node.output_param_indices().first_index(param_index);
          const MFOutputSocket &output_socket = *function_node.outputs()[output_socket_index];
          auto *values_destination = new GenericVectorArray(output_socket.type().base_type(),
                                                            array_size);
          params_builder.add_vector_output(*values_destination);
          vector_outputs_to_forward.append({&output_socket, values_destination});
          break;
        }
        case MFParamType::MutableVector: {
          uint input_socket_index = function_node.input_param_indices().first_index(param_index);
          const MFInputSocket &input_socket = *function_node.inputs()[input_socket_index];

          uint output_socket_index = function_node.output_param_indices().first_index(param_index);
          const MFOutputSocket &output_socket = *function_node.outputs()[output_socket_index];

          GenericVectorArray &values = storage.get_vector_array_for_input(input_socket);
          params_builder.add_mutable_vector(values);
          vector_outputs_to_forward.append({&output_socket, &values});
          break;
        }
      }
    }

    MFParams &params = params_builder.build();
    const MultiFunction &function = function_node.function();
    function.call(mask_indices, params, global_context);

    for (auto single_forward_info : single_outputs_to_forward) {
      const MFOutputSocket &output_socket = *single_forward_info.first;
      GenericMutableArrayRef values = single_forward_info.second;
      storage.take_array_ref_ownership(values);

      for (const MFInputSocket *target : output_socket.targets()) {
        storage.set_virtual_list_for_input__non_owning(*target, values);
      }
    }

    for (auto vector_forward_info : vector_outputs_to_forward) {
      const MFOutputSocket &output_socket = *vector_forward_info.first;
      GenericVectorArray *values = vector_forward_info.second;
      storage.take_vector_array_ownership__not_twice(values);

      for (const MFInputSocket *target : output_socket.targets()) {
        const MFNode &target_node = target->node();
        if (target_node.is_function()) {
          const MFFunctionNode &target_function_node = target_node.as_function();
          uint param_index = target_function_node.input_param_indices()[target->index()];
          MFParamType param_type =
              target_function_node.function().signature().param_types()[param_index];

          if (param_type.is_readonly_vector_input()) {
            storage.set_virtual_list_list_for_input__non_owning(*target, *values);
          }
          else if (param_type.is_mutable_vector()) {
            GenericVectorArray *copied_values = new GenericVectorArray(values->type(),
                                                                       values->size());
            for (uint i = 0; i < values->size(); i++) {
              copied_values->extend_single__copy(i, (*values)[i]);
            }
            storage.take_vector_array_ownership(copied_values);
            storage.set_vector_array_for_input__non_owning(*target, copied_values);
          }
          else {
            BLI_assert(false);
          }
        }
        else if (m_outputs.contains(target)) {
          storage.set_vector_array_for_input__non_owning(*target, values);
        }
      }
    }
  }

  BLI_NOINLINE void copy_computed_values_to_outputs(ArrayRef<uint> mask_indices,
                                                    MFParams &params,
                                                    Storage &storage) const
  {
    for (uint output_index = 0; output_index < m_outputs.size(); output_index++) {
      uint global_param_index = m_inputs.size() + output_index;
      const MFInputSocket &socket = *m_outputs[output_index];
      switch (socket.type().category()) {
        case MFDataType::None: {
          BLI_assert(false);
          break;
        }
        case MFDataType::Single: {
          GenericVirtualListRef values = storage.get_virtual_list_for_input(socket);
          GenericMutableArrayRef output_values = params.single_output(global_param_index,
                                                                      "Output");
          for (uint i : mask_indices) {
            output_values.copy_in__uninitialized(i, values[i]);
          }
          break;
        }
        case MFDataType::Vector: {
          GenericVirtualListListRef values = storage.get_virtual_list_list_for_input(socket);
          GenericVectorArray &output_values = params.vector_output(global_param_index, "Output");
          for (uint i : mask_indices) {
            output_values.extend_single__copy(i, values[i]);
          }
          break;
        }
      }
    }
  }

  GenericMutableArrayRef allocate_array(const CPPType &type, uint size) const
  {
    void *buffer = MEM_malloc_arrayN(size, type.size(), __func__);
    return GenericMutableArrayRef(type, buffer, size);
  }
};

void MOD_functiondeform_do(FunctionDeformModifierData *fdmd, float (*vertexCos)[3], int numVerts)
{
  if (fdmd->function_tree == nullptr) {
    return;
  }

  bNodeTree *tree = (bNodeTree *)DEG_get_original_id((ID *)fdmd->function_tree);
  VirtualNodeTree vtree;
  vtree.add_all_of_tree(tree);
  vtree.freeze_and_index();

  const VirtualNode &input_vnode = *vtree.nodes_with_idname("fn_FunctionInputNode")[0];
  const VirtualNode &output_vnode = *vtree.nodes_with_idname("fn_FunctionOutputNode")[0];

  OwnedResources resources;
  VTreeMFNetworkBuilder builder(vtree);
  if (!insert_nodes(builder, resources)) {
    BLI_assert(false);
  }
  if (!insert_links(builder, resources)) {
    BLI_assert(false);
  }
  if (!insert_unlinked_inputs(builder, resources)) {
    BLI_assert(false);
  }

  auto vtree_network = builder.build();

  Vector<const MFOutputSocket *> function_inputs = {
      &vtree_network->lookup_socket(input_vnode.output(0)).as_output(),
      &vtree_network->lookup_socket(input_vnode.output(1)).as_output(),
      &vtree_network->lookup_socket(input_vnode.output(2)).as_output()};

  Vector<const MFInputSocket *> function_outputs = {
      &vtree_network->lookup_socket(output_vnode.input(0)).as_input()};

  MultiFunction_FunctionTree function{function_inputs, function_outputs};

  MFParamsBuilder params;
  params.start_new(function.signature(), numVerts);
  params.add_readonly_single_input(ArrayRef<float3>((float3 *)vertexCos, numVerts));
  params.add_readonly_single_input(&fdmd->control1);
  params.add_readonly_single_input(&fdmd->control2);

  TemporaryVector<float3> output_vectors(numVerts);
  params.add_single_output<float3>(output_vectors);

  MFContext context;
  function.call(IndexRange(numVerts).as_array_ref(), params.build(), context);

  memcpy(vertexCos, output_vectors.begin(), output_vectors.size() * sizeof(float3));
}
