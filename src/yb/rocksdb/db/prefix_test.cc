//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//


#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr, "Please install gflags to run this test... Skipping...\n");
  return 0;
}
#else

#include <algorithm>
#include <iostream>
#include <vector>

#include "yb/util/flags.h"
#include "yb/rocksdb/comparator.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/filter_policy.h"
#include "yb/rocksdb/perf_context.h"
#include "yb/rocksdb/slice_transform.h"
#include "yb/rocksdb/memtablerep.h"
#include "yb/rocksdb/table.h"
#include "yb/rocksdb/util/histogram.h"
#include "yb/rocksdb/util/stop_watch.h"
#include "yb/rocksdb/util/testharness.h"
#include "yb/rocksdb/util/testutil.h"

#include "yb/util/random_util.h"
#include "yb/util/string_util.h"
#include "yb/util/test_util.h"

using GFLAGS::ParseCommandLineFlags;

DEFINE_UNKNOWN_bool(trigger_deadlock, false,
            "issue delete in range scan to trigger PrefixHashMap deadlock");
DEFINE_UNKNOWN_int32(bucket_count, 100000, "number of buckets");
DEFINE_UNKNOWN_uint64(num_locks, 10001, "number of locks");
DEFINE_UNKNOWN_bool(random_prefix, false, "randomize prefix");
DEFINE_UNKNOWN_uint64(total_prefixes, 100000, "total number of prefixes");
DEFINE_UNKNOWN_uint64(items_per_prefix, 1, "total number of values per prefix");
DEFINE_UNKNOWN_int64(write_buffer_size, 33554432, "");
DEFINE_UNKNOWN_int32(max_write_buffer_number, 2, "");
DEFINE_UNKNOWN_int32(min_write_buffer_number_to_merge, 1, "");
DEFINE_UNKNOWN_int32(skiplist_height, 4, "");
DEFINE_UNKNOWN_int32(memtable_prefix_bloom_bits, 10000000, "");
DEFINE_UNKNOWN_int32(memtable_prefix_bloom_probes, 10, "");
DEFINE_UNKNOWN_int32(memtable_prefix_bloom_huge_page_tlb_size, 2 * 1024 * 1024, "");
DEFINE_UNKNOWN_int32(value_size, 40, "");

// Path to the database on file system
const std::string kDbName = rocksdb::test::TmpDir() + "/prefix_test";

namespace rocksdb {

struct TestKey {
  uint64_t prefix;
  uint64_t sorted;

  TestKey(uint64_t _prefix, uint64_t _sorted)
      : prefix(_prefix), sorted(_sorted) {}
};

// return a slice backed by test_key
inline Slice TestKeyToSlice(const TestKey& test_key) {
  return Slice((const char*)&test_key, sizeof(test_key));
}

inline const TestKey* SliceToTestKey(const Slice& slice) {
  return (const TestKey*)slice.data();
}

class TestKeyComparator : public Comparator {
 public:

  // Compare needs to be aware of the possibility of a and/or b is
  // prefix only
  int Compare(const Slice& a, const Slice& b) const override {
    const TestKey* key_a = SliceToTestKey(a);
    const TestKey* key_b = SliceToTestKey(b);
    if (key_a->prefix != key_b->prefix) {
      if (key_a->prefix < key_b->prefix) return -1;
      if (key_a->prefix > key_b->prefix) return 1;
    } else {
      EXPECT_TRUE(key_a->prefix == key_b->prefix);
      // note, both a and b could be prefix only
      if (a.size() != b.size()) {
        // one of them is prefix
        EXPECT_TRUE(
            (a.size() == sizeof(uint64_t) && b.size() == sizeof(TestKey)) ||
            (b.size() == sizeof(uint64_t) && a.size() == sizeof(TestKey)));
        if (a.size() < b.size()) return -1;
        if (a.size() > b.size()) return 1;
      } else {
        // both a and b are prefix
        if (a.size() == sizeof(uint64_t)) {
          return 0;
        }

        // both a and b are whole key
        EXPECT_TRUE(a.size() == sizeof(TestKey) && b.size() == sizeof(TestKey));
        if (key_a->sorted < key_b->sorted) return -1;
        if (key_a->sorted > key_b->sorted) return 1;
        if (key_a->sorted == key_b->sorted) return 0;
      }
    }
    return 0;
  }

  const char* Name() const override {
    return "TestKeyComparator";
  }

  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const override {}

