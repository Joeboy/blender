#include <cmath>

#include "FN_functions.hpp"
#include "FN_types.hpp"
#include "FN_tuple_call.hpp"
#include "FN_llvm.hpp"

namespace FN {
namespace Functions {

class IterationStackFrame : public StackFrame {
 public:
  uint m_iteration;

  std::string to_string() const override
  {
    return "Iteration: " + std::to_string(m_iteration);
  }
};

class AutoVectorizationGen : public LLVMBuildIRBody {
 private:
  SharedFunction m_main;
  SmallVector<bool> m_input_is_list;
  SmallVector<SharedFunction> m_empty_list_value_builders;

  struct InputInfo {
    bool is_list;
    CPPTypeInfo *base_cpp_type;
    LLVMTypeInfo *base_llvm_type;
    LLVMTypeInfo *list_llvm_type;
    GetListLength get_length_fn;
    GetListDataPtr get_data_ptr_fn;
  };

  struct OutputInfo {
    CPPTypeInfo *base_cpp_type;
    LLVMTypeInfo *base_llvm_type;
    NewListWithAllocatedBuffer get_new_list_fn;
    GetListDataPtr get_data_ptr_fn;
  };

  SmallVector<InputInfo> m_input_info;
  SmallVector<OutputInfo> m_output_info;

 public:
  AutoVectorizationGen(SharedFunction main,
                       ArrayRef<bool> input_is_list,
                       ArrayRef<SharedFunction> empty_list_value_builders)
      : m_main(main),
        m_input_is_list(input_is_list.to_small_vector()),
        m_empty_list_value_builders(empty_list_value_builders.to_small_vector())
  {
    BLI_assert(input_is_list.contains(true));
    for (uint i = 0; i < main->input_amount(); i++) {
      SharedType &base_type = main->input_type(i);
      SharedType &list_type = get_list_type(base_type);
      InputInfo info;
      info.is_list = input_is_list[i];
      info.base_cpp_type = base_type->extension<CPPTypeInfo>();
      info.base_llvm_type = base_type->extension<LLVMTypeInfo>();
      info.list_llvm_type = list_type->extension<LLVMTypeInfo>();
      info.get_length_fn = GET_C_FN_list_length(base_type);
      info.get_data_ptr_fn = GET_C_FN_list_data_ptr(base_type);
      m_input_info.append(info);
    }
    for (auto &output : main->signature().outputs()) {
      SharedType &base_type = output.type();
      OutputInfo info;
      info.base_cpp_type = base_type->extension<CPPTypeInfo>();
      info.base_llvm_type = base_type->extension<LLVMTypeInfo>();
      info.get_new_list_fn = GET_C_FN_new_list_with_allocated_buffer(base_type);
      info.get_data_ptr_fn = GET_C_FN_list_data_ptr(base_type);
      m_output_info.append(info);
    }
  }

  void build_ir(CodeBuilder &builder,
                CodeInterface &interface,
                const BuildIRSettings &settings) const override
  {
    LLVMValues input_list_lengths = this->get_input_list_lengths(builder, interface);
    llvm::Value *max_length = builder.CreateSIntMax(input_list_lengths);

    LLVMValues input_data_pointers = this->get_input_data_pointers(builder, interface);
    LLVMValues output_data_pointers = this->create_output_lists(builder, interface, max_length);

    auto loop = builder.CreateNIterationsLoop(max_length);
    CodeBuilder &body_builder = loop.body_builder();
    llvm::Value *iteration = loop.current_iteration();

    LLVMValues main_inputs = this->prepare_main_function_inputs(
        body_builder, interface, settings, input_data_pointers, input_list_lengths, iteration);

    LLVMValues main_outputs(m_output_info.size());
    CodeInterface main_interface(
        main_inputs, main_outputs, interface.context_ptr(), interface.function_ir_cache());
    auto *body = m_main->body<LLVMBuildIRBody>();
    body->build_ir(body_builder, main_interface, settings);

    this->store_computed_values_in_output_lists(
        body_builder, main_outputs, output_data_pointers, iteration);

    loop.finalize(builder);
    this->free_input_lists(builder, interface);
  }

 private:
  LLVMValues get_input_list_lengths(CodeBuilder &builder, CodeInterface &interface) const
  {
    LLVMValues list_lengths;
    for (uint i = 0; i < m_input_info.size(); i++) {
      if (m_input_info[i].is_list) {
        auto *length = builder.CreateCallPointer((void *)m_input_info[i].get_length_fn,
                                                 {interface.get_input(i)},
                                                 builder.getInt32Ty(),
                                                 "Get list length");
        list_lengths.append(length);
      }
    }
    return list_lengths;
  }

