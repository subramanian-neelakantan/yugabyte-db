// Copyright (c) YugaByte, Inc.
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

#pragma once

#include <optional>
#include <unordered_map>

#include <boost/functional/hash.hpp>

#include <google/protobuf/repeated_field.h>

#include "yb/common/common_fwd.h"
#include "yb/common/column_id.h"

#include "yb/docdb/docdb.fwd.h"
#include "yb/docdb/docdb_fwd.h"

#include "yb/util/byte_buffer.h"
#include "yb/util/kv_util.h"

namespace yb {
namespace docdb {

// The packed row is encoded in the following format.
// Row packing/unpacking is accompanied by SchemaPacking class that is built from schema.
//
// We distinguish variable length columns from fixed length column.
// Fixed length column is non null columns with fixed length encoding, for instance int32.
// All other columns are variable length, for instance string or nullable int64.
//
// SchemaPacking contains ColumnPackingData for each column. See fields meaning below.
//
// For each variable length column we store offset of the end of his data.
// It allows us to easily find bytes range used by particular column.
// I.e. to find column data start we could just get end of previous varlen column and
// add offset_after_prev_varlen_column to it.
// Then in case of fixed column we could just add its size to find end of data.
// And in case of varlen column offset of the end of the data is just stored.
//
// The data is encoded as value type + actual encoded data.
//
// The serialized format:
// varint: schema_version
// uint32: end_of_column_data for the 1st varlen column
// ...
// uint32: end_of_column_data for the last varlen column
// bytes: data for the 1st column
// ...
// bytes: data for the last column
//
// NULL values are stored as columns with 0 length.
// Since we always embed ValueType in column data, we could easily distinguish NULL values
// from columns with empty value.
//
// The rationale for this format is to have ability to extract column value with O(1) complexity.
// Also it helps us to avoid storing common data for all rows, and put it to a single schema info.

// Replaces the schema version in packed value with the provided schema version.
// Note: Value starts with the schema version (does not contain control fields, value type).
Status ReplaceSchemaVersionInPackedValue(const Slice& value,
                                         const ValueControlFields& control_fields,
                                         SchemaVersion schema_version,
                                         ValueBuffer *out);
class RowPacker {
 public:
  RowPacker(SchemaVersion version, std::reference_wrapper<const SchemaPacking> packing,
            size_t packed_size_limit, const ValueControlFields& control_fields);

  RowPacker(SchemaVersion version, std::reference_wrapper<const SchemaPacking> packing,
            size_t packed_size_limit, const Slice& control_fields);

  bool Empty() const {
    return idx_ == 0;
  }

  bool Finished() const;

  void Restart();

  ColumnId NextColumnId() const;
  Result<const ColumnPackingData&> NextColumnData() const;

  // Returns false when unable to add value due to packed size limit.
  // tail_size is added to proposed encoded size, to make decision whether encoded value fits
  // into bounds or not.
  Result<bool> AddValue(ColumnId column_id, const Slice& value, ssize_t tail_size);
  // Add value consisting of 2 parts - value_prefix+value_suffix.
  Result<bool> AddValue(
      ColumnId column_id, const Slice& value_prefix, const Slice& value_suffix, ssize_t tail_size);
  Result<bool> AddValue(ColumnId column_id, const QLValuePB& value);
  Result<bool> AddValue(
      ColumnId column_id, const Slice& control_fields, const QLValuePB& value);

  Result<Slice> Complete();

 private:
  void Init(SchemaVersion version);

  template <class Value>
  Result<bool> DoAddValue(ColumnId column_id, const Value& value, ssize_t tail_size);

  const SchemaPacking& packing_;
  const ssize_t packed_size_limit_;
  size_t idx_ = 0;
  size_t prefix_end_;
  ValueBuffer result_;
  size_t varlen_write_pos_;
};

} // namespace docdb
} // namespace yb
