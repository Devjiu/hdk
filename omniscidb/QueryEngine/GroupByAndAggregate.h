#ifndef QUERYENGINE_GROUPBYANDAGGREGATE_H
#define QUERYENGINE_GROUPBYANDAGGREGATE_H

#include "GpuMemUtils.h"
#include "../Fragmenter/Fragmenter.h"
#include "../Planner/Planner.h"
#include "../Shared/sqltypes.h"
#include "RuntimeFunctions.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/noncopyable.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/variant.hpp>
#include <boost/version.hpp>
#include <glog/logging.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

#include <functional>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <vector>
#include <deque>

class Executor;

enum class GroupByColRangeType {
  OneColKnownRange,    // statically known range, only possible for column expressions
  OneColGuessedRange,  // best guess: small hash for the guess plus overflow for outliers
  MultiCol,
  MultiColPerfectHash,
  Scan,  // the plan is not a group by plan
};

// Private: each thread has its own memory, no atomic operations required
// Shared: threads in the same block share memory, atomic operations required
enum class GroupByMemSharing { Private, Shared };

struct QueryMemoryDescriptor;

typedef boost::variant<std::string, void*> NullableString;
typedef boost::variant<int64_t, double, NullableString> ScalarTargetValue;
typedef boost::variant<ScalarTargetValue, std::vector<ScalarTargetValue>> TargetValue;

struct TargetInfo {
  bool is_agg;
  SQLAgg agg_kind;
  SQLTypeInfo sql_type;
  bool skip_null_val;
  bool is_distinct;
};

enum class CountDistinctImplType { Bitmap, StdSet };

inline size_t bitmap_size_bytes(const size_t bitmap_sz) {
  size_t bitmap_byte_sz = bitmap_sz / 8;
  if (bitmap_sz % 8) {
    ++bitmap_byte_sz;
  }
  return bitmap_byte_sz;
}

struct CountDistinctDescriptor {
  const Executor* executor_;
  CountDistinctImplType impl_type_;
  int64_t min_val;
  int64_t bitmap_sz_bits;

  size_t bitmapSizeBytes() const {
    CHECK(impl_type_ == CountDistinctImplType::Bitmap);
    return bitmap_size_bytes(bitmap_sz_bits);
  }
};

typedef std::unordered_map<size_t, CountDistinctDescriptor> CountDistinctDescriptors;

class RowSetMemoryOwner;

struct QueryMemoryDescriptor {
  const Executor* executor_;
  GroupByColRangeType hash_type;
  bool keyless_hash;
  bool interleaved_bins_on_gpu;
  std::vector<int8_t> group_col_widths;
  std::vector<int8_t> agg_col_widths;
  size_t entry_count;        // the number of entries in the main buffer
  size_t entry_count_small;  // the number of entries in the small buffer
  int64_t min_val;           // meaningful for OneColKnownRange, MultiColPerfectHash only
  int64_t max_val;
  bool has_nulls;
  GroupByMemSharing sharing;  // meaningful for GPU only
  CountDistinctDescriptors count_distinct_descriptors_;
  bool sort_on_gpu_;
  bool is_sort_plan;  // TODO(alex): remove
  bool output_columnar;

  std::unique_ptr<QueryExecutionContext> getQueryExecutionContext(
      const std::vector<int64_t>& init_agg_vals,
      const Executor* executor,
      const ExecutorDeviceType device_type,
      const int device_id,
      const std::vector<std::vector<const int8_t*>>& col_buffers,
      std::shared_ptr<RowSetMemoryOwner>,
      const bool output_columnar,
      const bool sort_on_gpu) const;

  size_t getBufferSizeQuad(const ExecutorDeviceType device_type) const;
  size_t getSmallBufferSizeQuad() const;

  size_t getBufferSizeBytes(const ExecutorDeviceType device_type) const;
  size_t getSmallBufferSizeBytes() const;

  // TODO(alex): remove
  bool usesGetGroupValueFast() const;

  // TODO(alex): remove
  bool usesCachedContext() const;

  bool blocksShareMemory() const;
  bool threadsShareMemory() const;

  bool lazyInitGroups(const ExecutorDeviceType) const;

  bool interleavedBins(const ExecutorDeviceType) const;

  size_t sharedMemBytes(const ExecutorDeviceType) const;

  bool canOutputColumnar() const;

  bool sortOnGpu() const;
};

inline int64_t bitmap_set_size(const int64_t bitmap_ptr,
                               const int target_idx,
                               const CountDistinctDescriptors& count_distinct_descriptors) {
  const auto count_distinct_desc_it = count_distinct_descriptors.find(target_idx);
  CHECK(count_distinct_desc_it != count_distinct_descriptors.end());
  if (count_distinct_desc_it->second.impl_type_ != CountDistinctImplType::Bitmap) {
    CHECK(count_distinct_desc_it->second.impl_type_ == CountDistinctImplType::StdSet);
    return reinterpret_cast<std::set<int64_t>*>(bitmap_ptr)->size();
  }
  int64_t set_size{0};
  auto set_vals = reinterpret_cast<const int8_t*>(bitmap_ptr);
  for (size_t i = 0; i < count_distinct_desc_it->second.bitmapSizeBytes(); ++i) {
    for (auto bit_idx = 0; bit_idx < 8; ++bit_idx) {
      if (set_vals[i] & (1 << bit_idx)) {
        ++set_size;
      }
    }
  }
  return set_size;
}