  LLVMValues get_input_data_pointers(CodeBuilder &builder, CodeInterface &interface) const
  {
    LLVMValues data_pointers;
    for (uint i = 0; i < m_input_info.size(); i++) {
      if (m_input_info[i].is_list) {
        uint stride = m_input_info[i].base_cpp_type->size_of_type();
        llvm::Value *data_ptr = builder.CreateCallPointer((void *)m_input_info[i].get_data_ptr_fn,
                                                          {interface.get_input(i)},
                                                          builder.getVoidPtrTy(),
                                                          "Get list data pointer");
        llvm::Value *typed_data_ptr = builder.CastToPointerWithStride(data_ptr, stride);
        data_pointers.append(typed_data_ptr);
      }
    }
    return data_pointers;
  }

  LLVMValues create_output_lists(CodeBuilder &builder,
                                 CodeInterface &interface,
                                 llvm::Value *length) const
  {
    LLVMValues data_pointers;
    for (uint i = 0; i < m_output_info.size(); i++) {
      uint stride = m_output_info[i].base_cpp_type->size_of_type();

      llvm::Value *output_list = builder.CreateCallPointer(
          (void *)m_output_info[i].get_new_list_fn,
          {length},
          builder.getVoidPtrTy(),
          "Create new list with length");
      llvm::Value *data_ptr = builder.CreateCallPointer((void *)m_output_info[i].get_data_ptr_fn,
                                                        {output_list},
                                                        builder.getVoidPtrTy(),
                                                        "Get list data pointer");
      llvm::Value *typed_data_ptr = builder.CastToPointerWithStride(data_ptr, stride);
      data_pointers.append(typed_data_ptr);
      interface.set_output(i, output_list);
    }
    return data_pointers;
  }

  void store_computed_values_in_output_lists(CodeBuilder &builder,
                                             const LLVMValues &computed_values,
                                             const LLVMValues &output_data_pointers,
                                             llvm::Value *iteration) const
  {
    for (uint i = 0; i < m_output_info.size(); i++) {
      llvm::Value *store_address = builder.CreateGEP(output_data_pointers[i], iteration);
      m_output_info[i].base_llvm_type->build_store_ir__relocate(
          builder, computed_values[i], store_address);
    }
  }

  LLVMValues prepare_main_function_inputs(CodeBuilder &builder,
                                          CodeInterface &interface,
                                          const BuildIRSettings &settings,
                                          const LLVMValues &input_data_pointers,
                                          const LLVMValues &input_list_lengths,
                                          llvm::Value *iteration) const
  {
    LLVMValues main_inputs;

    uint list_input_index = 0;
    for (uint i = 0; i < m_input_info.size(); i++) {
      auto *type_info = m_input_info[i].base_llvm_type;
      if (m_input_is_list[i]) {
        llvm::Value *list_length = input_list_lengths[list_input_index];
        llvm::Value *list_is_empty = builder.CreateICmpEQ(list_length, builder.getInt32(0));

        auto ifthenelse = builder.CreateIfThenElse(list_is_empty, "List is Empty");
        CodeBuilder &then_builder = ifthenelse.then_builder();
        CodeBuilder &else_builder = ifthenelse.else_builder();

        /* Use default value when list has no elements. */
        SharedFunction &default_builder = m_empty_list_value_builders[list_input_index];
        auto *default_builder_body = default_builder->body<LLVMBuildIRBody>();
        LLVMValues default_builder_inputs(0);
        LLVMValues default_builder_outputs(1);
        CodeInterface default_builder_interface(default_builder_inputs,
                                                default_builder_outputs,
                                                interface.context_ptr(),
                                                interface.function_ir_cache());
        default_builder_body->build_ir(builder, default_builder_interface, settings);
        llvm::Value *default_value = default_builder_outputs[0];

        /* Load value from list. */
        llvm::Value *current_index = else_builder.CreateURem(iteration, list_length);
        llvm::Value *load_address = else_builder.CreateGEP(input_data_pointers[list_input_index],
                                                           current_index);
        llvm::Value *loaded_value_for_main = type_info->build_load_ir__copy(else_builder,
                                                                            load_address);

        ifthenelse.finalize(builder);

        auto *phi = builder.CreatePhi(type_info->get_type(builder.getContext()), 2);
        phi->addIncoming(default_value, then_builder.GetInsertBlock());
        phi->addIncoming(loaded_value_for_main, else_builder.GetInsertBlock());
        main_inputs.append(phi);
        list_input_index++;
      }
      else {
        llvm::Value *source_value = interface.get_input(i);
        llvm::Value *value_for_main = type_info->build_copy_ir(builder, source_value);
        main_inputs.append(value_for_main);
      }
    }
    return main_inputs;
  }

