// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_TEST_CCTEST_TEST_SWISS_NAME_DICTIONARY_INFRA_H_
#define V8_TEST_CCTEST_TEST_SWISS_NAME_DICTIONARY_INFRA_H_

#include <memory>
#include <utility>

#include "src/codegen/code-stub-assembler.h"
#include "src/init/v8.h"
#include "src/objects/objects-inl.h"
#include "src/objects/swiss-name-dictionary-inl.h"
#include "test/cctest/cctest.h"
#include "test/cctest/compiler/code-assembler-tester.h"
#include "test/cctest/compiler/function-tester.h"

namespace v8 {
namespace internal {
namespace test_swiss_hash_table {

using Value = std::string;
using ValueOpt = base::Optional<Value>;
using PropertyDetailsOpt = base::Optional<PropertyDetails>;
using IndexOpt = base::Optional<InternalIndex>;

static const ValueOpt kNoValue;
static const PropertyDetailsOpt kNoDetails;
static const base::Optional<int> kNoInt;
static const IndexOpt kNoIndex;

static const std::vector<int> interesting_initial_capacities = {
    4,
    8,
    16,
    128,
    1 << (sizeof(uint16_t) * 8),
    1 << (sizeof(uint16_t) * 8 + 1)};

extern const std::vector<PropertyDetails> distinct_property_details;

// Wrapping this in a struct makes the tests a bit more readable.
struct FakeH1 {
  uint32_t value;

  explicit FakeH1(int value) : value{static_cast<uint32_t>(value)} {}

  bool operator==(const FakeH1& other) const { return value == other.value; }
};

// Wrapping this in a struct makes the tests a bit more readable.
struct FakeH2 {
  uint8_t value;

  bool operator==(const FakeH2& other) const { return value == other.value; }
};

using FakeH1Opt = base::Optional<FakeH1>;
using FakeH2Opt = base::Optional<FakeH2>;

// Representation of keys used when writing test cases.
struct Key {
  std::string str;

  // If present, contains the value we faked the key's H1 hash with.
  FakeH1Opt h1_override = FakeH1Opt();

  // If present, contains the value we faked the key's H2 hash with.
  FakeH2Opt h2_override = FakeH2Opt();
};

// Internal representation of keys. See |create_key_with_hash| for details.
struct CachedKey {
  Handle<Symbol> key_symbol;

  // If present, contains the value we faked the key's H1 hash with.
  FakeH1Opt h1_override;

  // If present, contains the value we faked the key's H2 hash with.
  FakeH2Opt h2_override;
};

using KeyCache = std::unordered_map<std::string, CachedKey>;

Handle<Name> CreateKeyWithHash(Isolate* isolate, KeyCache& keys,
                               const Key& key);

class RuntimeTestRunner;
class CSATestRunner;

// Abstraction over executing a sequence of operations on a single hash table.
// Actually performing those operations is done by the TestRunner.
template <typename TestRunner>
class TestSequence {
 public:
  explicit TestSequence(Isolate* isolate, int initial_capacity)
      : isolate{isolate},
        initial_capacity{initial_capacity},
        keys_{},
        runner_{isolate, initial_capacity, keys_} {}

  // Determines whether or not to run VerifyHeap after each operation. Can make
  // debugging easier.
  static constexpr bool kVerifyAfterEachStep = false;

  void Add(Handle<Name> key, Handle<Object> value, PropertyDetails details) {
    runner_.Add(key, value, details);

    if (kVerifyAfterEachStep) {
      runner_.VerifyHeap();
    }
  }

  void Add(const Key& key, ValueOpt value = kNoValue,
           PropertyDetailsOpt details = kNoDetails) {
    if (!value) {
      value = "dummy_value";
    }

    if (!details) {
      details = PropertyDetails::Empty();
    }

    Handle<Name> key_handle = CreateKeyWithHash(isolate, keys_, key);
    Handle<Object> value_handle = isolate->factory()->NewStringFromAsciiChecked(
        value.value().c_str(), AllocationType::kYoung);

    Add(key_handle, value_handle, details.value());
  }

  void UpdateByKey(Handle<Name> key, Handle<Object> new_value,
                   PropertyDetails new_details) {
    InternalIndex entry = runner_.FindEntry(key);
    CHECK(entry.is_found());
    runner_.Put(entry, new_value, new_details);

    if (kVerifyAfterEachStep) {
      runner_.VerifyHeap();
    }
  }

  void UpdateByKey(const Key& existing_key, Value new_value,
                   PropertyDetails new_details) {
    Handle<Name> key_handle = CreateKeyWithHash(isolate, keys_, existing_key);
    Handle<Object> value_handle = isolate->factory()->NewStringFromAsciiChecked(
        new_value.c_str(), AllocationType::kYoung);

    UpdateByKey(key_handle, value_handle, new_details);
  }

  void DeleteByKey(Handle<Name> key) {
    InternalIndex entry = runner_.FindEntry(key);
    CHECK(entry.is_found());
    runner_.Delete(entry);

    if (kVerifyAfterEachStep) {
      runner_.VerifyHeap();
    }
  }