inline void bitmap_set_unify(int8_t* lhs, int8_t* rhs, const size_t bitmap_sz) {
  for (size_t i = 0; i < bitmap_sz; ++i) {
    lhs[i] = rhs[i] = lhs[i] | rhs[i];
  }
}

typedef std::vector<int64_t> ValueTuple;

class ChunkIter;

class DictStrLiteralsOwner {
 public:
  DictStrLiteralsOwner(StringDictionary* string_dict) : string_dict_(string_dict) {}

  ~DictStrLiteralsOwner() { string_dict_->clearTransient(); }

 private:
  StringDictionary* string_dict_;
};

class RowSetMemoryOwner : boost::noncopyable {
 public:
  void setCountDistinctDescriptors(const CountDistinctDescriptors& count_distinct_descriptors) {
    if (count_distinct_descriptors_.empty()) {
      count_distinct_descriptors_ = count_distinct_descriptors;
    }
  }

  void addCountDistinctBuffer(int8_t* count_distinct_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    count_distinct_bitmaps_.push_back(count_distinct_buffer);
  }

  void addCountDistinctSet(std::set<int64_t>* count_distinct_set) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    count_distinct_sets_.push_back(count_distinct_set);
  }

  const CountDistinctDescriptors& getCountDistinctDescriptors() const { return count_distinct_descriptors_; }

  void addGroupByBuffer(int64_t* group_by_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    group_by_buffers_.push_back(group_by_buffer);
  }

  std::string* addString(const std::string& str) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    strings_.emplace_back(str);
    return &strings_.back();
  }

  std::vector<int64_t>* addArray(const std::vector<int64_t>& arr) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    arrays_.emplace_back(arr);
    return &arrays_.back();
  }

  void addStringDict(StringDictionary* str_dict) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = str_dict_owned_.find(str_dict);
    if (it != str_dict_owned_.end()) {
      return;
    }
    str_dict_owned_.insert(str_dict);
    str_dict_owners_.emplace_back(new DictStrLiteralsOwner(str_dict));
  }

  void addLiteralStringDict(std::shared_ptr<StringDictionary> lit_str_dict) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lit_str_dict_ = lit_str_dict;
  }

  ~RowSetMemoryOwner() {
    for (auto count_distinct_buffer : count_distinct_bitmaps_) {
      free(count_distinct_buffer);
    }
    for (auto count_distinct_set : count_distinct_sets_) {
      delete count_distinct_set;
    }
    for (auto group_by_buffer : group_by_buffers_) {
      free(group_by_buffer);
    }
    decltype(str_dict_owners_)().swap(str_dict_owners_);
    decltype(str_dict_owned_)().swap(str_dict_owned_);
    if (lit_str_dict_) {
      lit_str_dict_->clearTransient();
    }
  }

 private:
  CountDistinctDescriptors count_distinct_descriptors_;
  std::vector<int8_t*> count_distinct_bitmaps_;
  std::vector<std::set<int64_t>*> count_distinct_sets_;
  std::vector<int64_t*> group_by_buffers_;
  std::list<std::string> strings_;
  std::list<std::vector<int64_t>> arrays_;
  std::unordered_set<StringDictionary*> str_dict_owned_;
  std::vector<std::unique_ptr<DictStrLiteralsOwner>> str_dict_owners_;
  std::shared_ptr<StringDictionary> lit_str_dict_;
  std::mutex state_mutex_;

  friend class QueryExecutionContext;
  friend class ResultRows;
};

inline TargetInfo target_info(const Analyzer::Expr* target_expr) {
  const auto agg_expr = dynamic_cast<const Analyzer::AggExpr*>(target_expr);
  bool notnull = target_expr->get_type_info().get_notnull();
  if (!agg_expr) {
    return {false, kMIN, target_expr ? target_expr->get_type_info() : SQLTypeInfo(kBIGINT, notnull), false, false};
  }
  const auto agg_type = agg_expr->get_aggtype();
  const auto agg_arg = agg_expr->get_arg();
  if (!agg_arg) {
    CHECK_EQ(kCOUNT, agg_type);
    CHECK(!agg_expr->get_is_distinct());
    return {true, kCOUNT, SQLTypeInfo(kBIGINT, notnull), false, false};
  }
  const auto& agg_arg_ti = agg_arg->get_type_info();
  bool is_distinct{false};
  if (agg_expr->get_aggtype() == kCOUNT) {
    is_distinct = agg_expr->get_is_distinct();
  }
  bool skip_null = !agg_arg_ti.get_notnull();
  return {
      true,
      agg_expr->get_aggtype(),
      agg_type == kAVG ? agg_arg_ti : (agg_type == kCOUNT ? SQLTypeInfo(kBIGINT, notnull) : agg_expr->get_type_info()),
      skip_null,
      is_distinct};
}

