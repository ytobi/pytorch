#include <torch/csrc/jit/mobile/import.h>
#include <ATen/core/ivalue.h>
#include <caffe2/serialize/inline_container.h>
#include <torch/csrc/jit/api/compilation_unit.h>
#include <torch/csrc/jit/mobile/observer.h>
#include <torch/csrc/jit/mobile/type_parser.h>
#include <torch/csrc/jit/runtime/instruction.h>
#include <torch/csrc/jit/serialization/import_export_constants.h>
#include <torch/csrc/jit/serialization/unpickler.h>
#include <torch/custom_class.h>

#include <exception>
#include <fstream>
#include <string>
#include <vector>

// The import process to serialize the bytecode package.
// An example for bytecode.pkl of a small mobile_module looks like:
// (3,
//   ('__torch__.m.forward',
//     (('instructions',
//       (('STOREN', 1, 2),
//        ('DROPR', 1, 0),
//        ('MOVE', 2, 0),
//        ('OP', 0, 0),
//        ('RET', 0, 0))),
//      ('operators', (('aten::Int', 'Tensor'),)),
//      ('constants', ()),
//      ('types', ()),
//      ('register_size', 2))))

// Note that currently the backward compatibility is not supported by bytecode.
// This format and process need to be revisted and redesigned if we want to
// support backward compatibility in future.

namespace c10 {
// std::string serializeType(const Type &t);
TypePtr parseType(const std::string& pythonStr);
} // namespace c10

namespace torch {
namespace jit {
using caffe2::serialize::IStreamAdapter;
using caffe2::serialize::PyTorchStreamReader;
using caffe2::serialize::ReadAdapterInterface;

OpCode parseOpCode(const char* str);

IValue expect_field(
    IValue tup,
    const std::string& expected_name,
    size_t entry) {
  auto row = tup.toTuple()->elements().at(entry).toTuple();
  TORCH_INTERNAL_ASSERT(
      row->elements().at(0).toStringRef() == expected_name,
      "Expected ",
      expected_name,
      " found ",
      row->elements().at(0).toStringRef());
  return row->elements().at(1);
}

std::string operator_str(
    const std::string& name,
    const std::string& overloadname) {
  std::string result = name;
  if (!overloadname.empty()) {
    result += "." + overloadname;
  }
  return result;
}

namespace {
void print_unsupported_ops_and_throw(
    const std::unordered_set<std::string>& unsupported_ops) {
  std::string error_message("{");
  for (const auto& op_name : unsupported_ops) {
    error_message += op_name + ", ";
  }
  error_message += "}";
  TORCH_CHECK(false, "Following ops cannot be found:", error_message);
}

void parseMethods(
    const std::vector<IValue>& vals,
    mobile::CompilationUnit& mcu) {
  TORCH_CHECK(vals.size() > 0, "Bytecode has no elements. ");
  // Initialized with the version number when kProducedBytecodeVersion was
  // introduced. The old models (some of them already in production) without
  // version number don't have to be re-generated.
  int64_t model_version = 0x3L;
  size_t method_i_start = 0;
  if (vals[0].isInt()) {
    model_version = vals[0].toInt();
    method_i_start = 1;
  }
  TORCH_CHECK(
      model_version == caffe2::serialize::kProducedBytecodeVersion,
      "Lite Interpreter verson number does not match. ",
      "The code version is ",
      caffe2::serialize::kProducedBytecodeVersion,
      " but the model version is ",
      model_version);

  for (size_t i = method_i_start; i < vals.size(); ++i) {
    const auto& element = vals[i];
    const auto& m_tuple = element.toTuple()->elements();
    const std::string& function_name = m_tuple[0].toStringRef();
    IValue table = m_tuple[1];

    auto function = std::unique_ptr<mobile::Function>(
        new mobile::Function(c10::QualifiedName(function_name)));

    const auto& ins_list =
        expect_field(table, "instructions", BYTECODE_INDEX_INSTRUCTION)
            .toTuple()
            ->elements();
    const auto& ops_list =
        expect_field(table, "operators", BYTECODE_INDEX_OPERATOR)
            .toTuple()
            ->elements();
    const auto& consts_list =
        expect_field(table, "constants", BYTECODE_INDEX_CONSTANT)
            .toTuple()
            ->elements();
    const auto& types_list =
        expect_field(table, "types", BYTECODE_INDEX_TYPE).toTuple()->elements();
    const auto& register_size = expect_field(table, "register_size", 4).toInt();

    for (const auto& ins : ins_list) {
      auto ins_item = ins.toTuple()->elements();
      TORCH_CHECK(
          ins_item.size() == 3,
          "There should be three parts in an instruction. The function name is ",
          function_name);
      OpCode op_code = parseOpCode(ins_item[0].toString()->string().c_str());
      int X = ins_item[1].toInt();
      int N = ins_item[2].toInt();
      function->append_instruction(op_code, X, N);
    }

    std::unordered_set<std::string> unsupported_op_names;
    for (const auto& op : ops_list) {
      auto op_item = op.toTuple()->elements();
      TORCH_CHECK(
          op_item.size() == 2,
          "There should be two parts in an operator name.");
      auto op_found = function->append_operator(
          op_item[0].toString()->string(), op_item[1].toString()->string());
      if (!op_found) {
        unsupported_op_names.emplace(operator_str(
            op_item[0].toString()->string(), op_item[1].toString()->string()));
      }
    }
    if (!unsupported_op_names.empty()) {
      print_unsupported_ops_and_throw(unsupported_op_names);
    };

    for (const auto& constant : consts_list) {
      function->append_constant(constant);
    }

    for (const auto& t : types_list) {
      function->append_type(c10::parseType(t.toStringRef()));
    }

    function->set_register_size(register_size);

    mcu.register_function(std::move(function));
  }
}

// The deserializer class which loads the bytecode package from bc files.
class BytecodeDeserializer final {
 public:
  explicit BytecodeDeserializer(std::unique_ptr<PyTorchStreamReader> reader);
  mobile::Module deserialize(c10::optional<at::Device> device);