  void DeleteByKey(const Key& existing_key) {
    Handle<Name> key_handle = CreateKeyWithHash(isolate, keys_, existing_key);

    DeleteByKey(key_handle);
  }

  void CheckDataAtKey(Handle<Name> key, IndexOpt expected_index_opt,
                      base::Optional<Handle<Object>> expected_value_opt,
                      PropertyDetailsOpt expected_details_opt) {
    InternalIndex actual_index = runner_.FindEntry(key);

    if (expected_index_opt) {
      CHECK_EQ(expected_index_opt.value(), actual_index);
    }

    if (actual_index.is_found()) {
      Handle<FixedArray> data = runner_.GetData(actual_index);
      CHECK_EQ(*key, data->get(0));

      if (expected_value_opt) {
        CHECK(expected_value_opt.value()->StrictEquals(data->get(1)));
      }

      if (expected_details_opt) {
        CHECK_EQ(expected_details_opt.value().AsSmi(), data->get(2));
      }
    }
  }

  void CheckDataAtKey(const Key& expected_key, IndexOpt expected_index,
                      ValueOpt expected_value = kNoValue,
                      PropertyDetailsOpt expected_details = kNoDetails) {
    Handle<Name> key_handle = CreateKeyWithHash(isolate, keys_, expected_key);
    base::Optional<Handle<Object>> value_handle_opt;
    if (expected_value) {
      value_handle_opt = isolate->factory()->NewStringFromAsciiChecked(
          expected_value.value().c_str(), AllocationType::kYoung);
    }

    CheckDataAtKey(key_handle, expected_index, value_handle_opt,
                   expected_details);
  }

  void CheckKeyAbsent(Handle<Name> key) {
    CHECK(runner_.FindEntry(key).is_not_found());
  }

  void CheckKeyAbsent(const Key& expected_key) {
    Handle<Name> key_handle = CreateKeyWithHash(isolate, keys_, expected_key);
    CheckKeyAbsent(key_handle);
  }

  void CheckHasKey(const Key& expected_key) {
    Handle<Name> key_handle = CreateKeyWithHash(isolate, keys_, expected_key);

    CHECK(runner_.FindEntry(key_handle).is_found());
  }

  void CheckCounts(base::Optional<int> capacity,
                   base::Optional<int> elements = base::Optional<int>(),
                   base::Optional<int> deleted = base::Optional<int>()) {
    runner_.CheckCounts(capacity, elements, deleted);
  }

  void CheckEnumerationOrder(const std::vector<std::string>& keys) {
    runner_.CheckEnumerationOrder(keys);
  }

  void RehashInplace() { runner_.RehashInplace(); }

  void Shrink() { runner_.Shrink(); }

  void CheckCopy() { runner_.CheckCopy(); }

  static constexpr bool IsRuntimeTest() {
    return std::is_same<TestRunner, RuntimeTestRunner>::value;
  }

  void VerifyHeap() { runner_.VerifyHeap(); }

  // Just for debugging
  void Print() { runner_.PrintTable(); }

  static std::vector<int> boundary_indices(int capacity) {
    if (capacity == 4 && SwissNameDictionary::MaxUsableCapacity(4) < 4) {
      // If we cannot put 4 entries in a capacity 4 table without resizing, just
      // work with 3 boundary indices.
      return {0, capacity - 2, capacity - 1};
    }
    return {0, 1, capacity - 2, capacity - 1};
  }

  // Contains all possible PropertyDetails suitable for storing in a
  // SwissNameDictionary (i.e., PropertyDetails for dictionary mode objects
  // without storing an enumeration index). Used to ensure that we can correctly
  // store an retrieve all possible such PropertyDetails.
  static const std::vector<PropertyDetails> distinct_property_details;

  static void WithAllInterestingInitialCapacities(
      std::function<void(TestSequence&)> manipulate_sequence) {
    WithInitialCapacities(interesting_initial_capacities, manipulate_sequence);
  }

  static void WithInitialCapacity(
      int capacity, std::function<void(TestSequence&)> manipulate_sequence) {
    WithInitialCapacities({capacity}, manipulate_sequence);
  }

  // For each capacity in |capacities|, create a TestSequence and run the given
  // function on it.
  static void WithInitialCapacities(
      const std::vector<int>& capacities,
      std::function<void(TestSequence&)> manipulate_sequence) {
    for (int capacity : capacities) {
      Isolate* isolate = CcTest::InitIsolateOnce();
      HandleScope scope{isolate};
      TestSequence<TestRunner> s(isolate, capacity);
      manipulate_sequence(s);
    }
  }

  Isolate* const isolate;
  const int initial_capacity;

 private:
  // Caches keys used in this TestSequence. See |create_key_with_hash| for
  // details.
  KeyCache keys_;
  TestRunner runner_;
};

}  // namespace test_swiss_hash_table
}  // namespace internal
}  // namespace v8

#endif  // V8_TEST_CCTEST_TEST_SWISS_NAME_DICTIONARY_INFRA_H_