namespace {

inline int64_t inline_int_null_val(const SQLTypeInfo& ti) {
  auto type = ti.is_decimal() ? decimal_to_int_type(ti) : ti.get_type();
  if (ti.is_string()) {
    CHECK_EQ(kENCODING_DICT, ti.get_compression());
    CHECK_EQ(4, ti.get_size());
    type = kINT;
  }
  switch (type) {
    case kBOOLEAN:
      return std::numeric_limits<int8_t>::min();
    case kSMALLINT:
      return std::numeric_limits<int16_t>::min();
    case kINT:
      return std::numeric_limits<int32_t>::min();
    case kBIGINT:
      return std::numeric_limits<int64_t>::min();
    case kTIMESTAMP:
    case kTIME:
    case kDATE:
      return std::numeric_limits<int64_t>::min();
    default:
      CHECK(false);
  }
}

inline double inline_fp_null_val(const SQLTypeInfo& ti) {
  CHECK(ti.is_fp());
  const auto type = ti.get_type();
  switch (type) {
    case kFLOAT:
      return NULL_FLOAT;
    case kDOUBLE:
      return NULL_DOUBLE;
    default:
      CHECK(false);
  }
}

}  // namespace

struct InternalTargetValue {
  int64_t i1;
  int64_t i2;

  enum class ITVType { Int, Pair, Str, Arr, Null };

  ITVType ty;

  explicit InternalTargetValue(const int64_t i1_) : i1(i1_), ty(ITVType::Int) {}

  explicit InternalTargetValue(const int64_t i1_, const int64_t i2_) : i1(i1_), i2(i2_), ty(ITVType::Pair) {}

  explicit InternalTargetValue(const std::string* s) : i1(reinterpret_cast<int64_t>(s)), ty(ITVType::Str) {}

  explicit InternalTargetValue(const std::vector<int64_t>* v) : i1(reinterpret_cast<int64_t>(v)), ty(ITVType::Arr) {}

  explicit InternalTargetValue() : ty(ITVType::Null) {}

  std::string strVal() const { return *reinterpret_cast<std::string*>(i1); }

  std::vector<int64_t> arrVal() const { return *reinterpret_cast<std::vector<int64_t>*>(i1); }

  bool isInt() const { return ty == ITVType::Int; }

  bool isPair() const { return ty == ITVType::Pair; }

  bool isNull() const { return ty == ITVType::Null; }

  bool isStr() const { return ty == ITVType::Str; }

  bool operator<(const InternalTargetValue& other) const {
    switch (ty) {
      case ITVType::Int:
        CHECK(other.ty == ITVType::Int);
        return i1 < other.i1;
      case ITVType::Pair:
        CHECK(other.ty == ITVType::Pair);
        if (i1 != other.i1) {
          return i1 < other.i1;
        }
        return i2 < other.i2;
      case ITVType::Str:
        CHECK(other.ty == ITVType::Str);
        return strVal() < other.strVal();
      case ITVType::Null:
        return false;
      default:
        CHECK(false);
    }
  }

  bool operator==(const InternalTargetValue& other) const { return !(*this < other || other < *this); }
};

class InternalRow {
 public:
  InternalRow(RowSetMemoryOwner* row_set_mem_owner) : row_set_mem_owner_(row_set_mem_owner){};

  bool operator==(const InternalRow& other) const { return row_ == other.row_; }

  bool operator<(const InternalRow& other) const { return row_ < other.row_; }

  InternalTargetValue& operator[](const size_t i) { return row_[i]; }

  const InternalTargetValue& operator[](const size_t i) const { return row_[i]; }

  size_t size() const { return row_.size(); }

 private:
  void reserve(const size_t n) { row_.reserve(n); }

  void addValue(const int64_t val) { row_.emplace_back(val); }

  // used for kAVG
  void addValue(const int64_t val1, const int64_t val2) { row_.emplace_back(val1, val2); }

  void addValue(const std::string& val) { row_.emplace_back(row_set_mem_owner_->addString(val)); }

  void addValue(const std::vector<int64_t>& val) { row_.emplace_back(row_set_mem_owner_->addArray(val)); }

  void addValue() { row_.emplace_back(); }

  std::vector<InternalTargetValue> row_;
  RowSetMemoryOwner* row_set_mem_owner_;

  friend class RowStorage;
};

class RowStorage {
 private:
  size_t size() const { return rows_.size(); }

  void clear() { rows_.clear(); }

  void reserve(const size_t n) { rows_.reserve(n); }

  void beginRow(RowSetMemoryOwner* row_set_mem_owner) { rows_.emplace_back(row_set_mem_owner); }

  void reserveRow(const size_t n) { rows_.back().reserve(n); }

  void discardRow() { rows_.pop_back(); }

  void addValue(const int64_t val) { rows_.back().addValue(val); }

  // used for kAVG
  void addValue(const int64_t val1, const int64_t val2) { rows_.back().addValue(val1, val2); }

  void addValue(const std::string& val) { rows_.back().addValue(val); }