  void FindShortSuccessor(std::string* key) const override {}
};

namespace {
void PutKey(DB* db, WriteOptions write_options, uint64_t prefix,
            uint64_t suffix, const Slice& value) {
  TestKey test_key(prefix, suffix);
  Slice key = TestKeyToSlice(test_key);
  ASSERT_OK(db->Put(write_options, key, value));
}

void SeekIterator(Iterator* iter, uint64_t prefix, uint64_t suffix) {
  TestKey test_key(prefix, suffix);
  Slice key = TestKeyToSlice(test_key);
  iter->Seek(key);
}

const std::string kNotFoundResult = "NOT_FOUND";

std::string Get(DB* db, const ReadOptions& read_options, uint64_t prefix,
                uint64_t suffix) {
  TestKey test_key(prefix, suffix);
  Slice key = TestKeyToSlice(test_key);

  std::string result;
  Status s = db->Get(read_options, key, &result);
  if (s.IsNotFound()) {
    result = kNotFoundResult;
  } else if (!s.ok()) {
    result = s.ToString();
  }
  return result;
}
}  // namespace

class PrefixTest : public RocksDBTest {
 public:
  std::shared_ptr<DB> OpenDb() {
    DB* db;

    options.create_if_missing = true;
    options.write_buffer_size = FLAGS_write_buffer_size;
    options.max_write_buffer_number = FLAGS_max_write_buffer_number;
    options.min_write_buffer_number_to_merge =
      FLAGS_min_write_buffer_number_to_merge;

    options.memtable_prefix_bloom_bits = FLAGS_memtable_prefix_bloom_bits;
    options.memtable_prefix_bloom_probes = FLAGS_memtable_prefix_bloom_probes;
    options.memtable_prefix_bloom_huge_page_tlb_size =
        FLAGS_memtable_prefix_bloom_huge_page_tlb_size;

    options.prefix_extractor.reset(NewFixedPrefixTransform(8));
    BlockBasedTableOptions bbto;
    bbto.filter_policy.reset(NewBloomFilterPolicy(10, false));
    bbto.whole_key_filtering = false;
    options.table_factory.reset(NewBlockBasedTableFactory(bbto));

    Status s = DB::Open(options, kDbName,  &db);
    EXPECT_OK(s);
    return std::shared_ptr<DB>(db);
  }

  void FirstOption() {
    option_config_ = kBegin;
  }

  bool NextOptions(int bucket_count) {
    // skip some options
    option_config_++;
    if (option_config_ < kEnd) {
      options.prefix_extractor.reset(NewFixedPrefixTransform(8));
      switch(option_config_) {
        case kHashSkipList:
          options.memtable_factory.reset(
              NewHashSkipListRepFactory(bucket_count, FLAGS_skiplist_height));
          return true;
        case kHashLinkList:
          options.memtable_factory.reset(
              NewHashLinkListRepFactory(bucket_count));
          return true;
        case kHashLinkListHugePageTlb:
          options.memtable_factory.reset(
              NewHashLinkListRepFactory(bucket_count, 2 * 1024 * 1024));
          return true;
        case kHashLinkListTriggerSkipList:
          options.memtable_factory.reset(
              NewHashLinkListRepFactory(bucket_count, 0, 3));
          return true;
        default:
          return false;
      }
    }
    return false;
  }

  PrefixTest() : option_config_(kBegin) {
    options.comparator = new TestKeyComparator();
  }
  ~PrefixTest() {
    delete options.comparator;
  }
 protected:
  enum OptionConfig {
    kBegin,
    kHashSkipList,
    kHashLinkList,
    kHashLinkListHugePageTlb,
    kHashLinkListTriggerSkipList,
    kEnd
  };
  int option_config_;
  Options options;
};

TEST_F(PrefixTest, TestResult) {
  for (int num_buckets = 1; num_buckets <= 2; num_buckets++) {
    FirstOption();
    while (NextOptions(num_buckets)) {
      std::cout << "*** Mem table: " << options.memtable_factory->Name()
                << " number of buckets: " << num_buckets
                << std::endl;
      ASSERT_OK(DestroyDB(kDbName, Options()));
      auto db = OpenDb();
      WriteOptions write_options;
      ReadOptions read_options;

      // 1. Insert one row.
      Slice v16("v16");
      PutKey(db.get(), write_options, 1, 6, v16);
      std::unique_ptr<Iterator> iter(db->NewIterator(read_options));
      SeekIterator(iter.get(), 1, 6);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v16 == iter->value());
      SeekIterator(iter.get(), 1, 5);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v16 == iter->value());
      SeekIterator(iter.get(), 1, 5);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v16 == iter->value());
      iter->Next();
      ASSERT_TRUE(!iter->Valid());

      SeekIterator(iter.get(), 2, 0);
      ASSERT_TRUE(!iter->Valid());

      ASSERT_EQ(v16.ToString(), Get(db.get(), read_options, 1, 6));
      ASSERT_EQ(kNotFoundResult, Get(db.get(), read_options, 1, 5));
      ASSERT_EQ(kNotFoundResult, Get(db.get(), read_options, 1, 7));
      ASSERT_EQ(kNotFoundResult, Get(db.get(), read_options, 0, 6));
      ASSERT_EQ(kNotFoundResult, Get(db.get(), read_options, 2, 6));