  void free_input_lists(CodeBuilder &builder, CodeInterface &interface) const
  {
    for (uint i = 0; i < m_input_info.size(); i++) {
      if (m_input_info[i].is_list) {
        llvm::Value *input_list = interface.get_input(i);
        m_input_info[i].list_llvm_type->build_free_ir(builder, input_list);
      }
    }
  }
};

class AutoVectorization : public TupleCallBody {
 private:
  SharedFunction m_main;
  TupleCallBody *m_main_body;

  const SmallVector<bool> m_input_is_list;
  SmallVector<uint> m_list_inputs;

  SmallVector<TupleCallBody *> m_get_length_bodies;
  uint m_max_len_in_size, m_max_len_out_size;

  SmallVector<TupleCallBody *> m_get_element_bodies;
  SmallVector<TupleCallBody *> m_create_empty_bodies;
  SmallVector<TupleCallBody *> m_append_bodies;

 public:
  AutoVectorization(SharedFunction main, const SmallVector<bool> &input_is_list)
      : m_main(main), m_main_body(main->body<TupleCallBody>()), m_input_is_list(input_is_list)
  {
    for (uint i = 0; i < input_is_list.size(); i++) {
      if (input_is_list[i]) {
        m_list_inputs.append(i);
      }
    }
    for (uint i : m_list_inputs) {
      SharedType &base_type = main->input_type(i);
      m_get_length_bodies.append(GET_FN_list_length(base_type)->body<TupleCallBody>());
      m_get_element_bodies.append(GET_FN_get_list_element(base_type)->body<TupleCallBody>());
    }

    m_max_len_in_size = 0;
    m_max_len_out_size = 0;
    for (TupleCallBody *body : m_get_length_bodies) {
      m_max_len_in_size = std::max(m_max_len_in_size, body->meta_in()->size_of_full_tuple());
      m_max_len_out_size = std::max(m_max_len_out_size, body->meta_out()->size_of_full_tuple());
    }

    for (auto output : main->signature().outputs()) {
      SharedType &base_type = output.type();
      m_create_empty_bodies.append(GET_FN_empty_list(base_type)->body<TupleCallBody>());
      m_append_bodies.append(GET_FN_append_to_list(base_type)->body<TupleCallBody>());
    }
  }

  void call(Tuple &fn_in, Tuple &fn_out, ExecutionContext &ctx) const override
  {
    uint *input_lengths = (uint *)BLI_array_alloca(input_lengths, m_list_inputs.size());
    this->get_input_list_lengths(fn_in, ctx, input_lengths);
    uint max_length = *std::max_element(input_lengths, input_lengths + m_list_inputs.size());

    this->initialize_empty_lists(fn_out, ctx);

    FN_TUPLE_CALL_ALLOC_TUPLES(m_main_body, main_in, main_out);

    IterationStackFrame iteration_frame;
    TextStackFrame function_name_frame(m_main->name().data());
    ctx.stack().push(&iteration_frame);
    ctx.stack().push(&function_name_frame);

    for (uint iteration = 0; iteration < max_length; iteration++) {
      uint list_index = 0;
      for (uint i = 0; i < m_input_is_list.size(); i++) {
        if (m_input_is_list[i]) {
          this->copy_in_iteration(
              iteration, fn_in, main_in, i, list_index, input_lengths[list_index], ctx);
          list_index++;
        }
        else {
          Tuple::copy_element(fn_in, i, main_in, i);
        }
      }

      iteration_frame.m_iteration = iteration;
      m_main_body->call(main_in, main_out, ctx);

      for (uint i = 0; i < m_main->output_amount(); i++) {
        this->append_to_output(main_out, fn_out, i, ctx);
      }
    }

    ctx.stack().pop();
    ctx.stack().pop();
  }

 private:
  void get_input_list_lengths(Tuple &fn_in, ExecutionContext &ctx, uint *r_lengths) const
  {
    for (uint i = 0; i < m_list_inputs.size(); i++) {
      uint index_in_tuple = m_list_inputs[i];
      r_lengths[i] = this->get_input_list_length(fn_in, index_in_tuple, i, ctx);
    }
  }