  void addValue(const std::vector<int64_t>& val) { rows_.back().addValue(val); }

  void addValue() { rows_.back().addValue(); }

  void push_back(const InternalRow& v) { rows_.push_back(v); }

  void append(const RowStorage& other) { rows_.insert(rows_.end(), other.rows_.begin(), other.rows_.end()); }

  void truncate(const size_t n) { rows_.erase(rows_.begin() + n, rows_.end()); }

  void drop(const size_t n) {
    if (n >= rows_.size()) {
      decltype(rows_)().swap(rows_);
      return;
    }
    decltype(rows_)(rows_.begin() + n, rows_.end()).swap(rows_);
  }

  InternalRow& operator[](const size_t i) { return rows_[i]; }

  const InternalRow& operator[](const size_t i) const { return rows_[i]; }

  InternalRow& front() { return rows_.front(); }

  const InternalRow& front() const { return rows_.front(); }

  const InternalRow& back() const { return rows_.back(); }

  void top(const int64_t n, const std::function<bool(const InternalRow& lhs, const InternalRow& rhs)> compare) {
    std::make_heap(rows_.begin(), rows_.end(), compare);
    decltype(rows_) top_target_values;
    top_target_values.reserve(n);
    for (int64_t i = 0; i < n && !rows_.empty(); ++i) {
      top_target_values.push_back(rows_.front());
      std::pop_heap(rows_.begin(), rows_.end(), compare);
      rows_.pop_back();
    }
    rows_.swap(top_target_values);
  }

  void sort(const std::function<bool(const InternalRow& lhs, const InternalRow& rhs)> compare) {
    std::sort(rows_.begin(), rows_.end(), compare);
  }

  void removeDuplicates() {
    std::sort(rows_.begin(), rows_.end());
    rows_.erase(std::unique(rows_.begin(), rows_.end()), rows_.end());
  }

  std::vector<InternalRow> rows_;

  friend class ResultRows;
};

class ReductionRanOutOfSlots : public std::runtime_error {
 public:
  ReductionRanOutOfSlots() : std::runtime_error("ReductionRanOutOfSlots") {}
};

class ResultRows {
 public:
  ResultRows(const std::vector<Analyzer::Expr*>& targets,
             const Executor* executor,
             const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
             const ExecutorDeviceType device_type,
             int64_t* group_by_buffer = nullptr,
             const int32_t groups_buffer_entry_count = 0,
             const int64_t min_val = 0,
             const int8_t warp_count = 0)
      : executor_(executor),
        query_mem_desc_{},
        row_set_mem_owner_(row_set_mem_owner),
        group_by_buffer_(group_by_buffer),
        groups_buffer_entry_count_(groups_buffer_entry_count),
        group_by_buffer_idx_(0),
        min_val_(min_val),
        warp_count_(warp_count),
        output_columnar_(false),
        in_place_(false),
        device_type_(device_type),
        device_id_(-1),
        crt_row_idx_(0),
        crt_row_buff_idx_(0),
        drop_first_(0),
        keep_first_(0),
        fetch_started_(false),
        in_place_buff_idx_(0),
        just_explain_(false) {
    for (const auto target_expr : targets) {
      const auto agg_info = target_info(target_expr);
      targets_.push_back(agg_info);
    }
  }

  ResultRows(const QueryMemoryDescriptor& query_mem_desc,
             const std::vector<Analyzer::Expr*>& targets,
             const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
             int64_t* group_by_buffer,
             const size_t groups_buffer_entry_count,
             const bool output_columnar,
             const std::vector<std::vector<const int8_t*>>& col_buffers,
             const ExecutorDeviceType device_type,
             const int device_id);

  explicit ResultRows(const std::string& explanation)
      : query_mem_desc_{},
        group_by_buffer_idx_(0),
        output_columnar_(false),
        in_place_(false),
        device_type_(ExecutorDeviceType::Hybrid),
        device_id_(-1),
        crt_row_idx_(0),
        crt_row_buff_idx_(0),
        drop_first_(0),
        keep_first_(0),
        fetch_started_(false),
        in_place_buff_idx_(0),
        just_explain_(true),
        explanation_(explanation) {}

  void moveToBegin() const {
    crt_row_idx_ = 0;
    crt_row_buff_idx_ = 0;
    in_place_buff_idx_ = 0;
    group_by_buffer_idx_ = 0;
    fetch_started_ = false;
  }
  void beginRow() { target_values_.beginRow(row_set_mem_owner_.get()); }

  void beginRow(const int64_t key) {
    CHECK(multi_keys_.empty());
    simple_keys_.push_back(key);
    target_values_.beginRow(row_set_mem_owner_.get());
  }

  void beginRow(const std::vector<int64_t>& key) {
    CHECK(simple_keys_.empty());
    multi_keys_.push_back(key);
    target_values_.beginRow(row_set_mem_owner_.get());
  }

  void addKeylessGroupByBuffer(const int64_t* group_by_buffer,
                               const int32_t groups_buffer_entry_count,
                               const int64_t min_val,
                               const int8_t warp_count,
                               const bool is_columnar);

