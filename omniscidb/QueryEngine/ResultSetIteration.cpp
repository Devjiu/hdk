/**
 * @file    ResultSetIteration.cpp
 * @author  Alex Suhan <alex@mapd.com>
 * @brief   Iteration part of the row set interface.
 *
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 */

#include "Execute.h"
#include "ResultSet.h"
#include "ResultRows.h"
#include "RuntimeFunctions.h"
#include "SqlTypesLayout.h"

namespace {

// Interprets ptr as an integer of compact_sz byte width and reads it.
int64_t read_int_from_buff(const int8_t* ptr, const int8_t compact_sz) {
  switch (compact_sz) {
    case 8: {
      return *reinterpret_cast<const int64_t*>(ptr);
    }
    case 4: {
      return *reinterpret_cast<const int32_t*>(ptr);
    }
    default:
      CHECK(false);
  }
  CHECK(false);
  return 0;
}

// Interprets ptr1, ptr2 as the sum and count pair used for AVG.
TargetValue make_avg_target_value(const int8_t* ptr1,
                                  const int8_t compact_sz1,
                                  const int8_t* ptr2,
                                  const int8_t compact_sz2,
                                  const TargetInfo& target_info) {
  int64_t sum{0};
  if (target_info.agg_arg_type.is_integer()) {
    sum = read_int_from_buff(ptr1, compact_sz1);
  } else if (target_info.agg_arg_type.is_fp()) {
    switch (compact_sz1) {
      case 8: {
        double d = *reinterpret_cast<const double*>(ptr1);
        sum = *reinterpret_cast<const int64_t*>(&d);
        break;
      }
      case 4: {
        double d = *reinterpret_cast<const float*>(ptr1);
        sum = *reinterpret_cast<const int64_t*>(&d);
        break;
      }
      default:
        CHECK(false);
    }
  } else {
    CHECK(false);
  }
  const auto count = read_int_from_buff(ptr2, compact_sz2);
  return pair_to_double({sum, count}, target_info.sql_type);
}

// Gets the byte offset, starting from the beginning of the row targets buffer, of
// the value in position slot_idx (only makes sense for row-wise representation).
size_t get_byteoff_of_slot(const size_t slot_idx, const QueryMemoryDescriptor& query_mem_desc) {
  size_t result = 0;
  for (size_t i = 0; i < slot_idx; ++i) {
    result += query_mem_desc.agg_col_widths[i].compact;
  }
  return result;
}

// Given the entire buffer for the result set, buff, finds the beginning of the
// column for slot_idx. Only makes sense for column-wise representation.
const int8_t* advance_col_buff_to_slot(const int8_t* buff,
                                       const QueryMemoryDescriptor& query_mem_desc,
                                       const std::vector<TargetInfo>& targets,
                                       const size_t slot_idx) {
  auto crt_col_ptr = get_cols_ptr(buff, query_mem_desc);
  const auto buffer_col_count = get_buffer_col_slot_count(query_mem_desc);
  size_t agg_col_idx{0};
  for (size_t target_idx = 0; target_idx < targets.size(); ++target_idx) {
    if (agg_col_idx == slot_idx) {
      return crt_col_ptr;
    }
    CHECK_LT(agg_col_idx, buffer_col_count);
    const auto& agg_info = targets[target_idx];
    crt_col_ptr = advance_to_next_columnar_target_buff(crt_col_ptr, query_mem_desc, agg_col_idx);
    if (agg_info.is_agg && agg_info.agg_kind == kAVG) {
      if (agg_col_idx + 1 == slot_idx) {
        return crt_col_ptr;
      }
      crt_col_ptr = advance_to_next_columnar_target_buff(crt_col_ptr, query_mem_desc, agg_col_idx + 1);
    }
    agg_col_idx = advance_slot(agg_col_idx, agg_info);
  }
  CHECK(false);
  return nullptr;
}

}  // namespace

std::vector<TargetValue> ResultSet::getNextRow(const bool translate_strings, const bool decimal_to_double) const {
  while (fetched_so_far_ < drop_first_) {
    getNextRowImpl(translate_strings, decimal_to_double);
  }
  return getNextRowImpl(translate_strings, decimal_to_double);
}

