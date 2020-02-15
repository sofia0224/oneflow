#include "oneflow/core/common/dss.h"
#include "oneflow/core/common/util.h"

namespace oneflow {

namespace {

struct Foo {
  BEGIN_DSS(DSS_GET_FIELD_COUNTER(), Foo, 0);
  int x;
  int y;
  int* z;

  DSS_DEFINE_FIELD(DSS_GET_FIELD_COUNTER(), "demo dss", x);
  DSS_DEFINE_FIELD(DSS_GET_FIELD_COUNTER(), "demo dss", y);
  DSS_DEFINE_FIELD(DSS_GET_FIELD_COUNTER(), "demo dss", z);

  END_DSS(DSS_GET_FIELD_COUNTER(), "demo dss", Foo);
};

struct Bar {
  BEGIN_DSS(DSS_GET_FIELD_COUNTER(), Foo, 0);

  END_DSS(DSS_GET_FIELD_COUNTER(), "demo dss", Bar);
};

template<typename T>
struct IsPointer {
  static const bool value = std::is_pointer<T>::value;
};

template<typename T>
struct RemovePointer {
  using type = typename std::remove_pointer<T>::type;
};

template<typename T>
struct IsScalar {
  static const bool value =
      std::is_arithmetic<T>::value || std::is_enum<T>::value || std::is_same<T, std::string>::value;
};

template<int field_counter, typename WalkCtxType, typename FieldType>
struct DumpFieldName {
  static void Call(WalkCtxType* ctx, FieldType* field, const char* field_name) {
    ctx->push_back(field_name);
  }
};

TEST(DSS, walk_field) {
  Foo foo;
  std::vector<std::string> field_names;
  foo.__WalkVerboseField__<DumpFieldName>(&field_names);
  ASSERT_EQ(field_names.size(), 3);
  ASSERT_TRUE(field_names[0] == "x");
  ASSERT_TRUE(field_names[1] == "y");
  ASSERT_TRUE(field_names[2] == "z");
}

template<bool is_pointer>
struct PushBackPtrFieldName {
  template<typename WalkCtxType>
  static void Call(WalkCtxType* ctx, const char* field_name) {}
};

template<>
struct PushBackPtrFieldName<true> {
  template<typename WalkCtxType>
  static void Call(WalkCtxType* ctx, const char* field_name) {
    ctx->push_back(field_name);
  }
};

template<int field_counter, typename WalkCtxType, typename FieldType>
struct FilterPointerFieldName {
  static void Call(WalkCtxType* ctx, FieldType* field, const char* field_name) {
    PushBackPtrFieldName<std::is_pointer<FieldType>::value>::Call(ctx, field_name);
  }
};

template<int field_counter, typename WalkCtxType, typename FieldType>
struct FilterPointerFieldNameUntil {
  static bool Call(WalkCtxType* ctx, FieldType* field) {
    return true;
    PushBackPtrFieldName<std::is_pointer<FieldType>::value>::Call(ctx, "");
  }
};

TEST(DSS, filter_field) {
  Foo foo;
  std::vector<std::string> field_names;
  foo.__WalkVerboseField__<FilterPointerFieldName>(&field_names);
  ASSERT_EQ(field_names.size(), 1);
  ASSERT_TRUE(field_names[0] == "z");
}

TEST(DSS, filter_field_until) {
  Foo foo;
  std::vector<std::string> field_names;
  ASSERT_TRUE(foo.__WalkFieldUntil__<FilterPointerFieldNameUntil>(&field_names));
  ASSERT_TRUE(field_names.empty());
}

#define DSS_DEFINE_TEST_UNION_FIELD(field_counter)                   \
  DSS_DEFINE_FIELD(field_counter, "demo dss", union_field);          \
  DSS_DEFINE_UNION_FIELD_VISITOR(field_counter, union_case,          \
                                 OF_PP_MAKE_TUPLE_SEQ(int32_t, x, 1) \
                                     OF_PP_MAKE_TUPLE_SEQ(int64_t, y, 2));

struct TestDssUnion {
  BEGIN_DSS(DSS_GET_FIELD_COUNTER(), TestDssUnion, 0);

 public:
  struct {
    int32_t union_case;
    union {
      int32_t x;
      int64_t y;
    };
  } union_field;

  DSS_DEFINE_TEST_UNION_FIELD(DSS_GET_FIELD_COUNTER());
  END_DSS(DSS_GET_FIELD_COUNTER(), "demo dss", TestDssUnion);
};

template<int field_counter, typename WalkCtxType, typename FieldType>
struct StaticDumpFieldName {
  static void Call(WalkCtxType* ctx, const char* field_name, const char* oneof_name) {
    ctx->push_back(field_name);
    ctx->push_back(oneof_name);
  }
};

TEST(DSS, union_field) {
  TestDssUnion foo;
  foo.union_field.union_case = 0;
  {
    std::vector<std::string> field_names;
    foo.__WalkVerboseField__<DumpFieldName>(&field_names);
    ASSERT_EQ(field_names.size(), 0);
  }
  foo.union_field.union_case = 1;
  {
    std::vector<std::string> field_names;
    foo.__WalkVerboseField__<DumpFieldName>(&field_names);
    ASSERT_EQ(field_names.size(), 1);
    ASSERT_EQ(field_names.at(0), "x");
  }
  foo.union_field.union_case = 2;
  {
    std::vector<std::string> field_names;
    foo.__WalkVerboseField__<DumpFieldName>(&field_names);
    ASSERT_EQ(field_names.size(), 1);
    ASSERT_EQ(field_names.at(0), "y");
  }
}

TEST(DSS, static_verbose_field) {
  std::vector<std::string> field_names;
  TestDssUnion::__WalkStaticVerboseField__<StaticDumpFieldName>(&field_names);
  ASSERT_EQ(field_names.size(), 4);
  ASSERT_EQ(field_names.at(0), "x");
  ASSERT_EQ(field_names.at(1), "union_field");
  ASSERT_EQ(field_names.at(2), "y");
  ASSERT_EQ(field_names.at(3), "union_field");
}

}  // namespace

}  // namespace oneflow