  void addValue(const int64_t val) { target_values_.addValue(val); }

  // used for kAVG
  void addValue(const int64_t val1, const int64_t val2) { target_values_.addValue(val1, val2); }

  void addValue(const std::string& val) { target_values_.addValue(val); }

  void addValue(const std::vector<int64_t>& val) { target_values_.addValue(val); }

  void addValue() { target_values_.addValue(); }

  void append(const ResultRows& more_results) {
    simple_keys_.insert(simple_keys_.end(), more_results.simple_keys_.begin(), more_results.simple_keys_.end());
    multi_keys_.insert(multi_keys_.end(), more_results.multi_keys_.begin(), more_results.multi_keys_.end());
    target_values_.append(more_results.target_values_);
    if (in_place_) {
      in_place_group_by_buffers_.insert(in_place_group_by_buffers_.end(),
                                        more_results.in_place_group_by_buffers_.begin(),
                                        more_results.in_place_group_by_buffers_.end());
      in_place_groups_by_buffers_entry_count_.insert(in_place_groups_by_buffers_entry_count_.end(),
                                                     more_results.in_place_groups_by_buffers_entry_count_.begin(),
                                                     more_results.in_place_groups_by_buffers_entry_count_.end());
    }
  }

  void reduce(const ResultRows& other_results, const QueryMemoryDescriptor& query_mem_desc, const bool output_columnar);

  void sort(const Planner::Sort* sort_plan, const int64_t top_n);

  void keepFirstN(const size_t n) {
    CHECK(n);
    if (in_place_ || group_by_buffer_) {
      keep_first_ = n;
      return;
    }
    if (n >= rowCount()) {
      return;
    }
    target_values_.truncate(n);
  }

  void dropFirstN(const size_t n) {
    if (in_place_ || group_by_buffer_) {
      drop_first_ = n;
      return;
    }
    if (!n) {
      return;
    }
    target_values_.drop(n);
  }

  size_t rowCount() const {
    if (in_place_ || group_by_buffer_) {
      moveToBegin();
      size_t row_count{0};
      while (true) {
        auto crt_row = getNextRow(false, false);
        if (crt_row.empty()) {
          break;
        }
        ++row_count;
      }
      moveToBegin();
      return row_count;
    }
    return just_explain_ ? 1 : target_values_.size();
  }

  size_t colCount() const { return just_explain_ ? 1 : targets_.size(); }

  bool definitelyHasNoRows() const {
    if (in_place_) {
      return in_place_group_by_buffers_.empty();
    }
    return !group_by_buffer_ && !just_explain_ && !rowCount();
  }

  static bool isNull(const SQLTypeInfo& ti, const InternalTargetValue& val);

  TargetValue getRowAt(const size_t row_idx,
                       const size_t col_idx,
                       const bool translate_strings,
                       const bool decimal_to_double = true) const;

  std::vector<TargetValue> getNextRow(const bool translate_strings, const bool decimal_to_double) const;

  SQLTypeInfo getColType(const size_t col_idx) const {
    if (just_explain_) {
      return SQLTypeInfo(kTEXT, false);
    }
    return targets_[col_idx].agg_kind == kAVG ? SQLTypeInfo(kDOUBLE, false) : targets_[col_idx].sql_type;
  }

 private:
  bool fetchLazyOrBuildRow(std::vector<TargetValue>& row,
                           const std::vector<std::vector<const int8_t*>>& col_buffers,
                           const std::vector<Analyzer::Expr*>& targets,
                           const bool translate_strings,
                           const bool decimal_to_double,
                           const bool fetch_lazy) const;

  void addValues(const std::vector<int64_t>& vals) {
    target_values_.reserveRow(vals.size());
    for (const auto val : vals) {
      target_values_.addValue(val);
    }
  }

  void discardRow() {
    CHECK_NE(simple_keys_.empty(), multi_keys_.empty());
    if (!simple_keys_.empty()) {
      simple_keys_.pop_back();
    } else {
      multi_keys_.pop_back();
    }
    target_values_.discardRow();
  }

  void createReductionMap() const {
    if (!as_map_.empty() || !as_unordered_map_.empty()) {
      return;
    }
    CHECK_NE(simple_keys_.empty(), multi_keys_.empty());
    for (size_t i = 0; i < simple_keys_.size(); ++i) {
      as_unordered_map_.insert(std::make_pair(simple_keys_[i], target_values_[i]));
    }
    for (size_t i = 0; i < multi_keys_.size(); ++i) {
      as_map_.insert(std::make_pair(multi_keys_[i], target_values_[i]));
    }
  }

  std::vector<TargetInfo> targets_;
  std::vector<int64_t> simple_keys_;
  typedef std::vector<int64_t> MultiKey;
  std::vector<MultiKey> multi_keys_;
  RowStorage target_values_;
  mutable std::map<MultiKey, InternalRow> as_map_;
  mutable std::unordered_map<int64_t, InternalRow> as_unordered_map_;
  const Executor* executor_;
  QueryMemoryDescriptor query_mem_desc_;
  std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner_;