std::vector<TargetValue> ResultSet::getNextRowImpl(const bool translate_strings, const bool decimal_to_double) const {
  auto entry_buff_idx = advanceCursorToNextEntry();
  if (keep_first_ && fetched_so_far_ >= drop_first_ + keep_first_) {
    return {};
  }
  if (crt_row_buff_idx_ >= entryCount()) {
    CHECK_EQ(entryCount(), crt_row_buff_idx_);
    return {};
  }
  const ResultSetStorage* storage{nullptr};
  std::tie(storage, entry_buff_idx) = findStorage(entry_buff_idx);
  const auto buff = storage->buff_;
  CHECK(buff);
  std::vector<TargetValue> row;
  size_t agg_col_idx = 0;
  const auto buffer_col_count = get_buffer_col_slot_count(storage_->query_mem_desc_);
  const int8_t* rowwise_target_ptr{nullptr};
  const int8_t* crt_col_ptr{nullptr};
  if (query_mem_desc_.output_columnar) {
    crt_col_ptr = get_cols_ptr(buff, query_mem_desc_);
  } else {
    rowwise_target_ptr =
        row_ptr_rowwise(buff, query_mem_desc_, entry_buff_idx) + get_key_bytes_rowwise(query_mem_desc_);
  }
  for (size_t target_idx = 0; target_idx < storage_->targets_.size(); ++target_idx) {
    CHECK_LT(agg_col_idx, buffer_col_count);
    const auto& agg_info = storage_->targets_[target_idx];
    if (query_mem_desc_.output_columnar) {
      const auto next_col_ptr = advance_to_next_columnar_target_buff(crt_col_ptr, query_mem_desc_, agg_col_idx);
      const auto col2_ptr = (agg_info.is_agg && agg_info.agg_kind == kAVG) ? next_col_ptr : nullptr;
      const auto compact_sz2 =
          (agg_info.is_agg && agg_info.agg_kind == kAVG) ? query_mem_desc_.agg_col_widths[agg_col_idx + 1].compact : 0;
      row.push_back(getTargetValueFromBufferColwise(crt_col_ptr,
                                                    query_mem_desc_.agg_col_widths[agg_col_idx].compact,
                                                    col2_ptr,
                                                    compact_sz2,
                                                    entry_buff_idx,
                                                    agg_info,
                                                    target_idx,
                                                    translate_strings,
                                                    decimal_to_double));
      crt_col_ptr = next_col_ptr;
      if (agg_info.is_agg && agg_info.agg_kind == kAVG) {
        crt_col_ptr = advance_to_next_columnar_target_buff(crt_col_ptr, query_mem_desc_, agg_col_idx + 1);
      }
    } else {
      row.push_back(getTargetValueFromBufferRowwise(
          rowwise_target_ptr, agg_info, target_idx, agg_col_idx, translate_strings, decimal_to_double));
      rowwise_target_ptr = advance_target_ptr(rowwise_target_ptr, agg_info, agg_col_idx, query_mem_desc_);
    }
    agg_col_idx = advance_slot(agg_col_idx, agg_info);
  }
  ++crt_row_buff_idx_;
  ++fetched_so_far_;
  return row;
}

namespace {

const int8_t* columnar_elem_ptr(const size_t entry_idx, const int8_t* col1_ptr, const int8_t compact_sz1) {
  return col1_ptr + compact_sz1 * entry_idx;
}

}  // namespace