  uint get_input_list_length(Tuple &fn_in,
                             uint index_in_tuple,
                             uint list_index,
                             ExecutionContext &ctx) const
  {
    TupleCallBody *body = m_get_length_bodies[list_index];
    FN_TUPLE_CALL_ALLOC_TUPLES(body, get_length_in, get_length_out);

    Tuple::copy_element(fn_in, index_in_tuple, get_length_in, 0);
    body->call__setup_stack(get_length_in, get_length_out, ctx);
    uint length = get_length_out.get<uint>(0);
    return length;
  }

  void copy_in_iteration(uint iteration,
                         Tuple &fn_in,
                         Tuple &main_in,
                         uint index,
                         uint list_index,
                         uint list_length,
                         ExecutionContext &ctx) const
  {
    if (list_length == 0) {
      main_in.init_default(index);
      return;
    }
    uint load_index = iteration % list_length;

    TupleCallBody *body = m_get_element_bodies[list_index];
    FN_TUPLE_CALL_ALLOC_TUPLES(body, get_element_in, get_element_out);

    Tuple::copy_element(fn_in, index, get_element_in, 0);
    get_element_in.set<uint>(1, load_index);
    get_element_in.init_default(2);
    body->call__setup_stack(get_element_in, get_element_out, ctx);
    Tuple::relocate_element(get_element_out, 0, main_in, index);
  }

  void initialize_empty_lists(Tuple &fn_out, ExecutionContext &ctx) const
  {
    for (uint i = 0; i < m_main->output_amount(); i++) {
      this->initialize_empty_list(fn_out, i, ctx);
    }
  }

  void initialize_empty_list(Tuple &fn_out, uint index, ExecutionContext &ctx) const
  {
    TupleCallBody *body = m_create_empty_bodies[index];
    FN_TUPLE_CALL_ALLOC_TUPLES(body, create_list_in, create_list_out);

    body->call__setup_stack(create_list_in, create_list_out, ctx);
    Tuple::relocate_element(create_list_out, 0, fn_out, index);
  }

  void append_to_output(Tuple &main_out, Tuple &fn_out, uint index, ExecutionContext &ctx) const
  {
    TupleCallBody *body = m_append_bodies[index];
    FN_TUPLE_CALL_ALLOC_TUPLES(body, append_in, append_out);

    Tuple::relocate_element(fn_out, index, append_in, 0);
    Tuple::relocate_element(main_out, index, append_in, 1);
    body->call__setup_stack(append_in, append_out, ctx);
    Tuple::relocate_element(append_out, 0, fn_out, index);
  }
};

SharedFunction to_vectorized_function(SharedFunction &original_fn,
                                      ArrayRef<bool> vectorized_inputs_mask,
                                      ArrayRef<SharedFunction> empty_list_value_builders)
{
  uint input_amount = original_fn->input_amount();
  uint output_amount = original_fn->output_amount();

  BLI_assert(vectorized_inputs_mask.size() == input_amount);
  BLI_assert(vectorized_inputs_mask.contains(true));
  BLI_assert(empty_list_value_builders.size() == vectorized_inputs_mask.count(true));

  if (!original_fn->has_body<TupleCallBody>()) {
    if (original_fn->has_body<LLVMBuildIRBody>()) {
      derive_TupleCallBody_from_LLVMBuildIRBody(original_fn);
    }
    else {
      BLI_assert(false);
    }
  }

  InputParameters inputs;
  for (uint i = 0; i < input_amount; i++) {
    auto original_parameter = original_fn->signature().inputs()[i];
    if (vectorized_inputs_mask[i]) {
      SharedType &list_type = get_list_type(original_parameter.type());
      inputs.append(InputParameter(original_parameter.name() + " (List)", list_type));
    }
    else {
      inputs.append(original_parameter);
    }
  }

  OutputParameters outputs;
  for (uint i = 0; i < output_amount; i++) {
    auto original_parameter = original_fn->signature().outputs()[i];
    SharedType &list_type = get_list_type(original_parameter.type());
    outputs.append(OutputParameter(original_parameter.name() + " (List)", list_type));
  }

  std::string name = original_fn->name() + " (Vectorized)";
  auto fn = SharedFunction::New(name, Signature(inputs, outputs));
  // fn->add_body<AutoVectorization>(original_fn, vectorized_inputs_mask);
  fn->add_body<AutoVectorizationGen>(
      original_fn, vectorized_inputs_mask, empty_list_value_builders);
  return fn;
}

}  // namespace Functions
}  // namespace FN