  int64_t* group_by_buffer_;
  int32_t groups_buffer_entry_count_;
  mutable size_t group_by_buffer_idx_;
  int64_t min_val_;
  int8_t warp_count_;
  bool output_columnar_;
  bool in_place_;
  ExecutorDeviceType device_type_;
  int device_id_;
  mutable size_t crt_row_idx_;
  mutable size_t crt_row_buff_idx_;
  size_t drop_first_;
  size_t keep_first_;
  mutable bool fetch_started_;
  mutable size_t in_place_buff_idx_;
  std::vector<int32_t> in_place_groups_by_buffers_entry_count_;
  std::vector<int64_t*> in_place_group_by_buffers_;
  bool just_explain_;
  std::string explanation_;
  std::unordered_set<int64_t> unkown_top_keys_;

  friend class Executor;
  friend class QueryExecutionContext;
};

namespace {

inline std::string nullable_str_to_string(const NullableString& str) {
  auto nptr = boost::get<void*>(&str);
  if (nptr) {
    CHECK(!*nptr);
    return "NULL";
  }
  auto sptr = boost::get<std::string>(&str);
  CHECK(sptr);
  return *sptr;
}

inline std::string datum_to_string(const TargetValue& tv, const SQLTypeInfo& ti, const std::string& delim) {
  if (ti.is_array()) {
    const auto list_tv = boost::get<std::vector<ScalarTargetValue>>(&tv);
    CHECK(list_tv);
    std::vector<std::string> elem_strs;
    elem_strs.reserve(list_tv->size());
    const auto& elem_ti = ti.get_elem_type();
    for (const auto& elem_tv : *list_tv) {
      elem_strs.push_back(datum_to_string(elem_tv, elem_ti, delim));
    }
    return "{" + boost::algorithm::join(elem_strs, delim) + "}";
  }
  const auto scalar_tv = boost::get<ScalarTargetValue>(&tv);
  if (ti.is_time()) {
    Datum datum;
    datum.timeval = *boost::get<int64_t>(scalar_tv);
    if (datum.timeval == NULL_BIGINT) {
      return "NULL";
    }
    return DatumToString(datum, ti);
  }
  if (ti.is_boolean()) {
    const auto bool_val = *boost::get<int64_t>(scalar_tv);
    return bool_val == NULL_BOOLEAN ? "NULL" : (bool_val ? "true" : "false");
  }
  auto iptr = boost::get<int64_t>(scalar_tv);
  if (iptr) {
    return *iptr == inline_int_null_val(ti) ? "NULL" : std::to_string(*iptr);
  }
  auto dptr = boost::get<double>(scalar_tv);
  if (dptr) {
    return *dptr == inline_fp_null_val(ti.is_decimal() ? SQLTypeInfo(kDOUBLE, false) : ti) ? "NULL"
                                                                                           : std::to_string(*dptr);
  }
  auto sptr = boost::get<NullableString>(scalar_tv);
  CHECK(sptr);
  return nullable_str_to_string(*sptr);
}

class ScopedScratchBuffer {
 public:
  ScopedScratchBuffer(const size_t num_bytes, Data_Namespace::DataMgr* data_mgr, const int device_id)
      : data_mgr_(data_mgr), ab_(alloc_gpu_abstract_buffer(data_mgr_, num_bytes, device_id)) {}
  ~ScopedScratchBuffer() { free_gpu_abstract_buffer(data_mgr_, ab_); }
  CUdeviceptr getPtr() const { return reinterpret_cast<CUdeviceptr>(ab_->getMemoryPtr()); }

 private:
  Data_Namespace::DataMgr* data_mgr_;
  Data_Namespace::AbstractBuffer* ab_;
};

}  // namespace

inline std::string row_col_to_string(const ResultRows& rows,
                                     const size_t row_idx,
                                     const size_t i,
                                     const std::string& delim = ", ") {
  const auto tv = rows.getRowAt(row_idx, i, true);
  const auto ti = rows.getColType(i);
  return datum_to_string(tv, ti, delim);
}

inline std::string row_col_to_string(const std::vector<TargetValue>& row,
                                     const size_t i,
                                     const SQLTypeInfo& ti,
                                     const std::string& delim = ", ") {
  return datum_to_string(row[i], ti, delim);
}

class QueryExecutionContext : boost::noncopyable {
 public:
  // TODO(alex): move init_agg_vals to GroupByBufferDescriptor, remove device_type
  QueryExecutionContext(const QueryMemoryDescriptor&,
                        const std::vector<int64_t>& init_agg_vals,
                        const Executor* executor,
                        const ExecutorDeviceType device_type,
                        const int device_id,
                        const std::vector<std::vector<const int8_t*>>& col_buffers,
                        std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                        const bool output_columnar,
                        const bool sort_on_gpu);

  // TOOD(alex): get rid of targets parameter
  ResultRows getRowSet(const std::vector<Analyzer::Expr*>& targets,
                       const QueryMemoryDescriptor& query_mem_desc,
                       const bool was_auto_device) const noexcept;
  ResultRows groupBufferToResults(const size_t i,
                                  const std::vector<Analyzer::Expr*>& targets,
                                  const bool was_auto_device) const;