InternalTargetValue ResultSet::getColumnInternal(const int8_t* buff,
                                                 const size_t entry_idx,
                                                 const size_t col_idx) const {
  CHECK(buff);
  const auto buffer_col_count = get_buffer_col_slot_count(storage_->query_mem_desc_);
  const int8_t* rowwise_target_ptr{nullptr};
  const int8_t* crt_col_ptr{nullptr};
  if (query_mem_desc_.output_columnar) {
    crt_col_ptr = get_cols_ptr(buff, query_mem_desc_);
  } else {
    rowwise_target_ptr = row_ptr_rowwise(buff, query_mem_desc_, entry_idx) + get_key_bytes_rowwise(query_mem_desc_);
  }
  CHECK_LT(col_idx, storage_->targets_.size());
  size_t agg_col_idx = 0;
  // TODO(alex): remove this loop, offsets can be computed only once
  for (size_t target_idx = 0; target_idx < storage_->targets_.size(); ++target_idx) {
    CHECK_LT(agg_col_idx, buffer_col_count);
    const auto& agg_info = storage_->targets_[target_idx];
    if (query_mem_desc_.output_columnar) {
      const auto next_col_ptr = advance_to_next_columnar_target_buff(crt_col_ptr, query_mem_desc_, agg_col_idx);
      const auto col2_ptr = (agg_info.is_agg && agg_info.agg_kind == kAVG) ? next_col_ptr : nullptr;
      const auto compact_sz2 =
          (agg_info.is_agg && agg_info.agg_kind == kAVG) ? query_mem_desc_.agg_col_widths[agg_col_idx + 1].compact : 0;
      if (target_idx == col_idx) {
        const auto compact_sz1 = query_mem_desc_.agg_col_widths[agg_col_idx].compact;
        const auto i1 = read_int_from_buff(columnar_elem_ptr(entry_idx, crt_col_ptr, compact_sz1), compact_sz1);
        if (col2_ptr) {
          const auto i2 = read_int_from_buff(columnar_elem_ptr(entry_idx, col2_ptr, compact_sz2), compact_sz2);
          return InternalTargetValue(i1, i2);
        } else {
          return InternalTargetValue(i1);
        }
      }
      crt_col_ptr = next_col_ptr;
      if (agg_info.is_agg && agg_info.agg_kind == kAVG) {
        crt_col_ptr = advance_to_next_columnar_target_buff(crt_col_ptr, query_mem_desc_, agg_col_idx + 1);
      }
    } else {
      const auto ptr1 = rowwise_target_ptr;
      const auto compact_sz1 = query_mem_desc_.agg_col_widths[agg_col_idx].compact;
      const int8_t* ptr2{nullptr};
      int8_t compact_sz2{0};
      if (agg_info.is_agg && agg_info.agg_kind == kAVG) {
        ptr2 = rowwise_target_ptr + query_mem_desc_.agg_col_widths[agg_col_idx].compact;
        compact_sz2 = query_mem_desc_.agg_col_widths[agg_col_idx + 1].compact;
      }
      if (target_idx == col_idx) {
        const auto i1 = read_int_from_buff(ptr1, compact_sz1);
        if (ptr2) {
          const auto i2 = read_int_from_buff(ptr2, compact_sz2);
          return InternalTargetValue(i1, i2);
        } else {
          return InternalTargetValue(i1);
        }
      }
      rowwise_target_ptr = advance_target_ptr(rowwise_target_ptr, agg_info, agg_col_idx, query_mem_desc_);
    }
    agg_col_idx = advance_slot(agg_col_idx, agg_info);
  }
  CHECK(false);
  return InternalTargetValue(int64_t(0));
}

// Not all entries in the buffer represent a valid row. Advance the internal cursor
// used for the getNextRow method to the next row which is valid.
size_t ResultSet::advanceCursorToNextEntry() const {
  CHECK(GroupByColRangeType::Scan != storage_->query_mem_desc_.hash_type);
  while (crt_row_buff_idx_ < entryCount()) {
    const ResultSetStorage* storage{nullptr};
    const auto entry_idx = permutation_.empty() ? crt_row_buff_idx_ : permutation_[crt_row_buff_idx_];
    size_t fixedup_entry_idx{entry_idx};
    std::tie(storage, fixedup_entry_idx) = findStorage(entry_idx);
    if (!storage->isEmptyEntry(fixedup_entry_idx)) {
      break;
    }
    ++crt_row_buff_idx_;
  }
  if (permutation_.empty()) {
    return crt_row_buff_idx_;
  }
  CHECK_LE(crt_row_buff_idx_, permutation_.size());
  return crt_row_buff_idx_ == permutation_.size() ? crt_row_buff_idx_ : permutation_[crt_row_buff_idx_];
}

size_t ResultSet::entryCount() const {
  return permutation_.empty() ? (query_mem_desc_.entry_count + query_mem_desc_.entry_count_small) : permutation_.size();
}