      // 2. Insert an entry for the same prefix as the last entry in the bucket.
      Slice v17("v17");
      PutKey(db.get(), write_options, 1, 7, v17);
      iter.reset(db->NewIterator(read_options));
      SeekIterator(iter.get(), 1, 7);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      SeekIterator(iter.get(), 1, 6);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v16 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());
      iter->Next();
      ASSERT_TRUE(!iter->Valid());

      SeekIterator(iter.get(), 2, 0);
      ASSERT_TRUE(!iter->Valid());

      // 3. Insert an entry for the same prefix as the head of the bucket.
      Slice v15("v15");
      PutKey(db.get(), write_options, 1, 5, v15);
      iter.reset(db->NewIterator(read_options));

      SeekIterator(iter.get(), 1, 7);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      SeekIterator(iter.get(), 1, 5);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v15 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v16 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      SeekIterator(iter.get(), 1, 5);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v15 == iter->value());

      ASSERT_EQ(v15.ToString(), Get(db.get(), read_options, 1, 5));
      ASSERT_EQ(v16.ToString(), Get(db.get(), read_options, 1, 6));
      ASSERT_EQ(v17.ToString(), Get(db.get(), read_options, 1, 7));

      // 4. Insert an entry with a larger prefix
      Slice v22("v22");
      PutKey(db.get(), write_options, 2, 2, v22);
      iter.reset(db->NewIterator(read_options));

      SeekIterator(iter.get(), 2, 2);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v22 == iter->value());
      SeekIterator(iter.get(), 2, 0);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v22 == iter->value());

      SeekIterator(iter.get(), 1, 5);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v15 == iter->value());

      SeekIterator(iter.get(), 1, 7);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      // 5. Insert an entry with a smaller prefix
      Slice v02("v02");
      PutKey(db.get(), write_options, 0, 2, v02);
      iter.reset(db->NewIterator(read_options));

      SeekIterator(iter.get(), 0, 2);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v02 == iter->value());
      SeekIterator(iter.get(), 0, 0);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v02 == iter->value());

      SeekIterator(iter.get(), 2, 0);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v22 == iter->value());

      SeekIterator(iter.get(), 1, 5);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v15 == iter->value());

      SeekIterator(iter.get(), 1, 7);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      // 6. Insert to the beginning and the end of the first prefix
      Slice v13("v13");
      Slice v18("v18");
      PutKey(db.get(), write_options, 1, 3, v13);
      PutKey(db.get(), write_options, 1, 8, v18);
      iter.reset(db->NewIterator(read_options));
      SeekIterator(iter.get(), 1, 7);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      SeekIterator(iter.get(), 1, 3);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v13 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v15 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v16 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());
      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v18 == iter->value());

      SeekIterator(iter.get(), 0, 0);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v02 == iter->value());

      SeekIterator(iter.get(), 2, 0);
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v22 == iter->value());

      ASSERT_EQ(v22.ToString(), Get(db.get(), read_options, 2, 2));
      ASSERT_EQ(v02.ToString(), Get(db.get(), read_options, 0, 2));
      ASSERT_EQ(v13.ToString(), Get(db.get(), read_options, 1, 3));
      ASSERT_EQ(v15.ToString(), Get(db.get(), read_options, 1, 5));
      ASSERT_EQ(v16.ToString(), Get(db.get(), read_options, 1, 6));
      ASSERT_EQ(v17.ToString(), Get(db.get(), read_options, 1, 7));
      ASSERT_EQ(v18.ToString(), Get(db.get(), read_options, 1, 8));
    }
  }
}

// Show results in prefix
TEST_F(PrefixTest, PrefixValid) {
  for (int num_buckets = 1; num_buckets <= 2; num_buckets++) {
    FirstOption();
    while (NextOptions(num_buckets)) {
      std::cout << "*** Mem table: " << options.memtable_factory->Name()
                << " number of buckets: " << num_buckets << std::endl;
      ASSERT_OK(DestroyDB(kDbName, Options()));
      auto db = OpenDb();
      WriteOptions write_options;
      ReadOptions read_options;

      // Insert keys with common prefix and one key with different
      Slice v16("v16");
      Slice v17("v17");
      Slice v18("v18");
      Slice v19("v19");
      PutKey(db.get(), write_options, 12345, 6, v16);
      PutKey(db.get(), write_options, 12345, 7, v17);
      PutKey(db.get(), write_options, 12345, 8, v18);
      PutKey(db.get(), write_options, 12345, 9, v19);
      PutKey(db.get(), write_options, 12346, 8, v16);
      ASSERT_OK(db->Flush(FlushOptions()));
      ASSERT_OK(db->Delete(write_options, TestKeyToSlice(TestKey(12346, 8))));
      ASSERT_OK(db->Flush(FlushOptions()));
      read_options.prefix_same_as_start = true;
      std::unique_ptr<Iterator> iter(db->NewIterator(read_options));
      ASSERT_OK(iter->status());
      SeekIterator(iter.get(), 12345, 6);
      ASSERT_TRUE(iter->Valid()) << "iter->status(): " << iter->status();
      ASSERT_TRUE(v16 == iter->value());

      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v17 == iter->value());

      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v18 == iter->value());

      iter->Next();
      ASSERT_TRUE(iter->Valid());
      ASSERT_TRUE(v19 == iter->value());
      iter->Next();
      ASSERT_FALSE(iter->Valid());
      ASSERT_EQ(kNotFoundResult, Get(db.get(), read_options, 12346, 8));
    }
  }
}