 private:
  c10::IValue readArchive(
      const std::string& archive_name,
      std::shared_ptr<mobile::CompilationUnit> mcu);
  std::shared_ptr<CompilationUnit> compilation_unit_;
  std::unordered_set<std::string> imported_libs_;
  std::unique_ptr<PyTorchStreamReader> reader_;
  c10::optional<at::Device> device_;
};

BytecodeDeserializer::BytecodeDeserializer(
    std::unique_ptr<PyTorchStreamReader> reader)
    : compilation_unit_(std::make_shared<CompilationUnit>()),
      reader_(std::move(reader)) {}

mobile::Module BytecodeDeserializer::deserialize(
    c10::optional<at::Device> device) {
  device_ = device;
  auto mcu = std::make_shared<mobile::CompilationUnit>();
  auto bvals = readArchive("bytecode", mcu).toTuple()->elements();
  parseMethods(bvals, *mcu);

  return mobile::Module(readArchive("data", mcu).toObject(), mcu);
}

c10::IValue BytecodeDeserializer::readArchive(
    const std::string& archive_name,
    std::shared_ptr<mobile::CompilationUnit> mcu) {
  std::stringstream picklename;
  picklename << archive_name << ".pkl";
  at::DataPtr pickle_ptr;
  size_t pickle_size;
  std::tie(pickle_ptr, pickle_size) = reader_->getRecord(picklename.str());

  size_t bytes_read = 0;
  auto data = reinterpret_cast<const char*>(pickle_ptr.get());
  auto reader = [&](char* buffer, size_t len) -> size_t {
    if (bytes_read >= pickle_size) {
      return 0;
    }
    len = std::min(pickle_size - bytes_read, len);
    // Copy len bytes into buffer
    const char* start = data + bytes_read;
    std::memcpy(buffer, start, len);
    bytes_read += len;
    return len;
  };

  static const c10::QualifiedName torchPrefix = "__torch__";
  auto type_resolver = [&](const c10::QualifiedName& qn) {
    TypePtr type;
    // HACK: first we check whether the name starts with `__torch__` to tell if
    // it's "supposed" to be a class type. This is a reliable check today, but
    // there is no guarantee that this is the case. The real solution is to
    // merge type parsers so we can share class resolution logic.
    if (torchPrefix.isPrefixOf(qn)) {
      if (compilation_unit_->get_class(qn) == nullptr) {
        auto typeptr = ClassType::create(qn, compilation_unit_, true);
        compilation_unit_->register_type(typeptr);
      }
      type = compilation_unit_->get_class(qn);
    } else {
      type = c10::parseType(qn.qualifiedName());
    }
    return c10::StrongTypePtr(compilation_unit_, type);
  };

  auto obj_loader = [&](at::StrongTypePtr type, IValue input) {
    auto cls = type.type_->expect<at::ClassType>();
    auto qn = cls->name();
    c10::QualifiedName method_name(qn.value(), "__setstate__");
    auto setstate = mcu->find_function(method_name);
    auto find_custom_class_with_setstate = [&qn]() -> c10::ClassTypePtr {
      auto custom_class_type = torch::jit::getCustomClass(qn->qualifiedName());
      if (custom_class_type && custom_class_type->findMethod("__setstate__")) {
        return custom_class_type;
      }
      return nullptr;
    };
    if (setstate) {
      auto obj = c10::ivalue::Object::create(type, 0);
      Stack stack({obj, input});
      setstate->run(stack);
      return obj;
    } else if (auto custom_class_type = find_custom_class_with_setstate()) {
      auto obj = c10::ivalue::Object::create(
          c10::StrongTypePtr(nullptr, custom_class_type), 1);
      Stack stack({obj, input});
      custom_class_type->getMethod("__setstate__").run(stack);
      return obj;
    } else {
      auto dict = std::move(input).toGenericDict();
      size_t ndict = dict.size();
      auto obj = c10::ivalue::Object::create(type, ndict);
      auto it = dict.begin();
      for (size_t i = 0; i < ndict; ++i) {
        std::stringstream name;
        name << it->key();
        cls->addOrCheckAttribute(name.str(), it->key().type());
        obj->setSlot(i, it->value());
        ++it;
      }
      return obj;
    }
  };

  auto read_record = [&](const std::string& name) {
    std::stringstream ss;
    ss << archive_name << "/" << name;
    return std::get<0>(reader_->getRecord(ss.str()));
  };

  Unpickler unpickler(
      reader,
      std::move(type_resolver),
      std::move(obj_loader),
      std::move(read_record),
      device_);
  return unpickler.parse_ivalue();
}

} // namespace

mobile::Module _load_for_mobile(
    std::istream& in,
    c10::optional<at::Device> device) {
  std::unique_ptr<IStreamAdapter> rai = std::make_unique<IStreamAdapter>(&in);
  auto module = _load_for_mobile(std::move(rai), device);
  return module;
}

mobile::Module _load_for_mobile(
    const std::string& filename,
    c10::optional<at::Device> device) {
  std::unique_ptr<FileAdapter> rai = std::make_unique<FileAdapter>(filename);
  auto module = _load_for_mobile(std::move(rai), device);
  return module;
}

mobile::Module _load_for_mobile(
    std::unique_ptr<ReadAdapterInterface> rai,
    c10::optional<c10::Device> device) {
  auto observer = torch::observerConfig().getModuleObserver();
  if (observer) {
    observer->onEnterLoadModel();
  }
  try {
    auto reader = torch::make_unique<PyTorchStreamReader>(std::move(rai));
    BytecodeDeserializer deserializer(std::move(reader));
    mobile::Module result = deserializer.deserialize(std::move(device));
    std::string name = result.name();
    if (observer) {
      observer->onExitLoadModel(name);
    }
    return result;
  } catch (const std::exception& ex) {
    if (observer) {
      observer->onFailLoadModel(
          "Error occured during loading model: " + (std::string)ex.what());
    }
    TORCH_CHECK(false, ex.what());
  } catch (...) {
    if (observer) {
      observer->onFailLoadModel("unknown exception");
    }
    TORCH_CHECK(false, "unknown exception");
  }
}

} // namespace jit
} // namespace torch