namespace {

int64_t lazy_decode(const ColumnLazyFetchInfo& col_lazy_fetch, const int8_t* byte_stream, const int64_t pos) {
  CHECK(col_lazy_fetch.is_lazily_fetched);
  const auto& type_info = col_lazy_fetch.type;
  const auto enc_type = type_info.get_compression();
  if (type_info.is_fp()) {
    if (type_info.get_type() == kFLOAT) {
      float fval = fixed_width_float_decode_noinline(byte_stream, pos);
      return *reinterpret_cast<int32_t*>(&fval);
    } else {
      double fval = fixed_width_double_decode_noinline(byte_stream, pos);
      return *reinterpret_cast<int64_t*>(&fval);
    }
  }
  CHECK(type_info.is_integer() || type_info.is_decimal() || type_info.is_time() || type_info.is_boolean() ||
        (type_info.is_string() && enc_type == kENCODING_DICT));
  size_t type_bitwidth = get_bit_width(type_info);
  if (type_info.get_compression() == kENCODING_FIXED) {
    type_bitwidth = type_info.get_comp_param();
  } else if (type_info.get_compression() == kENCODING_DICT) {
    type_bitwidth = 8 * type_info.get_size();
  }
  CHECK_EQ(size_t(0), type_bitwidth % 8);
  auto val = fixed_width_int_decode_noinline(byte_stream, type_bitwidth / 8, pos);
  if (type_info.get_compression() != kENCODING_NONE) {
    CHECK(type_info.get_compression() == kENCODING_FIXED || type_info.get_compression() == kENCODING_DICT);
    auto encoding = type_info.get_compression();
    if (encoding == kENCODING_FIXED) {
      encoding = kENCODING_NONE;
    }
    SQLTypeInfo col_logical_ti(type_info.get_type(),
                               type_info.get_dimension(),
                               type_info.get_scale(),
                               false,
                               encoding,
                               0,
                               type_info.get_subtype());
    if (val == inline_fixed_encoding_null_val(type_info)) {
      return inline_int_null_val(col_logical_ti);
    }
  }
  return val;
}

}  // namespace

// Interprets ptr1, ptr2 as the ptr and len pair used for raw string.
TargetValue ResultSet::makeRealStringTargetValue(const int8_t* ptr1,
                                                 const int8_t compact_sz1,
                                                 const int8_t* ptr2,
                                                 const int8_t compact_sz2,
                                                 const TargetInfo& target_info,
                                                 const size_t target_logical_idx) const {
  auto string_ptr = read_int_from_buff(ptr1, compact_sz1);
  if (!lazy_fetch_info_.empty()) {
    CHECK_LT(target_logical_idx, lazy_fetch_info_.size());
    const auto& col_lazy_fetch = lazy_fetch_info_[target_logical_idx];
    if (col_lazy_fetch.is_lazily_fetched) {
      const auto storage_idx = getStorageIndex(crt_row_buff_idx_);
      CHECK_LT(static_cast<size_t>(storage_idx.first), col_buffers_.size());
      auto& frag_col_buffers = col_buffers_[storage_idx.first];
      bool is_end{false};
      VarlenDatum vd;
      ChunkIter_get_nth(
          reinterpret_cast<ChunkIter*>(const_cast<int8_t*>(frag_col_buffers[col_lazy_fetch.local_col_id])),
          string_ptr,
          false,
          &vd,
          &is_end);
      CHECK(!is_end);
      if (vd.is_null) {
        return TargetValue(nullptr);
      }
      CHECK(vd.pointer);
      CHECK_GT(vd.length, 0);
      std::string fetched_str(reinterpret_cast<char*>(vd.pointer), vd.length);
      return fetched_str;
    }
  }
  if (!string_ptr) {
    return TargetValue(nullptr);
  }
  const auto length = read_int_from_buff(ptr2, compact_sz2);
  std::vector<int8_t> cpu_buffer;
  if (string_ptr && device_type_ == ExecutorDeviceType::GPU) {
    cpu_buffer.resize(length);
    auto& data_mgr = query_mem_desc_.executor_->catalog_->get_dataMgr();
    copy_from_gpu(&data_mgr, &cpu_buffer[0], static_cast<CUdeviceptr>(string_ptr), length, device_id_);
    string_ptr = reinterpret_cast<int64_t>(&cpu_buffer[0]);
  }
  return std::string(reinterpret_cast<char*>(string_ptr), length);
}