TEST_F(PrefixTest, DynamicPrefixIterator) {
  while (NextOptions(FLAGS_bucket_count)) {
    std::cout << "*** Mem table: " << options.memtable_factory->Name()
        << std::endl;
    ASSERT_OK(DestroyDB(kDbName, Options()));
    auto db = OpenDb();
    WriteOptions write_options;
    ReadOptions read_options;

    std::vector<uint64_t> prefixes;
    for (uint64_t i = 0; i < FLAGS_total_prefixes; ++i) {
      prefixes.push_back(i);
    }

    if (FLAGS_random_prefix) {
      std::shuffle(prefixes.begin(), prefixes.end(), yb::ThreadLocalRandom());
    }

    HistogramImpl hist_put_time;
    HistogramImpl hist_put_comparison;

    // insert x random prefix, each with y continuous element.
    for (auto prefix : prefixes) {
       for (uint64_t sorted = 0; sorted < FLAGS_items_per_prefix; sorted++) {
        TestKey test_key(prefix, sorted);

        Slice key = TestKeyToSlice(test_key);
        std::string value(FLAGS_value_size, 0);

        perf_context.Reset();
        StopWatchNano timer(Env::Default(), true);
        ASSERT_OK(db->Put(write_options, key, value));
        hist_put_time.Add(timer.ElapsedNanos());
        hist_put_comparison.Add(perf_context.user_key_comparison_count);
      }
    }

    std::cout << "Put key comparison: \n" << hist_put_comparison.ToString()
              << "Put time: \n" << hist_put_time.ToString();

    // test seek existing keys
    HistogramImpl hist_seek_time;
    HistogramImpl hist_seek_comparison;

    std::unique_ptr<Iterator> iter(db->NewIterator(read_options));

    for (auto prefix : prefixes) {
      TestKey test_key(prefix, FLAGS_items_per_prefix / 2);
      Slice key = TestKeyToSlice(test_key);
      std::string value = "v" + ToString(0);

      perf_context.Reset();
      StopWatchNano timer(Env::Default(), true);
      auto key_prefix = options.prefix_extractor->Transform(key);
      uint64_t total_keys = 0;
      for (iter->Seek(key);
           iter->Valid() && iter->key().starts_with(key_prefix);
           iter->Next()) {
        if (FLAGS_trigger_deadlock) {
          std::cout << "Behold the deadlock!\n";
          ASSERT_OK(db->Delete(write_options, iter->key()));
        }
        total_keys++;
      }
      hist_seek_time.Add(timer.ElapsedNanos());
      hist_seek_comparison.Add(perf_context.user_key_comparison_count);
      ASSERT_EQ(total_keys, FLAGS_items_per_prefix - FLAGS_items_per_prefix/2);
    }

    std::cout << "Seek key comparison: \n"
              << hist_seek_comparison.ToString()
              << "Seek time: \n"
              << hist_seek_time.ToString();

    // test non-existing keys
    HistogramImpl hist_no_seek_time;
    HistogramImpl hist_no_seek_comparison;

    for (auto prefix = FLAGS_total_prefixes;
         prefix < FLAGS_total_prefixes + 10000;
         prefix++) {
      TestKey test_key(prefix, 0);
      Slice key = TestKeyToSlice(test_key);

      perf_context.Reset();
      StopWatchNano timer(Env::Default(), true);
      iter->Seek(key);
      hist_no_seek_time.Add(timer.ElapsedNanos());
      hist_no_seek_comparison.Add(perf_context.user_key_comparison_count);
      ASSERT_TRUE(!iter->Valid());
    }

    std::cout << "non-existing Seek key comparison: \n"
              << hist_no_seek_comparison.ToString()
              << "non-existing Seek time: \n"
              << hist_no_seek_time.ToString();
  }
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ParseCommandLineFlags(&argc, &argv, true);
  std::cout << kDbName << "\n";

  return RUN_ALL_TESTS();
}

#endif  // GFLAGS