  std::vector<int64_t*> launchGpuCode(const std::vector<void*>& cu_functions,
                                      const bool hoist_literals,
                                      const std::vector<int8_t>& literal_buff,
                                      std::vector<std::vector<const int8_t*>> col_buffers,
                                      const std::vector<int64_t>& num_rows,
                                      const int64_t scan_limit,
                                      const std::vector<int64_t>& init_agg_vals,
                                      Data_Namespace::DataMgr* data_mgr,
                                      const unsigned block_size_x,
                                      const unsigned grid_size_x,
                                      const int device_id,
                                      int32_t* error_code) const;

 private:
  void outputBin(ResultRows& results,
                 const std::vector<Analyzer::Expr*>& targets,
                 int64_t* group_by_buffer,
                 const size_t groups_buffer_entry_count,
                 const size_t bin) const;

  void initGroups(int64_t* groups_buffer,
                  const int64_t* init_vals,
                  const int32_t groups_buffer_entry_count,
                  const bool keyless,
                  const size_t warp_size);

  void initColumnarGroups(int64_t* groups_buffer,
                          const int64_t* init_vals,
                          const int32_t groups_buffer_entry_count,
                          const bool keyless);

  std::vector<ssize_t> allocateCountDistinctBuffers(const bool deferred);
  int64_t allocateCountDistinctBitmap(const size_t bitmap_sz);
  int64_t allocateCountDistinctSet();

  const QueryMemoryDescriptor& query_mem_desc_;
  std::vector<int64_t> init_agg_vals_;
  const Executor* executor_;
  const ExecutorDeviceType device_type_;
  const int device_id_;
  const std::vector<std::vector<const int8_t*>>& col_buffers_;
  const size_t num_buffers_;

  std::vector<int64_t*> group_by_buffers_;
  std::vector<int64_t*> small_group_by_buffers_;
  std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner_;
  const bool output_columnar_;
  const bool sort_on_gpu_;

  friend class Executor;
  friend class ResultRows;
  friend void copy_group_by_buffers_from_gpu(Data_Namespace::DataMgr* data_mgr,
                                             const QueryExecutionContext* query_exe_context,
                                             const GpuQueryMemory& gpu_query_mem,
                                             const unsigned block_size_x,
                                             const unsigned grid_size_x,
                                             const int device_id,
                                             const bool prepend_index_buffer);
};

class GroupByAndAggregate {
 public:
  GroupByAndAggregate(Executor* executor,
                      const ExecutorDeviceType device_type,
                      const Planner::Plan* plan,
                      const Fragmenter_Namespace::QueryInfo& query_info,
                      std::shared_ptr<RowSetMemoryOwner>,
                      const size_t max_groups_buffer_entry_count,
                      const int64_t scan_limit,
                      const bool allow_multifrag,
                      const Planner::Sort* sort_plan,
                      const bool output_columnar_hint);

  QueryMemoryDescriptor getQueryMemoryDescriptor() const;

  bool outputColumnar() const;

  bool gpuCanHandleOrderEntries(const Planner::Sort* sort_plan);

  // returns true iff checking the error code after every row
  // is required -- slow path group by queries for now
  bool codegen(llvm::Value* filter_result, const ExecutorDeviceType, const bool hoist_literals);

 private:
  struct ColRangeInfo {
    const GroupByColRangeType hash_type_;
    const int64_t min;
    const int64_t max;
    const bool has_nulls;
  };

  struct DiamondCodegen {
    DiamondCodegen(llvm::Value* cond,
                   Executor* executor,
                   const bool chain_to_next,
                   const std::string& label_prefix,
                   DiamondCodegen* parent = nullptr);
    void setChainToNext();
    void setFalseTarget(llvm::BasicBlock* cond_false);
    ~DiamondCodegen();

    Executor* executor_;
    llvm::BasicBlock* cond_true_;
    llvm::BasicBlock* cond_false_;
    llvm::BasicBlock* orig_cond_false_;
    bool chain_to_next_;
    DiamondCodegen* parent_;
  };

  void initQueryMemoryDescriptor(const size_t, const bool sort_on_gpu_hint);

  llvm::Value* codegenGroupBy(const QueryMemoryDescriptor&,
                              const ExecutorDeviceType,
                              const bool hoist_literals,
                              DiamondCodegen&);

  llvm::Function* codegenPerfectHashFunction();

  GroupByAndAggregate::ColRangeInfo getColRangeInfo(const std::deque<Fragmenter_Namespace::FragmentInfo>&);

  GroupByAndAggregate::ColRangeInfo getExprRangeInfo(const Analyzer::Expr* expr,
                                                     const std::deque<Fragmenter_Namespace::FragmentInfo>& fragments);

  void codegenAggCalls(llvm::Value* agg_out_start_ptr,
                       const std::vector<llvm::Value*>& agg_out_vec,
                       const QueryMemoryDescriptor&,
                       const ExecutorDeviceType,
                       const bool hoist_literals);