// Reads an integer or a float from ptr based on the type and the byte width.
TargetValue ResultSet::makeTargetValue(const int8_t* ptr,
                                       const int8_t compact_sz,
                                       const TargetInfo& target_info,
                                       const size_t target_logical_idx,
                                       const bool translate_strings,
                                       const bool decimal_to_double) const {
  auto ival = read_int_from_buff(ptr, compact_sz);
  const auto& chosen_type = get_compact_type(target_info);
  if (!lazy_fetch_info_.empty()) {
    CHECK_LT(target_logical_idx, lazy_fetch_info_.size());
    const auto& col_lazy_fetch = lazy_fetch_info_[target_logical_idx];
    if (col_lazy_fetch.is_lazily_fetched) {
      const auto storage_idx = getStorageIndex(crt_row_buff_idx_);
      CHECK_LT(static_cast<size_t>(storage_idx.first), col_buffers_.size());
      auto& frag_col_buffers = col_buffers_[storage_idx.first];
      ival = lazy_decode(col_lazy_fetch, frag_col_buffers[col_lazy_fetch.local_col_id], ival);
      if (chosen_type.is_fp()) {
        if (chosen_type.get_type() == kFLOAT) {
          const auto fval_int = static_cast<const int32_t>(ival);
          return ScalarTargetValue(*reinterpret_cast<const float*>(&fval_int));
        } else {
          return ScalarTargetValue(*reinterpret_cast<const double*>(&ival));
        }
      }
    }
  }
  if (chosen_type.is_fp()) {
    switch (compact_sz) {
      case 8: {
        const auto dval = *reinterpret_cast<const double*>(ptr);
        return chosen_type.get_type() == kFLOAT ? ScalarTargetValue(static_cast<const float>(dval))
                                                : ScalarTargetValue(dval);
      }
      case 4: {
        CHECK_EQ(kFLOAT, chosen_type.get_type());
        return *reinterpret_cast<const float*>(ptr);
      }
      default:
        CHECK(false);
    }
  }
  if (chosen_type.is_integer() | chosen_type.is_boolean() || chosen_type.is_time() || chosen_type.is_timeinterval()) {
    if (target_info.is_distinct) {
      return TargetValue(bitmap_set_size(ival, target_logical_idx, query_mem_desc_.count_distinct_descriptors_));
    }
    if (inline_int_null_val(chosen_type) == ival) {
      return inline_int_null_val(target_info.sql_type);
    }
    return ival;
  }
  if (chosen_type.is_string() && chosen_type.get_compression() == kENCODING_DICT) {
    if (translate_strings) {
      if (ival == NULL_INT) {
        return NullableString(nullptr);
      }
      const auto sd = executor_ ? executor_->getStringDictionary(chosen_type.get_comp_param(), row_set_mem_owner_)
                                : row_set_mem_owner_->getStringDict(chosen_type.get_comp_param());
      return NullableString(sd->getString(ival));
    } else {
      return ival;
    }
  }
  if (chosen_type.is_decimal()) {
    if (decimal_to_double) {
      if (ival == inline_int_null_val(SQLTypeInfo(decimal_to_int_type(chosen_type), false))) {
        return NULL_DOUBLE;
      }
      return static_cast<double>(ival) / exp_to_scale(chosen_type.get_scale());
    }
    return ival;
  }
  CHECK(false);
  return TargetValue(int64_t(0));
}

// Gets the TargetValue stored at position entry_idx in the col1_ptr and col2_ptr
// column buffers. The second column is only used for AVG.
TargetValue ResultSet::getTargetValueFromBufferColwise(const int8_t* col1_ptr,
                                                       const int8_t compact_sz1,
                                                       const int8_t* col2_ptr,
                                                       const int8_t compact_sz2,
                                                       const size_t entry_idx,
                                                       const TargetInfo& target_info,
                                                       const size_t target_logical_idx,
                                                       const bool translate_strings,
                                                       const bool decimal_to_double) const {
  CHECK(query_mem_desc_.output_columnar);
  const auto ptr1 = columnar_elem_ptr(entry_idx, col1_ptr, compact_sz1);
  if (target_info.agg_kind == kAVG || is_real_str_or_array(target_info)) {
    CHECK(col2_ptr);
    CHECK(compact_sz2);
    const auto ptr2 = columnar_elem_ptr(entry_idx, col2_ptr, compact_sz2);
    return target_info.agg_kind == kAVG
               ? make_avg_target_value(ptr1, compact_sz1, ptr2, compact_sz2, target_info)
               : makeRealStringTargetValue(ptr1, compact_sz1, ptr2, compact_sz2, target_info, target_logical_idx);
  }
  return makeTargetValue(ptr1, compact_sz1, target_info, target_logical_idx, translate_strings, decimal_to_double);
}

// Gets the TargetValue stored in slot_idx (and slot_idx for AVG) of rowwise_target_ptr.
TargetValue ResultSet::getTargetValueFromBufferRowwise(const int8_t* rowwise_target_ptr,
                                                       const TargetInfo& target_info,
                                                       const size_t target_logical_idx,
                                                       const size_t slot_idx,
                                                       const bool translate_strings,
                                                       const bool decimal_to_double) const {
  auto ptr1 = rowwise_target_ptr;
  auto compact_sz1 = query_mem_desc_.agg_col_widths[slot_idx].compact;
  if (target_info.agg_kind == kAVG || is_real_str_or_array(target_info)) {
    const auto ptr2 = rowwise_target_ptr + query_mem_desc_.agg_col_widths[slot_idx].compact;
    const auto compact_sz2 = query_mem_desc_.agg_col_widths[slot_idx + 1].compact;
    CHECK(ptr2);
    return target_info.agg_kind == kAVG
               ? make_avg_target_value(ptr1, compact_sz1, ptr2, compact_sz2, target_info)
               : makeRealStringTargetValue(ptr1, compact_sz1, ptr2, compact_sz2, target_info, target_logical_idx);
  }
  return makeTargetValue(ptr1, compact_sz1, target_info, target_logical_idx, translate_strings, decimal_to_double);
}

// Returns true iff the entry at position entry_idx in buff contains a valid row.
bool ResultSetStorage::isEmptyEntry(const size_t entry_idx, const int8_t* buff) const {
  CHECK(query_mem_desc_.hash_type != GroupByColRangeType::Scan);
  if (query_mem_desc_.keyless_hash) {
    CHECK(query_mem_desc_.hash_type == GroupByColRangeType::OneColKnownRange ||
          query_mem_desc_.hash_type == GroupByColRangeType::MultiColPerfectHash);
    CHECK_GE(query_mem_desc_.idx_target_as_key, 0);
    CHECK_LT(static_cast<size_t>(query_mem_desc_.idx_target_as_key), target_init_vals_.size());
    if (query_mem_desc_.output_columnar) {
      const auto col_buff =
          advance_col_buff_to_slot(buff, query_mem_desc_, targets_, query_mem_desc_.idx_target_as_key);
      const auto entry_buff =
          col_buff + entry_idx * query_mem_desc_.agg_col_widths[query_mem_desc_.idx_target_as_key].compact;
      return read_int_from_buff(entry_buff,
                                query_mem_desc_.agg_col_widths[query_mem_desc_.idx_target_as_key].compact) ==
             target_init_vals_[query_mem_desc_.idx_target_as_key];
    }
    const auto rowwise_target_ptr =
        row_ptr_rowwise(buff, query_mem_desc_, entry_idx) + get_key_bytes_rowwise(query_mem_desc_);
    const auto target_slot_off = get_byteoff_of_slot(query_mem_desc_.idx_target_as_key, query_mem_desc_);
    return read_int_from_buff(rowwise_target_ptr + target_slot_off,
                              query_mem_desc_.agg_col_widths[query_mem_desc_.idx_target_as_key].compact) ==
           target_init_vals_[query_mem_desc_.idx_target_as_key];
  }
  // TODO(alex): Don't assume 64-bit keys, we could compact them as well.
  if (query_mem_desc_.output_columnar) {
    return reinterpret_cast<const int64_t*>(buff)[entry_idx] == EMPTY_KEY_64;
  }
  const auto keys_ptr = row_ptr_rowwise(buff, query_mem_desc_, entry_idx);
  return *reinterpret_cast<const int64_t*>(keys_ptr) == EMPTY_KEY_64;
}

bool ResultSetStorage::isEmptyEntry(const size_t entry_idx) const {
  return isEmptyEntry(entry_idx, buff_);
}

bool ResultSet::isNull(const SQLTypeInfo& ti, const InternalTargetValue& val) {
  if (val.isInt()) {
    if (!ti.is_fp()) {
      return val.i1 == inline_int_null_val(ti);
    }
    const auto null_val = inline_fp_null_val(ti);
    return val.i1 == *reinterpret_cast<const int64_t*>(&null_val);
  }
  if (val.isPair()) {
    CHECK(val.i2);
    return pair_to_double({val.i1, val.i2}, ti) == NULL_DOUBLE;
  }
  if (val.isStr()) {
    return false;
  }
  CHECK(val.isNull());
  return true;
}