  uint32_t aggColumnarOff(const uint32_t agg_out_off, const QueryMemoryDescriptor& query_mem_desc);

  void codegenCountDistinct(const size_t target_idx,
                            const Analyzer::Expr* target_expr,
                            std::vector<llvm::Value*>& agg_args,
                            const QueryMemoryDescriptor&,
                            const ExecutorDeviceType);

  std::vector<llvm::Value*> codegenAggArg(const Analyzer::Expr* target_expr, const bool hoist_literals);

  llvm::Value* emitCall(const std::string& fname, const std::vector<llvm::Value*>& args);

  QueryMemoryDescriptor query_mem_desc_;
  Executor* executor_;
  const Planner::Plan* plan_;
  const Fragmenter_Namespace::QueryInfo& query_info_;
  std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner_;
  const int64_t scan_limit_;
  bool output_columnar_;

  friend class Executor;
};

namespace {

inline size_t get_bit_width(const SQLTypeInfo& ti) {
  const auto int_type = ti.is_decimal() ? decimal_to_int_type(ti) : ti.get_type();
  switch (int_type) {
    case kBOOLEAN:
      return 8;
    case kSMALLINT:
      return 16;
    case kINT:
      return 32;
    case kBIGINT:
      return 64;
    case kFLOAT:
      return 32;
    case kDOUBLE:
      return 64;
    case kTIME:
    case kTIMESTAMP:
    case kDATE:
      return sizeof(time_t) * 8;
    case kTEXT:
    case kVARCHAR:
    case kCHAR:
      return 32;
    case kARRAY:
      throw std::runtime_error("Projecting on array columns not supported yet.");
    default:
      CHECK(false);
  }
}

inline uint64_t exp_to_scale(const unsigned exp) {
  uint64_t res = 1;
  for (unsigned i = 0; i < exp; ++i) {
    res *= 10;
  }
  return res;
}

inline std::vector<Analyzer::Expr*> get_agg_target_exprs(const Planner::Plan* plan) {
  const auto& target_list = plan->get_targetlist();
  std::vector<Analyzer::Expr*> result;
  for (auto target : target_list) {
    auto target_expr = target->get_expr();
    CHECK(target_expr);
    result.push_back(target_expr);
  }
  return result;
}

inline int64_t extract_from_datum(const Datum datum, const SQLTypeInfo& ti) {
  const auto type = ti.is_decimal() ? decimal_to_int_type(ti) : ti.get_type();
  switch (type) {
    case kBOOLEAN:
      return datum.tinyintval;
    case kSMALLINT:
      return datum.smallintval;
    case kCHAR:
    case kVARCHAR:
    case kTEXT:
      CHECK_EQ(kENCODING_DICT, ti.get_compression());
    case kINT:
      return datum.intval;
    case kBIGINT:
      return datum.bigintval;
    case kTIME:
    case kTIMESTAMP:
    case kDATE:
      return datum.timeval;
    default:
      CHECK(false);
  }
}

inline int64_t extract_min_stat(const ChunkStats& stats, const SQLTypeInfo& ti) {
  return extract_from_datum(stats.min, ti);
}

inline int64_t extract_max_stat(const ChunkStats& stats, const SQLTypeInfo& ti) {
  return extract_from_datum(stats.max, ti);
}

}  // namespace

template <class T>
inline std::vector<int8_t> get_col_byte_widths(const T& col_expr_list) {
  std::vector<int8_t> col_widths;
  for (const auto col_expr : col_expr_list) {
    if (!col_expr) {
      // row index
      col_widths.push_back(sizeof(int64_t));
    } else {
      const auto agg_info = target_info(col_expr);
      if ((agg_info.sql_type.is_string() && agg_info.sql_type.get_compression() == kENCODING_NONE) ||
          agg_info.sql_type.is_array()) {
        col_widths.push_back(sizeof(int64_t));
        col_widths.push_back(sizeof(int64_t));
        continue;
      }
      const auto col_expr_bitwidth = get_bit_width(agg_info.sql_type);
      CHECK_EQ(0, col_expr_bitwidth % 8);
      col_widths.push_back(col_expr_bitwidth / 8);
      // for average, we'll need to keep the count as well
      if (agg_info.agg_kind == kAVG) {
        CHECK(agg_info.is_agg);
        col_widths.push_back(sizeof(int64_t));
      }
    }
  }
  return col_widths;
}

namespace {

inline size_t key_columnar_off(const size_t group_by_col_count,
                               const size_t bin,
                               const size_t agg_col_count,
                               const bool output_columnar) {
  return (output_columnar ? 1 : (group_by_col_count + agg_col_count)) * bin;
}

inline uint32_t agg_columnar_off(const size_t group_by_col_count,
                                 const size_t out_vec_idx,
                                 const size_t groups_buffer_entry_count,
                                 const bool output_columnar) {
  return (out_vec_idx + group_by_col_count) * (output_columnar ? groups_buffer_entry_count : 1);
}

}  // namespace

#endif  // QUERYENGINE_GROUPBYANDAGGREGATE_H
