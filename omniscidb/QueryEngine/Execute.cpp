#include "Execute.h"

#include "Codec.h"
#include "GpuMemUtils.h"
#include "GroupByAndAggregate.h"
#include "NvidiaKernel.h"
#include "Fragmenter/Fragmenter.h"
#include "Chunk/Chunk.h"
#include "QueryTemplateGenerator.h"
#include "RuntimeFunctions.h"
#include "CudaMgr/CudaMgr.h"
#include "Import/CsvImport.h"

#include <boost/range/adaptor/reversed.hpp>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Instrumentation.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <nvvm.h>

#include <algorithm>
#include <numeric>
#include <thread>
#include <unistd.h>
#include <map>
#include <set>
#include <boost/filesystem/path.hpp>


TargetValue ResultRows::get(const size_t row_idx,
                            const size_t col_idx,
                            const bool translate_strings) const {
  CHECK_GE(row_idx, 0);
  CHECK_LT(row_idx, target_values_.size());
  CHECK_GE(col_idx, 0);
  CHECK_LT(col_idx, targets_.size());
  const auto agg_info = targets_[col_idx];
  if (agg_info.agg_kind == kAVG) {
    CHECK(!targets_[col_idx].sql_type.is_string());
    const auto& row_vals = target_values_[row_idx];
    CHECK_LT(col_idx, row_vals.size());
    const auto pair_p = boost::get<std::pair<int64_t, int64_t>>(&(row_vals[col_idx]));
    CHECK(pair_p);
    return pair_to_double(*pair_p, targets_[col_idx].sql_type.is_integer());
  }
  if (targets_[col_idx].sql_type.is_integer() ||
      targets_[col_idx].sql_type.is_boolean() ||
      targets_[col_idx].sql_type.is_time()) {
    if (agg_info.is_distinct) {
      return TargetValue(bitmap_set_size(*boost::get<int64_t>(&target_values_[row_idx][col_idx]), col_idx,
        row_set_mem_owner_->count_distinct_descriptors_));
    }
    CHECK_LT(col_idx, target_values_[row_idx].size());
    const auto v = target_values_[row_idx][col_idx];
    const auto pi = boost::get<int64_t>(&v);
    CHECK(pi);
    return *pi;
  } else if (targets_[col_idx].sql_type.is_string()) {
    if (targets_[col_idx].sql_type.get_compression() == kENCODING_DICT) {
      const int dict_id = targets_[col_idx].sql_type.get_comp_param();
      return translate_strings
        ? TargetValue(executor_->getStringDictionary(dict_id)->getString(
            *boost::get<int64_t>(&target_values_[row_idx][col_idx])))
        : TargetValue(*boost::get<int64_t>(&target_values_[row_idx][col_idx]));
    } else {
      CHECK_EQ(kENCODING_NONE, targets_[col_idx].sql_type.get_compression());
      return TargetValue(*boost::get<std::string>(&target_values_[row_idx][col_idx]));
    }
  } else {
    CHECK(targets_[col_idx].sql_type.is_fp());
    return TargetValue(*reinterpret_cast<const double*>(
      boost::get<int64_t>(&target_values_[row_idx][col_idx])));
  }
  CHECK(false);
}

namespace {

int64_t inline_int_null_val(const SQLTypeInfo& ti) {
  auto type = ti.get_type();
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

}  // namespace

void ResultRows::reduce(const ResultRows& other_results) {
  if (other_results.empty()) {
    return;
  }
  if (empty()) {
    *this = other_results;
    return;
  }

  auto reduce_impl = [this](InternalTargetValue* crt_val, const InternalTargetValue* new_val,
      const TargetInfo& agg_info, const size_t agg_col_idx) {
    CHECK(agg_info.sql_type.is_integer() || agg_info.sql_type.is_time() ||
          agg_info.sql_type.get_type() == kBOOLEAN || agg_info.sql_type.is_string() ||
          agg_info.sql_type.get_type() == kFLOAT || agg_info.sql_type.get_type() == kDOUBLE);
    switch (agg_info.agg_kind) {
    case kSUM:
    case kCOUNT:
    case kAVG:
      if (agg_info.is_distinct) {
        CHECK(agg_info.is_agg);
        CHECK_EQ(kCOUNT, agg_info.agg_kind);
        auto count_distinct_desc_it = row_set_mem_owner_->count_distinct_descriptors_.find(agg_col_idx);
        CHECK(count_distinct_desc_it != row_set_mem_owner_->count_distinct_descriptors_.end());
        if (count_distinct_desc_it->second.impl_type_ == CountDistinctImplType::Bitmap) {
          auto old_set = reinterpret_cast<int8_t*>(*boost::get<int64_t>(crt_val));
          auto new_set = reinterpret_cast<int8_t*>(*boost::get<int64_t>(new_val));
          bitmap_set_unify(new_set, old_set, count_distinct_desc_it->second.bitmapSizeBytes());
        } else {
          CHECK(count_distinct_desc_it->second.impl_type_ == CountDistinctImplType::StdSet);
          auto old_set = reinterpret_cast<std::set<int64_t>*>(*boost::get<int64_t>(crt_val));
          auto new_set = reinterpret_cast<std::set<int64_t>*>(*boost::get<int64_t>(new_val));
          old_set->insert(new_set->begin(), new_set->end());
          new_set->insert(old_set->begin(), old_set->end());
        }
        break;
      }
      if (agg_info.agg_kind == kAVG) {
        auto old_avg_p = boost::get<std::pair<int64_t, int64_t>>(crt_val);
        CHECK(old_avg_p);
        auto new_avg_p = boost::get<std::pair<int64_t, int64_t>>(new_val);
        CHECK(new_avg_p);
        if (agg_info.sql_type.is_fp()) {
          agg_sum_double(
            &old_avg_p->first,
            *reinterpret_cast<const double*>(&new_avg_p->first));
        } else {
          agg_sum(&old_avg_p->first, new_avg_p->first);
        }
        agg_sum(&old_avg_p->second, new_avg_p->second);
        break;
      }
      if (agg_info.sql_type.is_integer() || agg_info.sql_type.is_time()) {
        agg_sum(
          boost::get<int64_t>(crt_val),
          *boost::get<int64_t>(new_val));
      } else {
        agg_sum_double(
          boost::get<int64_t>(crt_val),
          *reinterpret_cast<const double*>(boost::get<int64_t>(new_val)));
      }
      break;
    case kMIN:
      if (agg_info.sql_type.is_integer() || agg_info.sql_type.is_time() || agg_info.sql_type.is_boolean()) {
        if (agg_info.skip_null_val) {
          agg_min_skip_val(
            boost::get<int64_t>(crt_val),
            *boost::get<int64_t>(new_val),
            inline_int_null_val(agg_info.sql_type));
        } else {
          agg_min(boost::get<int64_t>(crt_val), *boost::get<int64_t>(new_val));
        }
      } else {
        agg_min_double(
          boost::get<int64_t>(crt_val),
          *reinterpret_cast<const double*>(boost::get<int64_t>(new_val)));
      }
      break;
    case kMAX:
      if (agg_info.sql_type.is_integer() || agg_info.sql_type.is_time()) {
        if (agg_info.skip_null_val) {
          agg_max_skip_val(
            boost::get<int64_t>(crt_val),
            *boost::get<int64_t>(new_val),
            inline_int_null_val(agg_info.sql_type));
        } else {
          agg_max(boost::get<int64_t>(crt_val), *boost::get<int64_t>(new_val));
        }
      } else {
        agg_max_double(
          boost::get<int64_t>(crt_val),
          *reinterpret_cast<const double*>(boost::get<int64_t>(new_val)));
      }
      break;
    default:
      CHECK(false);
    }
  };

  if (simple_keys_.empty() && multi_keys_.empty()) {
    CHECK_EQ(1, size());
    CHECK_EQ(1, other_results.size());
    auto& crt_results = target_values_.front();
    const auto& new_results = other_results.target_values_.front();
    for (size_t agg_col_idx = 0; agg_col_idx < colCount(); ++agg_col_idx) {
      const auto agg_info = targets_[agg_col_idx];
      reduce_impl(&crt_results[agg_col_idx], &new_results[agg_col_idx],
        agg_info, agg_col_idx);
    }
    return;
  }

  CHECK_NE(simple_keys_.empty(), multi_keys_.empty());
  createReductionMap();
  other_results.createReductionMap();
  for (const auto& kv : other_results.as_map_) {
    auto it = as_map_.find(kv.first);
    if (it == as_map_.end()) {
      as_map_.insert(std::make_pair(kv.first, kv.second));
      continue;
    }
    auto& old_agg_results = it->second;
    CHECK_EQ(old_agg_results.size(), kv.second.size());
    const size_t agg_col_count = old_agg_results.size();
    for (size_t agg_col_idx = 0; agg_col_idx < agg_col_count; ++agg_col_idx) {
      const auto agg_info = targets_[agg_col_idx];
      reduce_impl(&old_agg_results[agg_col_idx], &kv.second[agg_col_idx], agg_info, agg_col_idx);
    }
  }
  for (const auto& kv : other_results.as_unordered_map_) {
    auto it = as_unordered_map_.find(kv.first);
    if (it == as_unordered_map_.end()) {
      as_unordered_map_.insert(std::make_pair(kv.first, kv.second));
      continue;
    }
    auto& old_agg_results = it->second;
    CHECK_EQ(old_agg_results.size(), kv.second.size());
    const size_t agg_col_count = old_agg_results.size();
    for (size_t agg_col_idx = 0; agg_col_idx < agg_col_count; ++agg_col_idx) {
      const auto agg_info = targets_[agg_col_idx];
      reduce_impl(&old_agg_results[agg_col_idx], &kv.second[agg_col_idx], agg_info, agg_col_idx);
    }
  }
  CHECK(simple_keys_.empty() != multi_keys_.empty());
  target_values_.clear();
  target_values_.reserve(std::max(as_map_.size(), as_unordered_map_.size()));
  if (simple_keys_.empty()) {
    multi_keys_.clear();
    multi_keys_.reserve(as_map_.size());
    for (const auto& kv : as_map_) {
      multi_keys_.push_back(kv.first);
      target_values_.push_back(kv.second);
    }
  } else {
    simple_keys_.clear();
    simple_keys_.reserve(as_unordered_map_.size());
    for (const auto& kv : as_unordered_map_) {
      simple_keys_.push_back(kv.first);
      target_values_.push_back(kv.second);
    }
  }
}

void ResultRows::sort(const Planner::Sort* sort_plan) {
  const auto& target_list = sort_plan->get_targetlist();
  const auto& order_entries = sort_plan->get_order_entries();
  // TODO(alex): check the semantics for order by multiple columns
  for (const auto order_entry : boost::adaptors::reverse(order_entries)) {
    CHECK_GE(order_entry.tle_no, 1);
    CHECK_LE(order_entry.tle_no, target_list.size());
    auto compare = [this, &order_entry](const TargetValues& lhs, const TargetValues& rhs) {
      // The compare function must define a strict weak ordering, which means
      // we can't use the overloaded less than operator for boost::variant since
      // there's not greater than counterpart. If we naively use "not less than"
      // as the compare function for descending order, std::sort will trigger
      // a segmentation fault (or corrupt memory).
      const auto is_int = targets_[order_entry.tle_no - 1].sql_type.is_integer();
      const auto is_dict = targets_[order_entry.tle_no - 1].sql_type.is_string() &&
        targets_[order_entry.tle_no - 1].sql_type.get_compression() == kENCODING_DICT;
      const auto lhs_v = lhs[order_entry.tle_no - 1];
      const auto rhs_v = rhs[order_entry.tle_no - 1];
      const auto lhs_ip = boost::get<int64_t>(&lhs_v);
      if (lhs_ip) {
        const auto rhs_ip = boost::get<int64_t>(&rhs_v);
        CHECK(rhs_ip);
        if (is_dict) {
          auto string_dict = executor_->getStringDictionary(
            targets_[order_entry.tle_no - 1].sql_type.get_comp_param());
          auto lhs_str = string_dict->getString(*lhs_ip);
          auto rhs_str = string_dict->getString(*rhs_ip);
          return order_entry.is_desc ? lhs_str > rhs_str : lhs_str < rhs_str;
        }
        return order_entry.is_desc ? *lhs_ip > *rhs_ip : *lhs_ip < *rhs_ip;
      } else {
        const auto lhs_fp = boost::get<std::pair<int64_t, int64_t>>(&lhs_v);
        if (lhs_fp) {
          const auto rhs_fp = boost::get<std::pair<int64_t, int64_t>>(&rhs_v);
          CHECK(rhs_fp);
          return order_entry.is_desc
            ? pair_to_double(*lhs_fp, is_int) > pair_to_double(*rhs_fp, is_int)
            : pair_to_double(*lhs_fp, is_int) < pair_to_double(*rhs_fp, is_int);
        } else {
          const auto lhs_sp = boost::get<std::string>(&lhs_v);
          CHECK(lhs_sp);
          const auto rhs_sp = boost::get<std::string>(&rhs_v);
          CHECK(rhs_sp);
          return order_entry.is_desc ? *lhs_sp > *rhs_sp : *lhs_sp < *rhs_sp;
        }
      }
    };
    std::sort(target_values_.begin(), target_values_.end(), compare);
  }
}

Executor::Executor(const int db_id, const size_t block_size_x, const size_t grid_size_x,
                   const std::string& debug_dir, const std::string& debug_file)
  : cgen_state_(new CgenState())
  , is_nested_(false)
  , block_size_x_(block_size_x)
  , grid_size_x_(grid_size_x)
  , debug_dir_(debug_dir)
  , debug_file_(debug_file)
  , db_id_(db_id)
  , catalog_(nullptr) {}

std::shared_ptr<Executor> Executor::getExecutor(
    const int db_id,
    const std::string& debug_dir,
    const std::string& debug_file,
    const size_t block_size_x,
    const size_t grid_size_x) {
  auto it = executors_.find(std::make_tuple(db_id, block_size_x, grid_size_x));
  if (it != executors_.end()) {
    return it->second;
  }
  auto executor = std::make_shared<Executor>(db_id, block_size_x, grid_size_x, debug_dir, debug_file);
  auto it_ok = executors_.insert(std::make_pair(std::make_tuple(db_id, block_size_x, grid_size_x), executor));
  CHECK(it_ok.second);
  return executor;
}

namespace {

const ColumnDescriptor* get_column_descriptor(
    const int col_id,
    const int table_id,
    const Catalog_Namespace::Catalog& cat) {
  const auto col_desc = cat.getMetadataForColumn(table_id, col_id);
  CHECK(col_desc);
  return col_desc;
}

int64_t get_scan_limit(const Planner::Plan* plan, const int64_t limit) {
  return dynamic_cast<const Planner::Scan*>(plan) && limit ? limit : 0;
}

}  // namespace

ResultRows Executor::executeSelectPlan(
    const Planner::Plan* plan,
    const int64_t limit,
    const bool hoist_literals,
    const ExecutorDeviceType device_type,
    const ExecutorOptLevel opt_level,
    const Catalog_Namespace::Catalog& cat,
    const size_t max_groups_buffer_entry_guess,
    int32_t* error_code) {
  if (dynamic_cast<const Planner::Scan*>(plan) || dynamic_cast<const Planner::AggPlan*>(plan)) {
    auto row_set_mem_owner = std::make_shared<RowSetMemoryOwner>();
    if (limit) {
      const int64_t scan_limit { get_scan_limit(plan, limit) };
      auto rows = executeAggScanPlan(plan, limit, hoist_literals, device_type, opt_level, cat,
        row_set_mem_owner, scan_limit ? scan_limit : max_groups_buffer_entry_guess, error_code);
      rows.keepFirstN(limit);
      return rows;
    }
    return executeAggScanPlan(plan, limit, hoist_literals, device_type, opt_level, cat, row_set_mem_owner,
      max_groups_buffer_entry_guess, error_code);
  }
  const auto result_plan = dynamic_cast<const Planner::Result*>(plan);
  if (result_plan) {
    if (limit) {
      auto rows = executeResultPlan(result_plan, hoist_literals, device_type, opt_level,
        cat, max_groups_buffer_entry_guess, error_code);
      rows.keepFirstN(limit);
      return rows;
    }
    return executeResultPlan(result_plan, hoist_literals, device_type, opt_level,
      cat, max_groups_buffer_entry_guess, error_code);
  }
  const auto sort_plan = dynamic_cast<const Planner::Sort*>(plan);
  if (sort_plan) {
    return executeSortPlan(sort_plan, limit, hoist_literals, device_type, opt_level,
      cat, max_groups_buffer_entry_guess, error_code);
  }
  CHECK(false);
}

/*
 * x64 benchmark: "SELECT COUNT(*) FROM test WHERE x > 41;"
 *                x = 42, 64-bit column, 1-byte encoding
 *                3B rows in 1.2s on a i7-4870HQ core
 *
 * TODO(alex): check we haven't introduced a regression with the new translator.
 */

ResultRows Executor::execute(
    const Planner::RootPlan* root_plan,
    const bool hoist_literals,
    const ExecutorDeviceType device_type,
    const ExecutorOptLevel opt_level) {
  catalog_ = &root_plan->get_catalog();
  const auto stmt_type = root_plan->get_stmt_type();
  std::lock_guard<std::mutex> lock(execute_mutex_);
  switch (stmt_type) {
  case kSELECT: {
    int32_t error_code { 0 };
    auto rows = executeSelectPlan(root_plan->get_plan(), root_plan->get_limit(),
      hoist_literals, device_type, opt_level, root_plan->get_catalog(),
      2048, &error_code);
    if (error_code == ERR_DIV_BY_ZERO) {
      throw std::runtime_error("Division by zero");
    }
    if (error_code) {
      rows = executeSelectPlan(root_plan->get_plan(), root_plan->get_limit(),
        hoist_literals, ExecutorDeviceType::CPU, opt_level, root_plan->get_catalog(),
        0, &error_code);
      CHECK(!error_code);
      return rows;
    }
    return rows;
  }
  case kINSERT: {
    executeSimpleInsert(root_plan);
    return ResultRows({}, nullptr, nullptr);
  }
  default:
    CHECK(false);
  }
}

StringDictionary* Executor::getStringDictionary(const int dict_id) const {
  CHECK(catalog_);
  const auto dd = catalog_->getMetadataForDict(dict_id);
  CHECK(dd);
  CHECK_EQ(32, dd->dictNBits);
  std::lock_guard<std::mutex> lock(str_dicts_mutex_);
  const auto dict_it = str_dicts_.find(dict_id);
  if (dict_it != str_dicts_.end()) {
    return dict_it->second.get();
  }
  auto dict_it_ok = str_dicts_.insert(std::make_pair(dict_id,
    std::unique_ptr<StringDictionary>(new StringDictionary(dd->dictFolderPath))));
  CHECK(dict_it_ok.second);
  return dict_it_ok.first->second.get();
}

std::vector<int8_t> Executor::serializeLiterals(const Executor::LiteralValues& literals) {
  size_t lit_buf_size { 0 };
  for (const auto& lit : literals) {
    lit_buf_size = addAligned(lit_buf_size, Executor::literalBytes(lit));
  }
  std::vector<int8_t> serialized(lit_buf_size);
  size_t off { 0 };
  for (const auto& lit : literals) {
    const auto lit_bytes = Executor::literalBytes(lit);
    off = addAligned(off, lit_bytes);
    switch (lit.which()) {
      case 0: {
        const auto p = boost::get<bool>(&lit);
        CHECK(p);
        serialized[off - lit_bytes] = *p;
        break;
      }
      case 1: {
        const auto p = boost::get<int16_t>(&lit);
        CHECK(p);
        memcpy(&serialized[off - lit_bytes], p, lit_bytes);
        break;
      }
      case 2: {
        const auto p = boost::get<int32_t>(&lit);
        CHECK(p);
        memcpy(&serialized[off - lit_bytes], p, lit_bytes);
        break;
      }
      case 3: {
        const auto p = boost::get<int64_t>(&lit);
        CHECK(p);
        memcpy(&serialized[off - lit_bytes], p, lit_bytes);
        break;
      }
      case 4: {
        const auto p = boost::get<float>(&lit);
        CHECK(p);
        memcpy(&serialized[off - lit_bytes], p, lit_bytes);
        break;
      }
      case 5: {
        const auto p = boost::get<double>(&lit);
        CHECK(p);
        memcpy(&serialized[off - lit_bytes], p, lit_bytes);
        break;
      }
      case 6: {
        const auto p = boost::get<std::pair<std::string, int>>(&lit);
        CHECK(p);
        const auto str_id = getStringDictionary(p->second)->get(p->first);
        memcpy(&serialized[off - lit_bytes], &str_id, lit_bytes);
        break;
      }
      default:
        CHECK(false);
    }
  }
  return serialized;
}

std::vector<llvm::Value*> Executor::codegen(
    const Analyzer::Expr* expr,
    const bool fetch_columns,
    const bool hoist_literals) {
  if (!expr) {
    return { cgen_state_->getCurrentRowIndex() };
  }
  auto bin_oper = dynamic_cast<const Analyzer::BinOper*>(expr);
  if (bin_oper) {
    return { codegen(bin_oper, hoist_literals) };
  }
  auto u_oper = dynamic_cast<const Analyzer::UOper*>(expr);
  if (u_oper) {
    return { codegen(u_oper, hoist_literals) };
  }
  auto col_var = dynamic_cast<const Analyzer::ColumnVar*>(expr);
  if (col_var) {
    return codegen(col_var, fetch_columns, hoist_literals);
  }
  auto constant = dynamic_cast<const Analyzer::Constant*>(expr);
  if (constant) {
    // The dictionary encoding case should be handled by the parent expression
    // (cast, for now), here is too late to know the dictionary id
    CHECK_NE(kENCODING_DICT, constant->get_type_info().get_compression());
    return { codegen(constant, -1, hoist_literals) };
  }
  auto case_expr = dynamic_cast<const Analyzer::CaseExpr*>(expr);
  if (case_expr) {
    return { codegen(case_expr, hoist_literals) };
  }
  auto extract_expr = dynamic_cast<const Analyzer::ExtractExpr*>(expr);
  if (extract_expr) {
    return { codegen(extract_expr, hoist_literals) };
  }
  auto like_expr = dynamic_cast<const Analyzer::LikeExpr*>(expr);
  if (like_expr) {
    return { codegen(like_expr, hoist_literals) };
  }
  auto in_expr = dynamic_cast<const Analyzer::InValues*>(expr);
  if (in_expr) {
    return { codegen(in_expr, hoist_literals) };
  }
  CHECK(false);
}

extern "C"
uint64_t string_decode(int8_t* chunk_iter_, int64_t pos) {
  auto chunk_iter = reinterpret_cast<ChunkIter*>(chunk_iter_);
  VarlenDatum vd;
  bool is_end;
  ChunkIter_get_nth(chunk_iter, pos, false, &vd, &is_end);
  CHECK(!is_end);
  return vd.is_null
    ? 0
    : (reinterpret_cast<uint64_t>(vd.pointer) & 0xffffffffffff) | (static_cast<uint64_t>(vd.length) << 48);
}

extern "C"
uint64_t string_decompress(const int32_t string_id, const int64_t string_dict_handle) {
  auto string_dict = reinterpret_cast<const StringDictionary*>(string_dict_handle);
  auto string_bytes = string_dict->getStringBytes(string_id);
  CHECK(string_bytes.first);
  return (reinterpret_cast<uint64_t>(string_bytes.first) & 0xffffffffffff) |
         (static_cast<uint64_t>(string_bytes.second) << 48);
}

extern "C"
int32_t string_compress(const int64_t ptr_and_len, const int64_t string_dict_handle) {
  std::string raw_str(
    reinterpret_cast<char*>(extract_str_ptr_noinline(ptr_and_len)),
    extract_str_len_noinline(ptr_and_len));
  auto string_dict = reinterpret_cast<const StringDictionary*>(string_dict_handle);
  return string_dict->get(raw_str);
}

llvm::Value* Executor::codegen(const Analyzer::LikeExpr* expr, const bool hoist_literals) {
  char escape_char { '\\' };
  if (expr->get_escape_expr()) {
    auto escape_char_expr = dynamic_cast<const Analyzer::Constant*>(expr->get_escape_expr());
    CHECK(escape_char_expr);
    CHECK(escape_char_expr->get_type_info().is_string());
    CHECK_EQ(1, escape_char_expr->get_constval().stringval->size());
    escape_char = (*escape_char_expr->get_constval().stringval)[0];
  }
  auto str_lv = codegen(expr->get_arg(), true, hoist_literals);
  if (str_lv.size() != 3) {
    CHECK_EQ(1, str_lv.size());
    str_lv.push_back(cgen_state_->emitCall("extract_str_ptr", { str_lv.front() }));
    str_lv.push_back(cgen_state_->emitCall("extract_str_len", { str_lv.front() }));
    cgen_state_->must_run_on_cpu_ = true;
  }
  auto like_expr_arg_const = dynamic_cast<const Analyzer::Constant*>(expr->get_like_expr());
  CHECK(like_expr_arg_const);
  CHECK(like_expr_arg_const->get_type_info().is_string());
  CHECK_EQ(kENCODING_NONE, like_expr_arg_const->get_type_info().get_compression());
  const auto like_pattern = *like_expr_arg_const->get_constval().stringval;
  auto like_pattern_lv = cgen_state_->addStringConstant(like_pattern);
  auto like_pattern_len = like_expr_arg_const->get_constval().stringval->size();
  llvm::Value* like_pattern_len_lv = ll_int(static_cast<int32_t>(like_pattern_len));
  auto string_like_fn = expr->get_is_ilike()
    ? cgen_state_->module_->getFunction("string_ilike")
    : cgen_state_->module_->getFunction("string_like");
  CHECK(string_like_fn);
  return cgen_state_->ir_builder_.CreateCall(string_like_fn, std::vector<llvm::Value*> {
    str_lv[1],
    str_lv[2],
    like_pattern_lv,
    like_pattern_len_lv,
    ll_int(int8_t(escape_char))
  });
}

llvm::Value* Executor::codegen(const Analyzer::InValues* expr, const bool hoist_literals) {
  llvm::Value* result = llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(cgen_state_->context_), false);
  for (auto in_val : *expr->get_value_list()) {
    result = cgen_state_->ir_builder_.CreateOr(result,
      toBool(codegenCmp(kEQ, expr->get_arg(), in_val, hoist_literals)));
  }
  return result;
}

llvm::Value* Executor::codegen(const Analyzer::BinOper* bin_oper, const bool hoist_literals) {
  const auto optype = bin_oper->get_optype();
  if (IS_ARITHMETIC(optype)) {
    return codegenArith(bin_oper, hoist_literals);
  }
  if (IS_COMPARISON(optype)) {
    return codegenCmp(bin_oper, hoist_literals);
  }
  if (IS_LOGIC(optype)) {
    return codegenLogical(bin_oper, hoist_literals);
  }
  CHECK(false);
}

llvm::Value* Executor::codegen(const Analyzer::UOper* u_oper, const bool hoist_literals) {
  const auto optype = u_oper->get_optype();
  switch (optype) {
  case kNOT:
    return codegenLogical(u_oper, hoist_literals);
  case kCAST:
    return codegenCast(u_oper, hoist_literals);
  case kUMINUS:
    return codegenUMinus(u_oper, hoist_literals);
  case kISNULL:
    return codegenIsNull(u_oper, hoist_literals);
  default:
    CHECK(false);
  }
}

namespace {

std::shared_ptr<Decoder> get_col_decoder(const Analyzer::ColumnVar* col_var) {
  const auto enc_type = col_var->get_compression();
  const auto& type_info = col_var->get_type_info();
  switch (enc_type) {
  case kENCODING_NONE:
    switch (type_info.get_type()) {
    case kBOOLEAN:
      return std::make_shared<FixedWidthInt>(1);
    case kSMALLINT:
      return std::make_shared<FixedWidthInt>(2);
    case kINT:
      return std::make_shared<FixedWidthInt>(4);
    case kBIGINT:
      return std::make_shared<FixedWidthInt>(8);
    case kFLOAT:
      return std::make_shared<FixedWidthReal>(false);
    case kDOUBLE:
      return std::make_shared<FixedWidthReal>(true);
    case kTIME:
    case kTIMESTAMP:
    case kDATE:
      return std::make_shared<FixedWidthInt>(sizeof(time_t));
    default:
      // TODO(alex): make columnar results write the correct encoding
      if (type_info.is_string()) {
        return std::make_shared<FixedWidthInt>(4);
      }
      CHECK(false);
    }
  case kENCODING_DICT:
    CHECK(type_info.is_string());
    return std::make_shared<FixedWidthInt>(4);
  case kENCODING_FIXED: {
    const auto bit_width = col_var->get_comp_param();
    CHECK_EQ(0, bit_width % 8);
    return std::make_shared<FixedWidthInt>(bit_width / 8);
  }
  default:
    CHECK(false);
  }
}

size_t get_col_bit_width(const Analyzer::ColumnVar* col_var) {
  const auto& type_info = col_var->get_type_info();
  return get_bit_width(type_info.get_type());
}

}  // namespace

std::vector<llvm::Value*> Executor::codegen(
    const Analyzer::ColumnVar* col_var,
    const bool fetch_column,
    const bool hoist_literals) {
  // only generate the decoding code once; if a column has been previously
  // fetch in the generated IR, we'll reuse it
  auto col_id = col_var->get_column_id();
  if (col_var->get_rte_idx() >= 0 && !is_nested_) {
    CHECK_GT(col_id, 0);
  } else {
    CHECK((col_id == 0) || (col_var->get_rte_idx() >= 0 && col_var->get_table_id() > 0));
    const auto var = dynamic_cast<const Analyzer::Var*>(col_var);
    CHECK(var);
    col_id = var->get_varno();
    CHECK_GE(col_id, 1);
    if (var->get_which_row() == Analyzer::Var::kGROUPBY) {
      CHECK_LE(col_id, cgen_state_->group_by_expr_cache_.size());
      return { cgen_state_->group_by_expr_cache_[col_id - 1] };
    }
  }
  const int local_col_id = getLocalColumnId(col_id, fetch_column);
  auto it = cgen_state_->fetch_cache_.find(local_col_id);
  if (it != cgen_state_->fetch_cache_.end()) {
    return { it->second };
  }
  llvm::Value* col_byte_stream;
  llvm::Value* pos_arg;
  std::tie(col_byte_stream, pos_arg) = colByteStream(col_id, fetch_column, hoist_literals);
  if (plan_state_->isLazyFetchColumn(col_var)) {
    plan_state_->columns_to_not_fetch_.insert(col_id);
    return { pos_arg };
  }
  if (col_var->get_type_info().is_string() &&
      col_var->get_type_info().get_compression() == kENCODING_NONE) {
    // real (not dictionary-encoded) strings; store the pointer to the payload
    auto ptr_and_len = cgen_state_->emitExternalCall(
      "string_decode", get_int_type(64, cgen_state_->context_),
      { col_byte_stream, pos_arg });
    // Unpack the pointer + length, see string_decode function.
    auto str_lv = cgen_state_->emitCall("extract_str_ptr", { ptr_and_len });
    auto len_lv = cgen_state_->emitCall("extract_str_len", { ptr_and_len });
    auto it_ok = cgen_state_->fetch_cache_.insert(std::make_pair(
      local_col_id,
      std::vector<llvm::Value*> { ptr_and_len, str_lv, len_lv }));
    CHECK(it_ok.second);
    return { ptr_and_len, str_lv, len_lv };
  }
  const auto decoder = get_col_decoder(col_var);
  auto dec_val = decoder->codegenDecode(
    col_byte_stream,
    pos_arg,
    cgen_state_->module_);
  cgen_state_->ir_builder_.Insert(dec_val);
  auto dec_type = dec_val->getType();
  llvm::Value* dec_val_cast { nullptr };
  if (dec_type->isIntegerTy()) {
    auto dec_width = static_cast<llvm::IntegerType*>(dec_type)->getBitWidth();
    auto col_width = get_col_bit_width(col_var);
    dec_val_cast = cgen_state_->ir_builder_.CreateCast(
      static_cast<size_t>(col_width) > dec_width
        ? llvm::Instruction::CastOps::SExt
        : llvm::Instruction::CastOps::Trunc,
      dec_val,
      get_int_type(col_width, cgen_state_->context_));
  } else {
    CHECK(dec_type->isFloatTy() || dec_type->isDoubleTy());
    if (dec_type->isDoubleTy()) {
      CHECK(col_var->get_type_info().get_type() == kDOUBLE);
    } else if (dec_type->isFloatTy()) {
      CHECK(col_var->get_type_info().get_type() == kFLOAT);
    }
    dec_val_cast = dec_val;
  }
  CHECK(dec_val_cast);
  auto it_ok = cgen_state_->fetch_cache_.insert(std::make_pair(
    local_col_id,
    std::vector<llvm::Value*> { dec_val_cast }));
  CHECK(it_ok.second);
  return { it_ok.first->second };
}

// returns the byte stream argument and the position for the given column
std::pair<llvm::Value*, llvm::Value*>
Executor::colByteStream(const int col_id, const bool fetch_column, const bool hoist_literals) {
  auto& in_arg_list = cgen_state_->row_func_->getArgumentList();
  CHECK_GE(in_arg_list.size(), 3);
  size_t arg_idx = 0;
  size_t pos_idx = 0;
  llvm::Value* pos_arg { nullptr };
  const int local_col_id = getLocalColumnId(col_id, fetch_column);
  for (auto& arg : in_arg_list) {
    if (arg.getType()->isIntegerTy()) {
      pos_arg = &arg;
      pos_idx = arg_idx;
    } else if (pos_arg && arg_idx == pos_idx + 1 + static_cast<size_t>(local_col_id) + (hoist_literals ? 1 : 0)) {
      return std::make_pair(&arg, pos_arg);
    }
    ++arg_idx;
  }
  CHECK(false);
}

std::vector<llvm::Value*> Executor::codegen(const Analyzer::Constant* constant,
                                            const int dict_id,
                                            const bool hoist_literals) {
  const auto& type_info = constant->get_type_info();
  if (hoist_literals && (!type_info.is_string() || dict_id > 0)) {
    auto arg_it = cgen_state_->row_func_->arg_begin();
    while (arg_it != cgen_state_->row_func_->arg_end()) {
      if (arg_it->getType()->isIntegerTy()) {
        ++arg_it;
        break;
      }
      ++arg_it;
    }
    CHECK(arg_it != cgen_state_->row_func_->arg_end());
    const auto val_bits = get_bit_width(type_info.get_type());
    CHECK_EQ(0, val_bits % 8);
    llvm::Type* val_ptr_type { nullptr };
    if (type_info.is_integer() || type_info.is_time() || type_info.is_string() || type_info.is_boolean()) {
      val_ptr_type = llvm::PointerType::get(llvm::IntegerType::get(cgen_state_->context_, val_bits), 0);
    } else {
      CHECK(type_info.get_type() == kFLOAT || type_info.get_type() == kDOUBLE);
      val_ptr_type = (type_info.get_type() == kFLOAT)
        ? llvm::Type::getFloatPtrTy(cgen_state_->context_)
        : llvm::Type::getDoublePtrTy(cgen_state_->context_);
    }
    const int16_t lit_off = cgen_state_->getOrAddLiteral(constant, dict_id);
    const auto lit_buf_start = cgen_state_->ir_builder_.CreateGEP(
      arg_it, ll_int(lit_off));
    auto lit_lv = cgen_state_->ir_builder_.CreateLoad(
      cgen_state_->ir_builder_.CreateBitCast(lit_buf_start, val_ptr_type));
    return { lit_lv };
  }
  switch (type_info.get_type()) {
  case kBOOLEAN:
    return { llvm::ConstantInt::get(get_int_type(8, cgen_state_->context_), constant->get_constval().boolval) };
  case kSMALLINT:
  case kINT:
  case kBIGINT:
  case kTIME:
  case kTIMESTAMP:
  case kDATE:
    return { codegenIntConst(constant) };
  case kFLOAT:
    return { llvm::ConstantFP::get(llvm::Type::getFloatTy(cgen_state_->context_), constant->get_constval().floatval) };
  case kDOUBLE:
    return { llvm::ConstantFP::get(llvm::Type::getDoubleTy(cgen_state_->context_), constant->get_constval().doubleval) };
  case kVARCHAR:
  case kCHAR:
  case kTEXT: {
    const auto& str_const = *constant->get_constval().stringval;
    if (dict_id > 0) {
      return { ll_int(getStringDictionary(dict_id)->get(str_const)) };
    }
    return { ll_int(int64_t(0)),
      cgen_state_->addStringConstant(str_const), ll_int(static_cast<int32_t>(str_const.size())) };
  }
  default:
    CHECK(false);
  }
  CHECK(false);
}

std::vector<llvm::Value*> Executor::codegen(const Analyzer::CaseExpr* case_expr, const bool hoist_literals) {
  // Generate a "projection" function which takes the case conditions and
  // values as arguments, interleaved. The 'else' expression is the last one.
  const auto& expr_pair_list = case_expr->get_expr_pair_list();
  const auto else_expr = case_expr->get_else_expr();
  std::vector<llvm::Type*> case_arg_types;
  const auto case_ti = case_expr->get_type_info();
  llvm::Type* case_llvm_type = nullptr;
  bool is_real_str = false;
  if (case_ti.is_integer() || case_ti.is_time()) {
    case_llvm_type = get_int_type(get_bit_width(case_ti.get_type()), cgen_state_->context_);
  } else if (case_ti.is_fp()) {
    case_llvm_type = case_ti.get_type() == kFLOAT
      ? llvm::Type::getFloatTy(cgen_state_->context_)
      : llvm::Type::getDoubleTy(cgen_state_->context_);
  } else if (case_ti.is_string()) {
    if (case_ti.get_compression() == kENCODING_DICT) {
      case_llvm_type = get_int_type(8 * case_ti.get_size(), cgen_state_->context_);
    } else {
      is_real_str = true;
      case_llvm_type = get_int_type(64, cgen_state_->context_);
    }
  }
  CHECK(case_llvm_type);
  for (const auto& expr_pair : expr_pair_list) {
    CHECK_EQ(expr_pair.first->get_type_info().get_type(), kBOOLEAN);
    case_arg_types.push_back(llvm::Type::getInt1Ty(cgen_state_->context_));
    CHECK(expr_pair.second->get_type_info() == case_ti);
    case_arg_types.push_back(case_llvm_type);
  }
  CHECK(!else_expr || else_expr->get_type_info() == case_ti);
  case_arg_types.push_back(case_llvm_type);
  auto ft = llvm::FunctionType::get(
    case_llvm_type,
    case_arg_types,
    false);
  auto case_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "case_func", cgen_state_->module_);
  const auto end_case = llvm::BasicBlock::Create(cgen_state_->context_, "end_case", case_func);
  size_t expr_idx = 0;
  auto& case_branch_exprs = case_func->getArgumentList();
  auto arg_it = case_branch_exprs.begin();
  llvm::BasicBlock* next_cmp_branch { end_case };
  auto& case_func_entry = case_func->front();
  llvm::IRBuilder<> case_func_builder(&case_func_entry);
  for (size_t i = 0; i < expr_pair_list.size(); ++i) {
    CHECK(arg_it != case_branch_exprs.end());
    llvm::Value* cond_lv = arg_it++;
    CHECK(arg_it != case_branch_exprs.end());
    llvm::Value* ret_lv = arg_it++;
    auto ret_case = llvm::BasicBlock::Create(cgen_state_->context_, "ret_case", case_func, next_cmp_branch);
    case_func_builder.SetInsertPoint(ret_case);
    case_func_builder.CreateRet(ret_lv);
    auto cmp_case = llvm::BasicBlock::Create(cgen_state_->context_, "cmp_case", case_func, ret_case);
    case_func_builder.SetInsertPoint(cmp_case);
    case_func_builder.CreateCondBr(cond_lv, ret_case, next_cmp_branch);
    next_cmp_branch = cmp_case;
    ++expr_idx;
  }
  CHECK(arg_it != case_branch_exprs.end());
  case_func_builder.SetInsertPoint(end_case);
  case_func_builder.CreateRet(arg_it++);
  CHECK(arg_it == case_branch_exprs.end());
  case_func->addAttribute(llvm::AttributeSet::FunctionIndex, llvm::Attribute::AlwaysInline);
  std::vector<llvm::Value*> case_func_args;
  // The 'case_func' function checks arguments for match in reverse order
  // (code generation is easier this way because of how 'BasicBlock::Create' works).
  // Reverse the actual arguments to compensate for this, then call the function.
  for (const auto& expr_pair : boost::adaptors::reverse(expr_pair_list)) {
    case_func_args.push_back(toBool(codegen(expr_pair.first, true, hoist_literals).front()));
    auto branch_val_lvs = codegen(expr_pair.second, true, hoist_literals);
    if (is_real_str && dynamic_cast<const Analyzer::Constant*>(expr_pair.second)) {
      CHECK_EQ(3, branch_val_lvs.size());
      case_func_args.push_back(cgen_state_->emitCall("string_pack", { branch_val_lvs[1], branch_val_lvs[2] }));
    } else {
      CHECK_EQ(1, branch_val_lvs.size());
      case_func_args.push_back(branch_val_lvs.front());
    }
  }
  Analyzer::Constant null_expr(case_ti, true, Datum {});
  auto else_lvs = codegen(else_expr ? else_expr : &null_expr, true, hoist_literals);
  if (is_real_str && dynamic_cast<const Analyzer::Constant*>(else_expr)) {
    CHECK_EQ(3, else_lvs.size());
    case_func_args.push_back(cgen_state_->emitCall("string_pack", { else_lvs[1], else_lvs[2] }));
  } else {
    case_func_args.push_back(else_lvs.front());
  }
  llvm::Value* case_val = cgen_state_->ir_builder_.CreateCall(case_func, case_func_args);
  std::vector<llvm::Value*> ret_vals { case_val };
  if (is_real_str) {
    ret_vals.push_back(cgen_state_->emitCall("extract_str_ptr", { case_val }));
    ret_vals.push_back(cgen_state_->emitCall("extract_str_len", { case_val }));
  }
  return ret_vals;
}

llvm::Value* Executor::codegen(const Analyzer::ExtractExpr* extract_expr, const bool hoist_literals) {
  auto from_expr = codegen(extract_expr->get_from_expr(), true, hoist_literals).front();
  const int32_t extract_field { extract_expr->get_field() };
  if (extract_field == kEPOCH) {
    CHECK(extract_expr->get_from_expr()->get_type_info().get_type() == kTIMESTAMP ||
          extract_expr->get_from_expr()->get_type_info().get_type() == kDATE);
    if (from_expr->getType()->isIntegerTy(32)) {
      from_expr = cgen_state_->ir_builder_.CreateCast(
        llvm::Instruction::CastOps::SExt, from_expr, get_int_type(64, cgen_state_->context_));
    }
    return from_expr;
  }
  CHECK(from_expr->getType()->isIntegerTy(32) || from_expr->getType()->isIntegerTy(64));
  const auto extract_func = cgen_state_->module_->getFunction("ExtractFromTime");
  auto arg_it = extract_func->arg_begin();
  ++arg_it;
  CHECK(arg_it->getType()->isIntegerTy(32) || arg_it->getType()->isIntegerTy(64));
  if (arg_it->getType()->isIntegerTy(32) && from_expr->getType()->isIntegerTy(64)) {
    from_expr = cgen_state_->ir_builder_.CreateCast(
      llvm::Instruction::CastOps::Trunc, from_expr, get_int_type(32, cgen_state_->context_));
  }
  CHECK(extract_func);
  std::vector<llvm::Value*> extract_func_args {
    ll_int(static_cast<int32_t>(extract_expr->get_field())),
    from_expr
  };
  return cgen_state_->ir_builder_.CreateCall(extract_func, extract_func_args);
}

namespace {

llvm::CmpInst::Predicate llvm_icmp_pred(const SQLOps op_type) {
  switch (op_type) {
  case kEQ:
    return llvm::ICmpInst::ICMP_EQ;
  case kNE:
    return llvm::ICmpInst::ICMP_NE;
  case kLT:
    return llvm::ICmpInst::ICMP_SLT;
  case kGT:
    return llvm::ICmpInst::ICMP_SGT;
  case kLE:
    return llvm::ICmpInst::ICMP_SLE;
  case kGE:
    return llvm::ICmpInst::ICMP_SGE;
  default:
    CHECK(false);
  }
}

std::string icmp_name(const SQLOps op_type) {
  switch (op_type) {
  case kEQ:
    return "eq";
  case kNE:
    return "ne";
  case kLT:
    return "lt";
  case kGT:
    return "gt";
  case kLE:
    return "le";
  case kGE:
    return "ge";
  default:
    CHECK(false);
  }
}

llvm::CmpInst::Predicate llvm_fcmp_pred(const SQLOps op_type) {
  switch (op_type) {
  case kEQ:
    return llvm::CmpInst::FCMP_OEQ;
  case kNE:
    return llvm::CmpInst::FCMP_ONE;
  case kLT:
    return llvm::CmpInst::FCMP_OLT;
  case kGT:
    return llvm::CmpInst::FCMP_OGT;
  case kLE:
    return llvm::CmpInst::FCMP_OLE;
  case kGE:
    return llvm::CmpInst::FCMP_OGE;
  default:
    CHECK(false);
  }
}

}  // namespace

namespace {

std::string string_cmp_func(const SQLOps optype) {
  switch (optype) {
  case kLT:
    return "string_lt";
  case kLE:
    return "string_le";
  case kGT:
    return "string_gt";
  case kGE:
    return "string_ge";
  case kEQ:
    return "string_eq";
  case kNE:
    return "string_ne";
  default:
    CHECK(false);
  }
}

}  // namespace

llvm::Value* Executor::codegenCmp(const Analyzer::BinOper* bin_oper, const bool hoist_literals) {
  const auto optype = bin_oper->get_optype();
  const auto lhs = bin_oper->get_left_operand();
  const auto rhs = bin_oper->get_right_operand();
  return codegenCmp(optype, lhs, rhs, hoist_literals);
}

llvm::Value* Executor::codegenCmp(const SQLOps optype,
                                  const Analyzer::Expr* lhs,
                                  const Analyzer::Expr* rhs,
                                  const bool hoist_literals) {
  CHECK(IS_COMPARISON(optype));
  const auto& lhs_ti = lhs->get_type_info();
  const auto& rhs_ti = rhs->get_type_info();
  auto lhs_lvs = codegen(lhs, true, hoist_literals);
  auto rhs_lvs = codegen(rhs, true, hoist_literals);
  CHECK((lhs_ti.get_type() == rhs_ti.get_type()) ||
        (lhs_ti.is_string() && rhs_ti.is_string()));
  const bool not_null { lhs_ti.get_notnull() && rhs_ti.get_notnull() };
  if (lhs_ti.is_integer() || lhs_ti.is_time() || lhs_ti.is_boolean() || lhs_ti.is_string()) {
    if (lhs_ti.is_string()) {
      CHECK(rhs_ti.is_string());
      CHECK_EQ(lhs_ti.get_compression(), rhs_ti.get_compression());
      if (lhs_ti.get_compression() == kENCODING_NONE) {
        // unpack pointer + length if necessary
        if (lhs_lvs.size() != 3) {
          CHECK_EQ(1, lhs_lvs.size());
          lhs_lvs.push_back(cgen_state_->emitCall("extract_str_ptr", { lhs_lvs.front() }));
          lhs_lvs.push_back(cgen_state_->emitCall("extract_str_len", { lhs_lvs.front() }));
        }
        if (rhs_lvs.size() != 3) {
          CHECK_EQ(1, rhs_lvs.size());
          rhs_lvs.push_back(cgen_state_->emitCall("extract_str_ptr", { rhs_lvs.front() }));
          rhs_lvs.push_back(cgen_state_->emitCall("extract_str_len", { rhs_lvs.front() }));
        }
        return cgen_state_->emitCall(string_cmp_func(optype),
          { lhs_lvs[1], lhs_lvs[2], rhs_lvs[1], rhs_lvs[2] });
      } else {
        CHECK(optype == kEQ || optype == kNE);
      }
    }
    const std::string int_typename { "int" + std::to_string(get_bit_width(lhs_ti.get_type())) + "_t" };
    auto lhs_type = lhs_ti.get_type();
    // TODO(alex): refactor this
    if (lhs_ti.is_string()) {
      CHECK_EQ(kENCODING_DICT, lhs_ti.get_compression());
      CHECK_EQ(4, lhs_ti.get_size());
      lhs_type = kINT;
    } else if (lhs_ti.is_time()) {
      switch (lhs_ti.get_size()) {
      case 4:
        lhs_type = kINT;
        break;
      case 8:
        lhs_type = kBIGINT;
        break;
      default:
        CHECK(false);
      }
    }
    return not_null
      ? cgen_state_->ir_builder_.CreateICmp(llvm_icmp_pred(optype), lhs_lvs.front(), rhs_lvs.front())
      : cgen_state_->emitCall(icmp_name(optype) + "_" + int_typename + "_nullable",
        { lhs_lvs.front(), rhs_lvs.front(), ll_int(inline_int_null_val(lhs_ti)),
          inlineIntNull(SQLTypeInfo(kBOOLEAN)) });
  }
  if (lhs_ti.get_type() == kFLOAT || lhs_ti.get_type() == kDOUBLE) {
    const std::string fp_typename { lhs_ti.get_type() == kFLOAT ? "float" : "double" };
    return not_null
      ? cgen_state_->ir_builder_.CreateFCmp(llvm_fcmp_pred(optype), lhs_lvs.front(), rhs_lvs.front())
      : cgen_state_->emitCall(icmp_name(optype) + "_" + fp_typename + "_nullable",
        {
          lhs_lvs.front(), rhs_lvs.front(),
          lhs_ti.get_type() == kFLOAT ? ll_fp(NULL_FLOAT) : ll_fp(NULL_DOUBLE),
          inlineIntNull(SQLTypeInfo(kBOOLEAN))
        });
  }
  CHECK(false);
}

llvm::Value* Executor::codegenLogical(const Analyzer::BinOper* bin_oper, const bool hoist_literals) {
  const auto optype = bin_oper->get_optype();
  CHECK(IS_LOGIC(optype));
  const auto lhs = bin_oper->get_left_operand();
  const auto rhs = bin_oper->get_right_operand();
  const auto lhs_lv = toBool(codegen(lhs, true, hoist_literals).front());
  const auto rhs_lv = toBool(codegen(rhs, true, hoist_literals).front());
  switch (optype) {
  case kAND:
    return cgen_state_->ir_builder_.CreateAnd(lhs_lv, rhs_lv);
  case kOR:
    return cgen_state_->ir_builder_.CreateOr(lhs_lv, rhs_lv);
  default:
    CHECK(false);
  }
}

llvm::Value* Executor::toBool(llvm::Value* lv) {
  CHECK(lv->getType()->isIntegerTy());
  if (static_cast<llvm::IntegerType*>(lv->getType())->getBitWidth() > 1) {
    return cgen_state_->ir_builder_.CreateICmp(llvm::ICmpInst::ICMP_SGT, lv,
      llvm::ConstantInt::get(lv->getType(), 0));
  }
  return lv;
}

llvm::Value* Executor::codegenCast(const Analyzer::UOper* uoper, const bool hoist_literals) {
  CHECK_EQ(uoper->get_optype(), kCAST);
  const auto& ti = uoper->get_type_info();
  const auto operand = uoper->get_operand();
  const auto operand_as_const = dynamic_cast<const Analyzer::Constant*>(operand);
  // For dictionary encoded constants, the cast holds the dictionary id
  // information as the compression parameter; handle this case separately.
  auto operand_lv = operand_as_const
    ? codegen(operand_as_const, ti.get_comp_param(), hoist_literals).front()
    : codegen(operand, true, hoist_literals).front();
  const auto& operand_ti = operand->get_type_info();
  if (operand_lv->getType()->isIntegerTy()) {
    if (operand_ti.is_string()) {
      CHECK(ti.is_string());
      // dictionary encode non-constant
      if (operand_ti.get_compression() != kENCODING_DICT && !operand_as_const) {
        CHECK_EQ(kENCODING_NONE, operand_ti.get_compression());
        CHECK_EQ(kENCODING_DICT, ti.get_compression());
        CHECK_EQ(64, static_cast<llvm::IntegerType*>(operand_lv->getType())->getBitWidth());
        cgen_state_->must_run_on_cpu_ = true;
        return cgen_state_->emitExternalCall("string_compress",
          get_int_type(32, cgen_state_->context_),
          { operand_lv, ll_int(int64_t(getStringDictionary(ti.get_comp_param()))) });
      }
      CHECK_EQ(32, static_cast<llvm::IntegerType*>(operand_lv->getType())->getBitWidth());
      const auto dd = catalog_->getMetadataForDict(operand_ti.get_comp_param());
      CHECK(dd);
      if (ti.get_compression() == kENCODING_NONE) {
        CHECK_EQ(kENCODING_DICT, operand_ti.get_compression());
        cgen_state_->must_run_on_cpu_ = true;
        return cgen_state_->emitExternalCall("string_decompress",
          get_int_type(64, cgen_state_->context_),
          { operand_lv, ll_int(int64_t(getStringDictionary(operand_ti.get_comp_param()))) });
      }
      CHECK(operand_as_const);
      CHECK_EQ(kENCODING_DICT, ti.get_compression());
      return operand_lv;
    }
    CHECK(operand_ti.is_integer() || operand_ti.is_time() || operand_ti.is_boolean());
    if (operand_ti.is_boolean()) {
      CHECK(operand_lv->getType()->isIntegerTy(1) ||
            operand_lv->getType()->isIntegerTy(8));
      if (operand_lv->getType()->isIntegerTy(1)) {
        operand_lv = cgen_state_->ir_builder_.CreateZExt(operand_lv, get_int_type(8, cgen_state_->context_));
      }
    }
    if (ti.is_integer() || ti.is_time()) {
      const auto operand_width = static_cast<llvm::IntegerType*>(operand_lv->getType())->getBitWidth();
      const auto target_width = get_bit_width(ti.get_type());
      return cgen_state_->ir_builder_.CreateCast(target_width > operand_width
            ? llvm::Instruction::CastOps::SExt
            : llvm::Instruction::CastOps::Trunc,
          operand_lv,
          get_int_type(target_width, cgen_state_->context_));
    } else {
      CHECK(ti.get_type() == kFLOAT || ti.get_type() == kDOUBLE);
      return cgen_state_->ir_builder_.CreateSIToFP(operand_lv, ti.get_type() == kFLOAT
        ? llvm::Type::getFloatTy(cgen_state_->context_)
        : llvm::Type::getDoubleTy(cgen_state_->context_));
    }
  } else {
    CHECK(operand_ti.get_type() == kFLOAT ||
          operand_ti.get_type() == kDOUBLE);
    CHECK(operand_lv->getType()->isFloatTy() || operand_lv->getType()->isDoubleTy());
    if (ti.get_type() == kDOUBLE) {
      return cgen_state_->ir_builder_.CreateFPExt(
        operand_lv, llvm::Type::getDoubleTy(cgen_state_->context_));
    } else if (ti.is_integer()) {
      return cgen_state_->ir_builder_.CreateFPToSI(operand_lv,
        get_int_type(get_bit_width(ti.get_type()), cgen_state_->context_));
    } else {
      CHECK(false);
    }
  }
}

llvm::Value* Executor::codegenUMinus(const Analyzer::UOper* uoper, const bool hoist_literals) {
  CHECK_EQ(uoper->get_optype(), kUMINUS);
  const auto operand_lv = codegen(uoper->get_operand(), true, hoist_literals).front();
  CHECK(operand_lv->getType()->isIntegerTy());
  const auto& ti = uoper->get_type_info();
  const std::string int_typename { "int" + std::to_string(get_bit_width(ti.get_type())) + "_t" };
  return ti.get_notnull()
    ? cgen_state_->ir_builder_.CreateNeg(operand_lv)
    : cgen_state_->emitCall("uminus_" + int_typename + "_nullable",
      { operand_lv, inlineIntNull(ti) });
}

llvm::Value* Executor::codegenLogical(const Analyzer::UOper* uoper, const bool hoist_literals) {
  const auto optype = uoper->get_optype();
  CHECK(optype == kNOT || optype == kUMINUS || optype == kISNULL);
  const auto operand = uoper->get_operand();
  const auto operand_lv = toBool(codegen(operand, true, hoist_literals).front());
  switch (optype) {
  case kNOT:
    return cgen_state_->ir_builder_.CreateNot(operand_lv);
  default:
    CHECK(false);
  }
}

llvm::Value* Executor::codegenIsNull(const Analyzer::UOper* uoper, const bool hoist_literals) {
  const auto operand = uoper->get_operand();
  CHECK(operand->get_type_info().is_integer() ||
        operand->get_type_info().is_boolean() ||
        operand->get_type_info().is_time() ||
        operand->get_type_info().is_string());
  // if the type is inferred as non null, short-circuit to false
  if (operand->get_type_info().get_notnull()) {
    return llvm::ConstantInt::get(get_int_type(1, cgen_state_->context_), 0);
  }
  const auto operand_lv = codegen(operand, true, hoist_literals).front();
  return cgen_state_->ir_builder_.CreateICmp(llvm::ICmpInst::ICMP_EQ,
    operand_lv, inlineIntNull(operand->get_type_info()));
}

llvm::ConstantInt* Executor::codegenIntConst(const Analyzer::Constant* constant) {
  const auto& type_info = constant->get_type_info();
  if (constant->get_is_null()) {
    return inlineIntNull(type_info);
  }
  switch (type_info.get_type()) {
  case kSMALLINT:
    return ll_int(constant->get_constval().smallintval);
  case kINT:
    return ll_int(constant->get_constval().intval);
  case kBIGINT:
    return ll_int(constant->get_constval().bigintval);
  case kTIME:
  case kTIMESTAMP:
  case kDATE:
    return ll_int(constant->get_constval().timeval);
  default:
    CHECK(false);
  }
}

llvm::ConstantInt* Executor::inlineIntNull(const SQLTypeInfo& type_info) {
  auto type = type_info.get_type();
  if (type_info.is_string()) {
    switch (type_info.get_compression()) {
    case kENCODING_DICT: {
      return ll_int(static_cast<int32_t>(inline_int_null_val(type_info)));
    }
    case kENCODING_NONE:
      return ll_int(int64_t(0));
    default:
      CHECK(false);
    }
  }
  switch (type) {
  case kBOOLEAN:
    return ll_int(static_cast<int8_t>(inline_int_null_val(type_info)));
  case kSMALLINT:
    return ll_int(static_cast<int16_t>(inline_int_null_val(type_info)));
  case kINT:
    return ll_int(static_cast<int32_t>(inline_int_null_val(type_info)));
  case kBIGINT:
    return ll_int(inline_int_null_val(type_info));
  case kTIME:
  case kTIMESTAMP:
  case kDATE:
    return ll_int(inline_int_null_val(type_info));
  default:
    CHECK(false);
  }
}

llvm::Value* Executor::codegenArith(const Analyzer::BinOper* bin_oper, const bool hoist_literals) {
  const auto optype = bin_oper->get_optype();
  CHECK(IS_ARITHMETIC(optype));
  const auto lhs = bin_oper->get_left_operand();
  const auto rhs = bin_oper->get_right_operand();
  const auto& lhs_type = lhs->get_type_info();
  const auto& rhs_type = rhs->get_type_info();
  const auto lhs_lv = codegen(lhs, true, hoist_literals).front();
  const auto rhs_lv = codegen(rhs, true, hoist_literals).front();
  CHECK_EQ(lhs_type.get_type(), rhs_type.get_type());
  const bool not_null { lhs_type.get_notnull() && rhs_type.get_notnull() };
  if (lhs_type.is_integer()) {
    const std::string int_typename { "int" + std::to_string(get_bit_width(lhs_type.get_type())) + "_t" };
    switch (optype) {
    case kMINUS:
      if (not_null) {
        return cgen_state_->ir_builder_.CreateSub(lhs_lv, rhs_lv);
      } else {
        return cgen_state_->emitCall("sub_" + int_typename + "_nullable",
          { lhs_lv, rhs_lv, ll_int(inline_int_null_val(lhs_type)) });
      }
    case kPLUS:
      if (not_null) {
        return cgen_state_->ir_builder_.CreateAdd(lhs_lv, rhs_lv);
      } else {
        return cgen_state_->emitCall("add_" + int_typename + "_nullable",
          { lhs_lv, rhs_lv, ll_int(inline_int_null_val(lhs_type)) });
      }
    case kMULTIPLY:
      if (not_null) {
        return cgen_state_->ir_builder_.CreateMul(lhs_lv, rhs_lv);
      } else {
        return cgen_state_->emitCall("mul_" + int_typename + "_nullable",
          { lhs_lv, rhs_lv, ll_int(inline_int_null_val(lhs_type)) });
      }
    case kDIVIDE:
      return codegenDiv(lhs_lv, rhs_lv, not_null ? "" : int_typename, lhs_type);
    default:
      CHECK(false);
    }
  }
  if (lhs_type.is_fp()) {
    const std::string fp_typename { lhs_type.get_type() == kFLOAT ? "float" : "double" };
    llvm::ConstantFP* fp_null { lhs_type.get_type() == kFLOAT ? ll_fp(NULL_FLOAT) : ll_fp(NULL_DOUBLE) };
    switch (optype) {
    case kMINUS:
      return not_null
        ? cgen_state_->ir_builder_.CreateFSub(lhs_lv, rhs_lv)
        : cgen_state_->emitCall("sub_" + fp_typename + "_nullable", { lhs_lv, rhs_lv, fp_null });
    case kPLUS:
      return not_null
        ? cgen_state_->ir_builder_.CreateFAdd(lhs_lv, rhs_lv)
        : cgen_state_->emitCall("add_" + fp_typename + "_nullable", { lhs_lv, rhs_lv, fp_null });
    case kMULTIPLY:
      return not_null
        ? cgen_state_->ir_builder_.CreateFAdd(lhs_lv, rhs_lv)
        : cgen_state_->emitCall("mul_" + fp_typename + "_nullable", { lhs_lv, rhs_lv, fp_null });
    case kDIVIDE:
      return codegenDiv(lhs_lv, rhs_lv, not_null ? "" : fp_typename, lhs_type);
    default:
      CHECK(false);
    }
  }
  CHECK(false);
}

llvm::Value* Executor::codegenDiv(llvm::Value* lhs_lv, llvm::Value* rhs_lv,
    const std::string& null_typename, const SQLTypeInfo& ti) {
  CHECK_EQ(lhs_lv->getType(), rhs_lv->getType());
  cgen_state_->uses_div_ = true;
  auto div_ok = llvm::BasicBlock::Create(cgen_state_->context_, "div_ok", cgen_state_->row_func_);
  auto div_zero = llvm::BasicBlock::Create(cgen_state_->context_, "div_zero", cgen_state_->row_func_);
  auto zero_const = rhs_lv->getType()->isIntegerTy()
    ? llvm::ConstantInt::get(rhs_lv->getType(), 0, true)
    : llvm::ConstantFP::get(rhs_lv->getType(), 0.);
  cgen_state_->ir_builder_.CreateCondBr(zero_const->getType()->isFloatingPointTy()
    ? cgen_state_->ir_builder_.CreateFCmp(llvm::FCmpInst::FCMP_ONE, rhs_lv, zero_const)
    : cgen_state_->ir_builder_.CreateICmp(llvm::ICmpInst::ICMP_NE, rhs_lv, zero_const),
    div_ok, div_zero);
  cgen_state_->ir_builder_.SetInsertPoint(div_ok);
  auto ret = zero_const->getType()->isIntegerTy()
    ? (null_typename.empty()
        ? cgen_state_->ir_builder_.CreateSDiv(lhs_lv, rhs_lv)
        : cgen_state_->emitCall("div_" + null_typename + "_nullable",
          { lhs_lv, rhs_lv, ll_int(inline_int_null_val(ti)) }))
    : (null_typename.empty()
        ? cgen_state_->ir_builder_.CreateFDiv(lhs_lv, rhs_lv)
        : cgen_state_->emitCall("div_" + null_typename + "_nullable",
          { lhs_lv, rhs_lv, ti.get_type() == kFLOAT ? ll_fp(NULL_FLOAT) : ll_fp(NULL_DOUBLE) }));
  cgen_state_->ir_builder_.SetInsertPoint(div_zero);
  cgen_state_->ir_builder_.CreateRet(ll_int(ERR_DIV_BY_ZERO));
  cgen_state_->ir_builder_.SetInsertPoint(div_ok);
  return ret;
}

namespace {

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

std::vector<int64_t*> launch_query_cpu_code(
    const std::vector<void*>& fn_ptrs,
    const bool hoist_literals,
    const std::vector<int8_t>& literal_buff,
    std::vector<const int8_t*> col_buffers,
    const int64_t num_rows,
    const int64_t scan_limit,
    const std::vector<int64_t>& init_agg_vals,
    std::vector<int64_t*> group_by_buffers,
    std::vector<int64_t*> small_group_by_buffers,
    int32_t* error_code) {
  const size_t agg_col_count = init_agg_vals.size();
  std::vector<int64_t*> out_vec;
  if (group_by_buffers.empty()) {
    for (size_t i = 0; i < agg_col_count; ++i) {
      auto buff = new int64_t[1];
      out_vec.push_back(static_cast<int64_t*>(buff));
    }
  }
  if (hoist_literals) {
    typedef void (*agg_query)(
      const int8_t** col_buffers,
      const int8_t* literals,
      const int64_t* num_rows,
      const int64_t* max_matched,
      const int64_t* init_agg_value,
      int64_t** out,
      int64_t** out2,
      int32_t* resume_row_index);
    *error_code = 0;
    if (group_by_buffers.empty()) {
      reinterpret_cast<agg_query>(fn_ptrs[0])(&col_buffers[0], &literal_buff[0],
        &num_rows, &scan_limit, &init_agg_vals[0], &out_vec[0], nullptr, error_code);
    } else {
      reinterpret_cast<agg_query>(fn_ptrs[0])(&col_buffers[0], &literal_buff[0],
        &num_rows, &scan_limit, &init_agg_vals[0], &group_by_buffers[0], &small_group_by_buffers[0], error_code);
    }
  } else {
    typedef void (*agg_query)(
      const int8_t** col_buffers,
      const int64_t* num_rows,
      const int64_t* max_matched,
      const int64_t* init_agg_value,
      int64_t** out,
      int64_t** out2,
      int32_t* resume_row_index);
    *error_code = 0;
    if (group_by_buffers.empty()) {
      reinterpret_cast<agg_query>(fn_ptrs[0])(&col_buffers[0], &num_rows, &scan_limit,
        &init_agg_vals[0], &out_vec[0], nullptr, error_code);
    } else {
      *error_code = 0;
      reinterpret_cast<agg_query>(fn_ptrs[0])(&col_buffers[0], &num_rows, &scan_limit,
        &init_agg_vals[0], &group_by_buffers[0], &small_group_by_buffers[0], error_code);
    }
  }
  return out_vec;
}

#undef checkCudaErrors

#ifdef __clang__
#pragma clang diagnostic pop
#else
#pragma GCC diagnostic pop
#endif

// TODO(alex): proper types for aggregate
int64_t init_agg_val(const SQLAgg agg, const SQLTypeInfo& ti) {
  const auto target_type = ti.get_type();
  switch (agg) {
  case kAVG:
  case kSUM:
  case kCOUNT: {
    const double zero_double { 0. };
    return (IS_INTEGER(target_type) || IS_TIME(target_type)) ? 0L : *reinterpret_cast<const int64_t*>(&zero_double);
  }
  case kMIN: {
    const double max_double { std::numeric_limits<double>::max() };
    return (IS_INTEGER(target_type) || IS_TIME(target_type))
      ? (ti.get_notnull() ? std::numeric_limits<int64_t>::max() : inline_int_null_val(ti))
      : *reinterpret_cast<const int64_t*>(&max_double);
  }
  case kMAX: {
    const auto min_double { std::numeric_limits<double>::min() };
    return (IS_INTEGER(target_type) || IS_TIME(target_type))
      ? (ti.get_notnull() ? std::numeric_limits<int64_t>::min() : inline_int_null_val(ti))
      : *reinterpret_cast<const int64_t*>(&min_double);
  }
  default:
    CHECK(false);
  }
}

// TODO(alex): remove
int64_t reduce_results(const SQLAgg agg, const SQLTypeInfo& ti, const int64_t* out_vec, const size_t out_vec_sz) {
  switch (agg) {
  case kAVG:
  case kSUM:
  case kCOUNT:
    if (ti.is_integer() || ti.is_time()) {
      return std::accumulate(out_vec, out_vec + out_vec_sz, init_agg_val(agg, ti));
    } else {
      CHECK(ti.is_fp());
      const int64_t agg_init_val = init_agg_val(agg, ti);
      double r = *reinterpret_cast<const double*>(&agg_init_val);
      for (size_t i = 0; i < out_vec_sz; ++i) {
        r += *reinterpret_cast<const double*>(&out_vec[i]);
      }
      return *reinterpret_cast<const int64_t*>(&r);
    }
  case kMIN: {
    const int64_t agg_init_val = init_agg_val(agg, ti);
    if (ti.is_integer() || ti.is_time()) {
      int64_t agg_result = agg_init_val;
      for (size_t i = 0; i < out_vec_sz; ++i) {
        agg_min_skip_val(&agg_result, out_vec[i], agg_init_val);
      }
      return agg_result;
    } else {
      CHECK(ti.is_fp());
      double r = *reinterpret_cast<const double*>(&agg_init_val);
      for (size_t i = 0; i < out_vec_sz; ++i) {
        r = std::min<const double>(*reinterpret_cast<const double*>(&r), *reinterpret_cast<const double*>(&out_vec[i]));
      }
      return *reinterpret_cast<const int64_t*>(&r);
    }
  }
  case kMAX: {
    const int64_t agg_init_val = init_agg_val(agg, ti);
    if (ti.is_integer() || ti.is_time()) {
      int64_t agg_result = agg_init_val;
      for (size_t i = 0; i < out_vec_sz; ++i) {
        agg_max_skip_val(&agg_result, out_vec[i], agg_init_val);
      }
      return agg_result;
    } else {
      CHECK(ti.is_fp());
      double r = *reinterpret_cast<const double*>(&agg_init_val);
      for (size_t i = 0; i < out_vec_sz; ++i) {
        r = std::max<const double>(*reinterpret_cast<const double*>(&r), *reinterpret_cast<const double*>(&out_vec[i]));
      }
      return *reinterpret_cast<const int64_t*>(&r);
    }
  }
  default:
    CHECK(false);
  }
  CHECK(false);
}

}  // namespace

ResultRows Executor::reduceMultiDeviceResults(
    const std::vector<ResultRows>& results_per_device,
    const QueryMemoryDescriptor& query_mem_desc,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner) const {
  if (results_per_device.empty()) {
    return ResultRows({}, nullptr, nullptr);
  }

  auto reduced_results = results_per_device.front();

  for (size_t i = 1; i < results_per_device.size(); ++i) {
    reduced_results.reduce(results_per_device[i]);
  }

  return reduced_results;
}

namespace {

class ColumnarResults {
public:
  ColumnarResults(const ResultRows& rows,
                  const size_t num_columns,
                  const std::vector<SQLTypeInfo>& target_types)
    : column_buffers_(num_columns)
    , num_rows_(rows.size()) {
    column_buffers_.resize(num_columns);
    for (size_t i = 0; i < num_columns; ++i) {
      CHECK(!target_types[i].is_string() || (target_types[i].get_compression() == kENCODING_DICT &&
        target_types[i].get_size() == 4));
      column_buffers_[i] = static_cast<const int8_t*>(
        malloc(num_rows_ * (get_bit_width(target_types[i].get_type()) / 8)));
    }
    for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
      for (size_t i = 0; i < num_columns; ++i) {
        const auto col_val = rows.get(row_idx, i, false);
        auto i64_p = boost::get<int64_t>(&col_val);
        if (i64_p) {
          switch (get_bit_width(target_types[i].get_type())) {
          case 16:
            ((int16_t*) column_buffers_[i])[row_idx] = *i64_p;
            break;
          case 32:
            ((int32_t*) column_buffers_[i])[row_idx] = *i64_p;
            break;
          case 64:
            ((int64_t*) column_buffers_[i])[row_idx] = *i64_p;
            break;
          default:
            CHECK(false);
          }
        } else {
          CHECK(target_types[i].get_type() == kFLOAT || target_types[i].get_type() == kDOUBLE);
          auto double_p = boost::get<double>(&col_val);
          switch (target_types[i].get_type()) {
          case kFLOAT:
            ((float*) column_buffers_[i])[row_idx] = static_cast<float>(*double_p);
            break;
          case kDOUBLE:
            ((double*) column_buffers_[i])[row_idx] = static_cast<double>(*double_p);
            break;
          default:
            CHECK(false);
          }
        }
      }
    }
  }
  ~ColumnarResults() {
    for (const auto column_buffer : column_buffers_) {
      free((void*) column_buffer);
    }
  }
  const std::vector<const int8_t*>& getColumnBuffers() const {
    return column_buffers_;
  }
  const size_t size() const {
    return num_rows_;
  }
private:
  std::vector<const int8_t*> column_buffers_;
  const size_t num_rows_;
};

std::pair<llvm::Function*, std::vector<llvm::Value*>> create_row_function(
    const size_t in_col_count,
    const size_t agg_col_count,
    const bool hoist_literals,
    llvm::Function* query_func,
    llvm::Module* module,
    llvm::LLVMContext& context);

std::vector<Executor::AggInfo> get_agg_name_and_exprs(const Planner::Plan* plan) {
  std::vector<Executor::AggInfo> result;
  const auto target_exprs = get_agg_target_exprs(plan);
  for (size_t target_idx = 0; target_idx < target_exprs.size(); ++target_idx) {
    const auto target_expr = target_exprs[target_idx];
    CHECK(target_expr);
    const auto target_type_info = target_expr->get_type_info();
    const auto target_type = target_type_info.get_type();
    const auto agg_expr = dynamic_cast<Analyzer::AggExpr*>(target_expr);
    if (!agg_expr) {
      result.emplace_back((target_type == kFLOAT || target_type == kDOUBLE) ? "agg_id_double" : "agg_id",
                          target_expr, 0, target_idx);
      if (target_type_info.is_string() && target_type_info.get_compression() == kENCODING_NONE) {
        result.emplace_back("agg_id", target_expr, 0, target_idx);
      }
      continue;
    }
    CHECK(target_type_info.is_integer() || target_type_info.is_time() || target_type == kFLOAT || target_type == kDOUBLE);
    const auto agg_type = agg_expr->get_aggtype();
    const auto agg_init_val = init_agg_val(agg_type, target_type_info);
    switch (agg_type) {
    case kAVG: {
      const auto agg_arg_type_info = agg_expr->get_arg()->get_type_info();
      const auto agg_arg_type = agg_arg_type_info.get_type();
      CHECK(agg_arg_type_info.is_integer() || agg_arg_type == kFLOAT || agg_arg_type == kDOUBLE);
      result.emplace_back((agg_arg_type_info.is_integer() || agg_arg_type_info.is_time()) ? "agg_sum" : "agg_sum_double",
                          agg_expr->get_arg(), agg_init_val, target_idx);
      result.emplace_back((agg_arg_type_info.is_integer() || agg_arg_type_info.is_time()) ? "agg_count" : "agg_count_double",
                          agg_expr->get_arg(), agg_init_val, target_idx);
      break;
   }
    case kMIN:
      result.emplace_back((target_type_info.is_integer() || target_type_info.is_time()) ? "agg_min" : "agg_min_double",
                          agg_expr->get_arg(), agg_init_val, target_idx);
      break;
    case kMAX:
      result.emplace_back((target_type_info.is_integer() || target_type_info.is_time()) ? "agg_max" : "agg_max_double",
                          agg_expr->get_arg(), agg_init_val, target_idx);
      break;
    case kSUM:
      result.emplace_back((target_type_info.is_integer() || target_type_info.is_time()) ? "agg_sum" : "agg_sum_double",
                          agg_expr->get_arg(), agg_init_val, target_idx);
      break;
    case kCOUNT:
      result.emplace_back(
        agg_expr->get_is_distinct() ? "agg_count_distinct" : "agg_count",
        agg_expr->get_arg(),
        agg_init_val,
        target_idx);
      break;
    default:
      CHECK(false);
    }
  }
  return result;
}

ResultRows results_union(const std::vector<ResultRows>& results_per_device) {
  if (results_per_device.empty()) {
    return ResultRows({}, nullptr, nullptr);
  }
  auto all_results = results_per_device.front();
  for (size_t dev_idx = 1; dev_idx < results_per_device.size(); ++dev_idx) {
    all_results.append(results_per_device[dev_idx]);
  }
  return all_results;
}

}  // namespace

ResultRows Executor::executeResultPlan(
    const Planner::Result* result_plan,
    const bool hoist_literals,
    const ExecutorDeviceType device_type,
    const ExecutorOptLevel opt_level,
    const Catalog_Namespace::Catalog& cat,
    const size_t max_groups_buffer_entry_guess,
    int32_t* error_code) {
  const auto agg_plan = dynamic_cast<const Planner::AggPlan*>(result_plan->get_child_plan());
  CHECK(agg_plan);
  auto result_rows = executeAggScanPlan(agg_plan, 0, hoist_literals, device_type, opt_level, cat,
    nullptr, max_groups_buffer_entry_guess, error_code);
  if (*error_code) {
    return ResultRows({}, nullptr, nullptr);
  }
  const auto& targets = result_plan->get_targetlist();
  CHECK(!targets.empty());
  std::vector<AggInfo> agg_infos;
  for (size_t target_idx = 0; target_idx < targets.size(); ++target_idx) {
    const auto target_entry = targets[target_idx];
    const auto target_type = target_entry->get_expr()->get_type_info().get_type();
    agg_infos.emplace_back(
      (target_type == kFLOAT || target_type == kDOUBLE) ? "agg_id_double" : "agg_id",
      target_entry->get_expr(), 0, target_idx);
  }
  const int in_col_count { static_cast<int>(agg_plan->get_targetlist().size()) };
  const size_t in_agg_count { targets.size() };
  std::vector<SQLTypeInfo> target_types;
  std::vector<int64_t> init_agg_vals(in_col_count);
  for (auto in_col : agg_plan->get_targetlist()) {
    target_types.push_back(in_col->get_expr()->get_type_info());
  }
  ColumnarResults result_columns(result_rows, in_col_count, target_types);
  std::vector<llvm::Value*> col_heads;
  llvm::Function* row_func;
  // Nested query, let the compiler know
  is_nested_ = true;
  std::vector<Analyzer::Expr*> target_exprs;
  for (auto target_entry : targets) {
    target_exprs.push_back(target_entry->get_expr());
  }
  QueryMemoryDescriptor query_mem_desc {
    this,
    GroupByColRangeType::OneColGuessedRange, false, false,
    { sizeof(int64_t) },
    get_col_byte_widths(target_exprs),
    result_rows.size(),
    small_groups_buffer_entry_count_,
    0, GroupByMemSharing::Shared
  };
  auto query_func = query_group_by_template(cgen_state_->module_, is_nested_,
    hoist_literals, query_mem_desc, ExecutorDeviceType::CPU, false);
  std::tie(row_func, col_heads) = create_row_function(
    in_col_count, in_agg_count, hoist_literals, query_func, cgen_state_->module_, cgen_state_->context_);
  CHECK(row_func);
  std::list<int> pseudo_scan_cols;
  for (int pseudo_col = 1; pseudo_col <= in_col_count; ++pseudo_col) {
    pseudo_scan_cols.push_back(pseudo_col);
  }
  auto compilation_result = compilePlan(result_plan, {}, agg_infos, pseudo_scan_cols,
    result_plan->get_constquals(), result_plan->get_quals(), hoist_literals,
    ExecutorDeviceType::CPU, opt_level, nullptr, false, nullptr, result_rows.size(), 0);
  auto column_buffers = result_columns.getColumnBuffers();
  CHECK_EQ(column_buffers.size(), in_col_count);
  auto query_exe_context = query_mem_desc.getQueryExecutionContext(init_agg_vals, this,
    ExecutorDeviceType::CPU, 0, {}, nullptr);
  const auto hoist_buf = serializeLiterals(compilation_result.literal_values);
  *error_code = 0;
  launch_query_cpu_code(compilation_result.native_functions, hoist_literals, hoist_buf,
    column_buffers, result_columns.size(), 0, init_agg_vals,
    query_exe_context->group_by_buffers_, query_exe_context->small_group_by_buffers_, error_code);
  CHECK(!*error_code);
  return query_exe_context->groupBufferToResults(0, target_exprs);
}

ResultRows Executor::executeSortPlan(
    const Planner::Sort* sort_plan,
    const int64_t limit,
    const bool hoist_literals,
    const ExecutorDeviceType device_type,
    const ExecutorOptLevel opt_level,
    const Catalog_Namespace::Catalog& cat,
    const size_t max_groups_buffer_entry_guess,
    int32_t* error_code) {
  *error_code = 0;
  auto rows_to_sort = executeSelectPlan(sort_plan->get_child_plan(), 0,
    hoist_literals, device_type, opt_level, cat, max_groups_buffer_entry_guess,
    error_code);
  rows_to_sort.sort(sort_plan);
  if (limit) {
    rows_to_sort.keepFirstN(limit);
  }
  return rows_to_sort;
}

ResultRows Executor::executeAggScanPlan(
    const Planner::Plan* plan,
    const int64_t limit,
    const bool hoist_literals,
    const ExecutorDeviceType device_type_in,
    const ExecutorOptLevel opt_level,
    const Catalog_Namespace::Catalog& cat,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const size_t max_groups_buffer_entry_guess_in,
    int32_t* error_code) {
  *error_code = 0;
  const auto agg_plan = dynamic_cast<const Planner::AggPlan*>(plan);
  // TODO(alex): heuristic for group by buffer size
  const auto scan_plan = agg_plan
    ? dynamic_cast<const Planner::Scan*>(plan->get_child_plan())
    : dynamic_cast<const Planner::Scan*>(plan);
  CHECK(scan_plan);
  auto agg_infos = get_agg_name_and_exprs(plan);
  auto device_type = device_type_in;
  for (const auto& agg_info : agg_infos) {
    // TODO(alex): count distinct can't be executed on the GPU yet, punt to CPU
    if (std::get<0>(agg_info) == "agg_count_distinct") {
      device_type = ExecutorDeviceType::CPU;
      break;
    }
  }
  std::list<Analyzer::Expr*> groupby_exprs = agg_plan ? agg_plan->get_groupby_list() : std::list<Analyzer::Expr*> { nullptr };
  const int table_id = scan_plan->get_table_id();
  const auto table_descriptor = cat.getMetadataForTable(table_id);
  const auto fragmenter = table_descriptor->fragmenter;
  CHECK(fragmenter);
  Fragmenter_Namespace::QueryInfo query_info;
  fragmenter->getFragmentsForQuery(query_info);
  const auto& fragments = query_info.fragments;

  auto max_groups_buffer_entry_guess = max_groups_buffer_entry_guess_in;
  if (!max_groups_buffer_entry_guess) {
    // The query has failed the first execution attempt because of running out
    // of group by slots. Make the conservative choice: allocate fragment size
    // slots and run on the CPU.
    CHECK(device_type == ExecutorDeviceType::CPU);
    using Fragmenter_Namespace::FragmentInfo;
    CHECK(!fragments.empty());
    auto it = std::max_element(fragments.begin(), fragments.end(),
      [](const FragmentInfo& f1, const FragmentInfo& f2) { return f1.numTuples < f2.numTuples; });
    max_groups_buffer_entry_guess = it->numTuples;  // not a guess anymore
  }

  const std::list<Analyzer::Expr*>& simple_quals = scan_plan->get_simple_quals();

  const int64_t scan_limit { get_scan_limit(plan, limit) };

  CompilationResult compilation_result_cpu;
  auto compile_on_cpu = [&]() {
    try {
      compilation_result_cpu = compilePlan(plan, query_info, agg_infos,
        scan_plan->get_col_list(),
        simple_quals, scan_plan->get_quals(),
        hoist_literals, ExecutorDeviceType::CPU, opt_level,
        cat.get_dataMgr().cudaMgr_, true, row_set_mem_owner,
        max_groups_buffer_entry_guess, scan_limit);
    } catch (...) {
      compilation_result_cpu = compilePlan(plan, query_info, agg_infos,
        scan_plan->get_col_list(),
        simple_quals, scan_plan->get_quals(),
        hoist_literals, ExecutorDeviceType::CPU, opt_level,
        cat.get_dataMgr().cudaMgr_, false, row_set_mem_owner,
        max_groups_buffer_entry_guess, scan_limit);
    }
  };

  if (device_type == ExecutorDeviceType::CPU || device_type == ExecutorDeviceType::Auto) {
    compile_on_cpu();
  }

  CompilationResult compilation_result_gpu;
  if (device_type == ExecutorDeviceType::GPU || (device_type == ExecutorDeviceType::Auto &&
      cat.get_dataMgr().gpusPresent())) {
    try {
      compilation_result_gpu = compilePlan(plan, query_info, agg_infos,
        scan_plan->get_col_list(),
        simple_quals, scan_plan->get_quals(),
        hoist_literals, ExecutorDeviceType::GPU, opt_level,
        cat.get_dataMgr().cudaMgr_,
        true, row_set_mem_owner, max_groups_buffer_entry_guess, scan_limit);
    } catch (...) {
      compilation_result_gpu = compilePlan(plan, query_info, agg_infos,
        scan_plan->get_col_list(),
        simple_quals, scan_plan->get_quals(),
        hoist_literals, ExecutorDeviceType::GPU, opt_level,
        cat.get_dataMgr().cudaMgr_,
        false, row_set_mem_owner, max_groups_buffer_entry_guess, scan_limit);
    }
  }

  if (cgen_state_->must_run_on_cpu_) {
    if (device_type == ExecutorDeviceType::GPU) {  // override user choice
      compile_on_cpu();
    }
    device_type = ExecutorDeviceType::CPU;
  }

  for (const auto target_expr : get_agg_target_exprs(plan)) {
    plan_state_->target_exprs_.push_back(target_expr);
  }
  const auto current_dbid = cat.get_currentDB().dbId;
  const auto& col_global_ids = scan_plan->get_col_list();
  std::vector<ResultRows> all_fragment_results;
  all_fragment_results.reserve(fragments.size());
  std::vector<std::thread> query_threads;
  // could use std::thread::hardware_concurrency(), but some
  // slightly out-of-date compilers (gcc 4.7) implement it as always 0.
  // Play it POSIX.1 safe instead.
  int available_cpus = std::max(2 * sysconf(_SC_NPROCESSORS_CONF), 1L);
  std::unordered_set<int> available_gpus;
  if (cat.get_dataMgr().gpusPresent()) {
    int gpu_count = cat.get_dataMgr().cudaMgr_->getDeviceCount();
    CHECK_GT(gpu_count, 0);
    for (int gpu_id = 0; gpu_id < gpu_count; ++gpu_id) {
      available_gpus.insert(gpu_id);
    }
  }
  std::condition_variable scheduler_cv;
  std::mutex scheduler_mutex;
  std::mutex reduce_mutex;
  std::mutex str_dec_mutex;
  size_t result_rows_count { 0 };
  for (size_t i = 0; i < fragments.size(); ++i) {
    if (skipFragment(fragments[i], simple_quals)) {
      continue;
    }
    auto dispatch = [this, plan, scan_limit, current_dbid, device_type, i, table_id,
        &available_cpus, &available_gpus, &reduce_mutex, &scheduler_cv, &scheduler_mutex,
        &str_dec_mutex, &compilation_result_cpu, &compilation_result_gpu, hoist_literals,
        &all_fragment_results, &cat, &col_global_ids, &fragments, &groupby_exprs,
        row_set_mem_owner, error_code]
        (const ExecutorDeviceType chosen_device_type, int chosen_device_id) {
      ResultRows device_results({}, nullptr, nullptr);
      std::vector<const int8_t*> col_buffers(plan_state_->global_to_local_col_ids_.size());
      const auto& fragment = fragments[i];
      auto num_rows = static_cast<int64_t>(fragment.numTuples);
      const auto memory_level = chosen_device_type == ExecutorDeviceType::GPU
        ? Data_Namespace::GPU_LEVEL
        : Data_Namespace::CPU_LEVEL;
      if (device_type != ExecutorDeviceType::Auto) {
        chosen_device_id = fragment.deviceIds[static_cast<int>(memory_level)];
      }
      CHECK_GE(chosen_device_id, 0);
      CHECK_LT(chosen_device_id, max_gpu_count);
      std::list<ChunkIter> chunk_iterators;  // need to own them while query executes
      std::unique_ptr<std::lock_guard<std::mutex>> gpu_lock;
      if (chosen_device_type == ExecutorDeviceType::GPU) {
        gpu_lock.reset(new std::lock_guard<std::mutex>(gpu_exec_mutex_[chosen_device_id]));
      }
      std::vector<std::shared_ptr<Chunk_NS::Chunk>> chunks;
      for (const int col_id : col_global_ids) {
        auto chunk_meta_it = fragment.chunkMetadataMap.find(col_id);
        CHECK(chunk_meta_it != fragment.chunkMetadataMap.end());
        ChunkKey chunk_key { current_dbid, table_id, col_id, fragment.fragmentId };
        const ColumnDescriptor *cd = cat.getMetadataForColumn(table_id, col_id);
        auto it = plan_state_->global_to_local_col_ids_.find(col_id);
        CHECK(it != plan_state_->global_to_local_col_ids_.end());
        CHECK_LT(it->second, plan_state_->global_to_local_col_ids_.size());
        auto memory_level_for_column = memory_level;
        if (plan_state_->columns_to_fetch_.find(col_id) == plan_state_->columns_to_fetch_.end()) {
          memory_level_for_column = Data_Namespace::CPU_LEVEL;
        }
        std::shared_ptr<Chunk_NS::Chunk> chunk;
        {
          std::lock_guard<std::mutex> lock(str_dec_mutex);
          chunk = Chunk_NS::Chunk::getChunk(cd, &cat.get_dataMgr(),
            chunk_key,
            memory_level_for_column,
            chosen_device_id,
            chunk_meta_it->second.numBytes,
            chunk_meta_it->second.numElements);
          chunks.push_back(chunk);
        }
        const bool is_real_string = cd->columnType.is_string() &&
          cd->columnType.get_compression() == kENCODING_NONE;
        if (is_real_string) {
          chunk_iterators.push_back(chunk->begin_iterator());
          auto& chunk_iter = chunk_iterators.back();
          if (memory_level_for_column == Data_Namespace::CPU_LEVEL) {
            col_buffers[it->second] = reinterpret_cast<int8_t*>(&chunk_iter);
          } else {
            CHECK_EQ(Data_Namespace::GPU_LEVEL, memory_level_for_column);
            auto& data_mgr = cat.get_dataMgr();
            auto chunk_iter_gpu = alloc_gpu_mem(&data_mgr, sizeof(ChunkIter), chosen_device_id);
            copy_to_gpu(&data_mgr, chunk_iter_gpu, &chunk_iter, sizeof(ChunkIter), chosen_device_id);
            col_buffers[it->second] = reinterpret_cast<int8_t*>(chunk_iter_gpu);
          }
        } else {
          auto ab = chunk->get_buffer();
          CHECK(ab->getMemoryPtr());
          col_buffers[it->second] = ab->getMemoryPtr(); // @TODO(alex) change to use ChunkIter
        }
      }
      CHECK(chosen_device_type != ExecutorDeviceType::Auto);
      const CompilationResult& compilation_result =
        chosen_device_type == ExecutorDeviceType::GPU ? compilation_result_gpu : compilation_result_cpu;
      auto query_exe_context = compilation_result.query_mem_desc.getQueryExecutionContext(
        plan_state_->init_agg_vals_, this, chosen_device_type, chosen_device_id, col_buffers, row_set_mem_owner);
      int32_t err { 0 };
      if (groupby_exprs.empty()) {
        err = executePlanWithoutGroupBy(
          compilation_result, hoist_literals,
          device_results, get_agg_target_exprs(plan), chosen_device_type,
          col_buffers, query_exe_context.get(),
          num_rows, &cat.get_dataMgr(), chosen_device_id);
      } else {
        err = executePlanWithGroupBy(
          compilation_result, hoist_literals,
          device_results, get_agg_target_exprs(plan),
          groupby_exprs.size(), chosen_device_type, col_buffers,
          query_exe_context.get(), num_rows,
          &cat.get_dataMgr(), chosen_device_id,
          scan_limit);
      }
      {
        std::lock_guard<std::mutex> lock(reduce_mutex);
        if (err) {
          *error_code = err;
        }
        all_fragment_results.push_back(device_results);
      }
      if (device_type == ExecutorDeviceType::Auto) {
        std::unique_lock<std::mutex> scheduler_lock(scheduler_mutex);
        if (chosen_device_type == ExecutorDeviceType::CPU) {
          ++available_cpus;
        } else {
          CHECK(chosen_device_type == ExecutorDeviceType::GPU);
          auto it_ok = available_gpus.insert(chosen_device_id);
          CHECK(it_ok.second);
        }
        scheduler_lock.unlock();
        scheduler_cv.notify_one();
      }
    };
    auto chosen_device_type = device_type;
    int chosen_device_id = 0;
    if (device_type == ExecutorDeviceType::Auto) {
      std::unique_lock<std::mutex> scheduler_lock(scheduler_mutex);
      scheduler_cv.wait(scheduler_lock, [this, &available_cpus, &available_gpus] {
        return available_cpus || !available_gpus.empty();
      });
      if (!available_gpus.empty()) {
        chosen_device_type = ExecutorDeviceType::GPU;
        auto device_id_it = available_gpus.begin();
        chosen_device_id = *device_id_it;
        available_gpus.erase(device_id_it);
      } else {
        chosen_device_type = ExecutorDeviceType::CPU;
        CHECK_GT(available_cpus, 0);
        --available_cpus;
      }
    }
    if (!agg_plan) {
      dispatch(chosen_device_type, chosen_device_id);
      result_rows_count += all_fragment_results.back().size();
      if (limit && result_rows_count >= static_cast<size_t>(limit)) {
        break;
      }
    } else {
      query_threads.push_back(std::thread(dispatch, chosen_device_type, chosen_device_id));
    }
  }
  for (auto& child : query_threads) {
    child.join();
  }
  cat.get_dataMgr().freeAllBuffers();
  return agg_plan
    ? reduceMultiDeviceResults(all_fragment_results, compilation_result_cpu.query_mem_desc, row_set_mem_owner)
    : results_union(all_fragment_results);
}

int32_t Executor::executePlanWithoutGroupBy(
    const CompilationResult& compilation_result,
    const bool hoist_literals,
    ResultRows& results,
    const std::vector<Analyzer::Expr*>& target_exprs,
    const ExecutorDeviceType device_type,
    std::vector<const int8_t*>& col_buffers,
    const QueryExecutionContext* query_exe_context,
    const int64_t num_rows,
    Data_Namespace::DataMgr* data_mgr,
    const int device_id) {
  int32_t error_code { 0 };
  std::vector<int64_t*> out_vec;
  const auto hoist_buf = serializeLiterals(compilation_result.literal_values);
  if (device_type == ExecutorDeviceType::CPU) {
    out_vec = launch_query_cpu_code(
      compilation_result.native_functions, hoist_literals, hoist_buf,
      col_buffers, num_rows, 0, plan_state_->init_agg_vals_, {}, {}, &error_code);
  } else {
    out_vec = query_exe_context->launchGpuCode(
      compilation_result.native_functions, hoist_literals, hoist_buf,
      col_buffers, num_rows, 0, plan_state_->init_agg_vals_,
      data_mgr, block_size_x_, grid_size_x_, device_id, &error_code);
  }
  results = ResultRows(target_exprs, this, query_exe_context->row_set_mem_owner_);
  results.beginRow();
  size_t out_vec_idx = 0;
  for (const auto target_expr : target_exprs) {
    const auto agg_info = target_info(target_expr);
    CHECK(agg_info.is_agg);
    const auto val1 = reduce_results(
      agg_info.agg_kind,
      target_expr->get_type_info(),
      out_vec[out_vec_idx],
      device_type == ExecutorDeviceType::GPU ? block_size_x_ * grid_size_x_ : 1);
    if (agg_info.agg_kind == kAVG) {
      ++out_vec_idx;
      results.addValue(val1, reduce_results(
        agg_info.agg_kind,
        target_expr->get_type_info(),
        out_vec[out_vec_idx],
        device_type == ExecutorDeviceType::GPU ? block_size_x_ * grid_size_x_ : 1));
    } else {
      results.addValue(val1);
    }
    ++out_vec_idx;
  }
  for (auto out : out_vec) {
    delete[] out;
  }
  return error_code;
}

int32_t Executor::executePlanWithGroupBy(
    const CompilationResult& compilation_result,
    const bool hoist_literals,
    ResultRows& results,
    const std::vector<Analyzer::Expr*>& target_exprs,
    const size_t group_by_col_count,
    const ExecutorDeviceType device_type,
    std::vector<const int8_t*>& col_buffers,
    const QueryExecutionContext* query_exe_context,
    const int64_t num_rows,
    Data_Namespace::DataMgr* data_mgr,
    const int device_id,
    const int64_t scan_limit) {
  CHECK(results.empty());
  CHECK_GT(group_by_col_count, 0);
  // TODO(alex):
  // 1. Optimize size (make keys more compact).
  // 2. Resize on overflow.
  // 3. Optimize runtime.
  const auto hoist_buf = serializeLiterals(compilation_result.literal_values);
  int32_t error_code { 0 };
  if (device_type == ExecutorDeviceType::CPU) {
    launch_query_cpu_code(compilation_result.native_functions, hoist_literals, hoist_buf, col_buffers,
      num_rows, scan_limit, plan_state_->init_agg_vals_,
      query_exe_context->group_by_buffers_, query_exe_context->small_group_by_buffers_, &error_code);
  } else {
    query_exe_context->launchGpuCode(
      compilation_result.native_functions, hoist_literals, hoist_buf, col_buffers,
      num_rows, scan_limit, plan_state_->init_agg_vals_,
      data_mgr, block_size_x_, grid_size_x_, device_id, &error_code);
  }
  results = query_exe_context->getRowSet(target_exprs);
  if (error_code && (!scan_limit || results.size() < static_cast<size_t>(scan_limit))) {
    return error_code;  // unlucky, not enough results and we ran out of slots
  }
  return 0;
}

void Executor::executeSimpleInsert(const Planner::RootPlan* root_plan) {
  const auto plan = root_plan->get_plan();
  CHECK(plan);
  const auto values_plan = dynamic_cast<const Planner::ValuesScan*>(plan);
  CHECK(values_plan);
  const auto& targets = values_plan->get_targetlist();
  const int table_id = root_plan->get_result_table_id();
  const auto& col_id_list = root_plan->get_result_col_list();
  std::vector<const ColumnDescriptor*> col_descriptors;
  std::vector<int> col_ids;
  std::unordered_map<int, int8_t*> col_buffers;
  std::unordered_map<int, std::vector<std::string>> str_col_buffers;
  auto& cat = root_plan->get_catalog();
  for (const int col_id : col_id_list) {
    const auto cd = get_column_descriptor(col_id, table_id, cat);
    const auto col_enc = cd->columnType.get_compression();
    if (cd->columnType.is_string()) {
      switch (col_enc) {
      case kENCODING_NONE: {
        auto it_ok = str_col_buffers.insert(std::make_pair(col_id, std::vector<std::string> {}));
        CHECK(it_ok.second);
        break;
      }
      case kENCODING_DICT: {
        const auto dd = cat.getMetadataForDict(cd->columnType.get_comp_param());
        CHECK(dd);
        auto it_ok = col_buffers.insert(std::make_pair(col_id, nullptr));
        CHECK(it_ok.second);
        break;
      }
      default:
        CHECK(false);
      }
    } else {
      auto it_ok = col_buffers.insert(std::make_pair(col_id, nullptr));
      CHECK(it_ok.second);
    }
    col_descriptors.push_back(cd);
    col_ids.push_back(col_id);
  }
  size_t col_idx = 0;
  Fragmenter_Namespace::InsertData insert_data;
  insert_data.databaseId = cat.get_currentDB().dbId;
  insert_data.tableId = table_id;
  for (auto target_entry : targets) {
    auto col_cv = dynamic_cast<const Analyzer::Constant*>(target_entry->get_expr());
    if (!col_cv) {
      auto col_cast = dynamic_cast<const Analyzer::UOper*>(target_entry->get_expr());
      CHECK(col_cast);
      CHECK_EQ(kCAST, col_cast->get_optype());
      col_cv = dynamic_cast<const Analyzer::Constant*>(col_cast->get_operand());
    }
    CHECK(col_cv);
    const auto cd = col_descriptors[col_idx];
    auto col_datum = col_cv->get_constval();
    switch (cd->columnType.get_type()) {
    case kBOOLEAN: {
      auto col_data = reinterpret_cast<int8_t*>(malloc(sizeof(int8_t)));
      *col_data = col_cv->get_is_null()
        ? inline_int_null_val(cd->columnType)
        : (col_datum.boolval ? 1 : 0);
      col_buffers[col_ids[col_idx]] = col_data;
      break;
    }
    case kSMALLINT: {
      auto col_data = reinterpret_cast<int16_t*>(malloc(sizeof(int16_t)));
      *col_data = col_cv->get_is_null()
        ? inline_int_null_val(cd->columnType)
        : col_datum.smallintval;
      col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
      break;
    }
    case kINT: {
      auto col_data = reinterpret_cast<int32_t*>(malloc(sizeof(int32_t)));
      *col_data = col_cv->get_is_null()
        ? inline_int_null_val(cd->columnType)
        : col_datum.intval;
      col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
      break;
    }
    case kBIGINT: {
      auto col_data = reinterpret_cast<int64_t*>(malloc(sizeof(int64_t)));
      *col_data = col_cv->get_is_null()
        ? inline_int_null_val(cd->columnType)
        : col_datum.bigintval;
      col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
      break;
    }
    case kFLOAT: {
      auto col_data = reinterpret_cast<float*>(malloc(sizeof(float)));
      *col_data = col_datum.floatval;
      col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
      break;
    }
    case kDOUBLE: {
      auto col_data = reinterpret_cast<double*>(malloc(sizeof(double)));
      *col_data = col_datum.doubleval;
      col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
      break;
    }
    case kTEXT:
    case kVARCHAR:
    case kCHAR: {
      switch (cd->columnType.get_compression()) {
      case kENCODING_NONE:
        str_col_buffers[col_ids[col_idx]].push_back(*col_datum.stringval);
        break;
      case kENCODING_DICT: {
        const int dict_id = cd->columnType.get_comp_param();
        const int32_t str_id = getStringDictionary(dict_id)->getOrAdd(*col_datum.stringval);
        auto col_data = reinterpret_cast<int32_t*>(malloc(sizeof(int32_t)));
        *col_data = str_id;
        col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
        break;
      }
      default:
        CHECK(false);
      }
      break;
    }
    case kTIME: 
    case kTIMESTAMP:
    case kDATE: {
      auto col_data = reinterpret_cast<time_t*>(malloc(sizeof(time_t)));
      *col_data = col_datum.timeval;
      col_buffers[col_ids[col_idx]] = reinterpret_cast<int8_t*>(col_data);
      break;
    }
    default:
      CHECK(false);
    }
    ++col_idx;
  }
  for (const auto kv : col_buffers) {
    insert_data.columnIds.push_back(kv.first);
    DataBlockPtr p;
    p.numbersPtr = kv.second;
    insert_data.data.push_back(p);
  }
  for (auto& kv : str_col_buffers){
    insert_data.columnIds.push_back(kv.first);
    DataBlockPtr p;
    p.stringsPtr = &kv.second;
    insert_data.data.push_back(p);
  }
  insert_data.numRows = 1;
  const auto table_descriptor = cat.getMetadataForTable(table_id);
  table_descriptor->fragmenter->insertData(insert_data);
  cat.get_dataMgr().checkpoint();
  for (const auto kv : col_buffers) {
    free(kv.second);
  }
}

llvm::Module* makeLLVMModuleContents(llvm::Module *mod);

namespace {

llvm::Module* create_runtime_module(llvm::LLVMContext& context) {
  return makeLLVMModuleContents(new llvm::Module("empty_module", context));
}

void bind_pos_placeholders(
    const std::string& pos_fn_name,
    const bool use_resume_param,
    llvm::Function* query_func,
    llvm::Module* module) {
  for (auto it = llvm::inst_begin(query_func), e = llvm::inst_end(query_func); it != e; ++it) {
    if (!llvm::isa<llvm::CallInst>(*it)) {
      continue;
    }
    auto& pos_call = llvm::cast<llvm::CallInst>(*it);
    if (std::string(pos_call.getCalledFunction()->getName()) == pos_fn_name) {
      if (use_resume_param) {
        auto& resume_param = query_func->getArgumentList().back();
        llvm::ReplaceInstWithInst(&pos_call, llvm::CallInst::Create(
          module->getFunction(pos_fn_name + "_impl"),
          std::vector<llvm::Value*> { &resume_param }));
      } else {
        llvm::ReplaceInstWithInst(
          &pos_call, llvm::CallInst::Create(module->getFunction(pos_fn_name + "_impl")));
      }
      break;
    }
  }
}

void bind_init_group_by_buffer(llvm::Function* query_func,
                               const QueryMemoryDescriptor& query_mem_desc,
                               const ExecutorDeviceType device_type) {
  for (auto it = llvm::inst_begin(query_func), e = llvm::inst_end(query_func); it != e; ++it) {
    if (!llvm::isa<llvm::CallInst>(*it)) {
      continue;
    }
    auto& init_group_by_buffer_call = llvm::cast<llvm::CallInst>(*it);
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < init_group_by_buffer_call.getNumArgOperands(); ++i) {
      args.push_back(init_group_by_buffer_call.getArgOperand(i));
    }
    if (std::string(init_group_by_buffer_call.getCalledFunction()->getName()) == "init_group_by_buffer") {
      if (query_mem_desc.lazyInitGroups(device_type)) {
        llvm::ReplaceInstWithInst(
          &init_group_by_buffer_call,
          llvm::CallInst::Create(query_func->getParent()->getFunction("init_group_by_buffer_impl"), args));
      } else {
        // init_group_by_buffer is meaningless if groups are initialized on host
        init_group_by_buffer_call.eraseFromParent();
      }
      return;
    }
  }
}

std::vector<llvm::Value*> generate_column_heads_load(
    const int num_columns,
    llvm::Function* query_func,
    llvm::LLVMContext& context) {
  auto max_col_local_id = num_columns - 1;
  auto& fetch_bb = query_func->front();
  llvm::IRBuilder<> fetch_ir_builder(&fetch_bb);
  fetch_ir_builder.SetInsertPoint(fetch_bb.begin());
  auto& in_arg_list = query_func->getArgumentList();
  CHECK_GE(in_arg_list.size(), 4);
  auto& byte_stream_arg = in_arg_list.front();
  std::vector<llvm::Value*> col_heads;
  for (int col_id = 0; col_id <= max_col_local_id; ++col_id) {
    col_heads.emplace_back(fetch_ir_builder.CreateLoad(fetch_ir_builder.CreateGEP(
      &byte_stream_arg,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), col_id))));
  }
  return col_heads;
}

void set_row_func_argnames(llvm::Function* row_func,
                           const size_t in_col_count,
                           const size_t agg_col_count,
                           const bool hoist_literals) {
  auto arg_it = row_func->arg_begin();

  if (agg_col_count) {
    for (size_t i = 0; i < agg_col_count; ++i) {
      arg_it->setName("out");
      ++arg_it;
    }
  } else {
    arg_it->setName("group_by_buff");
    ++arg_it;
    arg_it->setName("small_group_by_buff");
    ++arg_it;
    arg_it->setName("crt_match");
    ++arg_it;
  }

  arg_it->setName("agg_init_val");
  ++arg_it;

  arg_it->setName("pos");
  ++arg_it;

  if (hoist_literals) {
    arg_it->setName("literals");
    ++arg_it;
  }

  for (size_t i = 0; i < in_col_count; ++i) {
    arg_it->setName("col_buf");
    ++arg_it;
  }
}

std::pair<llvm::Function*, std::vector<llvm::Value*>> create_row_function(
    const size_t in_col_count,
    const size_t agg_col_count,
    const bool hoist_literals,
    llvm::Function* query_func,
    llvm::Module* module,
    llvm::LLVMContext& context) {
  std::vector<llvm::Type*> row_process_arg_types;

  if (agg_col_count) {
    // output (aggregate) arguments
    for (size_t i = 0; i < agg_col_count; ++i) {
      row_process_arg_types.push_back(llvm::Type::getInt64PtrTy(context));
    }
  } else {
    // group by buffer
    row_process_arg_types.push_back(llvm::Type::getInt64PtrTy(context));
    // small group by buffer
    row_process_arg_types.push_back(llvm::Type::getInt64PtrTy(context));
    // current match count
    row_process_arg_types.push_back(llvm::Type::getInt64PtrTy(context));
  }

  // aggregate init values
  row_process_arg_types.push_back(llvm::Type::getInt64PtrTy(context));

  // position argument
  row_process_arg_types.push_back(llvm::Type::getInt64Ty(context));

  // literals buffer argument
  if (hoist_literals) {
    row_process_arg_types.push_back(llvm::Type::getInt8PtrTy(context));
  }

  // Generate the function signature and column head fetches s.t.
  // double indirection isn't needed in the inner loop
  auto col_heads = generate_column_heads_load(in_col_count, query_func, context);
  CHECK_EQ(in_col_count, col_heads.size());

  // column buffer arguments
  for (size_t i = 0; i < in_col_count; ++i) {
    row_process_arg_types.emplace_back(llvm::Type::getInt8PtrTy(context));
  }

  // generate the function
  auto ft = llvm::FunctionType::get(
    get_int_type(32, context),
    row_process_arg_types,
    false);

  auto row_func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "row_func", module);

  // set the row function argument names; for debugging purposes only
  set_row_func_argnames(row_func, in_col_count, agg_col_count, hoist_literals);

  return std::make_pair(row_func, col_heads);
}

}  // namespace

void Executor::nukeOldState(const bool allow_lazy_fetch) {
  cgen_state_.reset(new CgenState());
  plan_state_.reset(new PlanState(allow_lazy_fetch));
}

Executor::CompilationResult Executor::compilePlan(
    const Planner::Plan* plan,
    const Fragmenter_Namespace::QueryInfo& query_info,
    const std::vector<Executor::AggInfo>& agg_infos,
    const std::list<int>& scan_cols,
    const std::list<Analyzer::Expr*>& simple_quals,
    const std::list<Analyzer::Expr*>& quals,
    const bool hoist_literals,
    const ExecutorDeviceType device_type,
    const ExecutorOptLevel opt_level,
    const CudaMgr_Namespace::CudaMgr* cuda_mgr,
    const bool allow_lazy_fetch,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const size_t max_groups_buffer_entry_guess,
    const int64_t scan_limit) {
  nukeOldState(allow_lazy_fetch);

  GroupByAndAggregate group_by_and_aggregate(this, plan, query_info, row_set_mem_owner,
    max_groups_buffer_entry_guess, scan_limit);
  auto query_mem_desc = group_by_and_aggregate.getQueryMemoryDescriptor(max_groups_buffer_entry_guess);

  if (device_type == ExecutorDeviceType::GPU &&
      query_mem_desc.hash_type == GroupByColRangeType::MultiColKnownCardinality) {
    const size_t required_memory { (grid_size_x_ * query_mem_desc.getBufferSizeBytes(ExecutorDeviceType::GPU)) };
    CHECK(catalog_->get_dataMgr().cudaMgr_);
    const size_t max_memory { catalog_->get_dataMgr().cudaMgr_->deviceProperties[0].globalMem / 10 };
    cgen_state_->must_run_on_cpu_ =  required_memory > max_memory;
  }

  // Read the module template and target either CPU or GPU
  // by binding the stream position functions to the right implementation:
  // stride access for GPU, contiguous for CPU
  cgen_state_->module_ = create_runtime_module(cgen_state_->context_);

  const bool is_group_by { !query_mem_desc.group_col_widths.empty() };
  auto query_func = is_group_by
    ? query_group_by_template(cgen_state_->module_, is_nested_, hoist_literals, query_mem_desc, device_type, scan_limit)
    : query_template(cgen_state_->module_, agg_infos.size(), is_nested_, hoist_literals);
  bind_pos_placeholders("pos_start", true, query_func, cgen_state_->module_);
  bind_pos_placeholders("pos_step", false, query_func, cgen_state_->module_);
  if (is_group_by) {
    bind_init_group_by_buffer(query_func, query_mem_desc, device_type);
  }

  std::vector<llvm::Value*> col_heads;
  std::tie(cgen_state_->row_func_, col_heads) = create_row_function(
    scan_cols.size(), is_group_by ? 0 : agg_infos.size(), hoist_literals, query_func,
    cgen_state_->module_, cgen_state_->context_);
  CHECK(cgen_state_->row_func_);

  // make sure it's in-lined, we don't want register spills in the inner loop
  cgen_state_->row_func_->addAttribute(llvm::AttributeSet::FunctionIndex, llvm::Attribute::AlwaysInline);

  auto bb = llvm::BasicBlock::Create(cgen_state_->context_, "entry", cgen_state_->row_func_);
  cgen_state_->ir_builder_.SetInsertPoint(bb);

  // generate the code for the filter
  allocateLocalColumnIds(scan_cols);

  llvm::Value* filter_lv = llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(cgen_state_->context_), true);
  for (auto expr : simple_quals) {
    filter_lv = cgen_state_->ir_builder_.CreateAnd(filter_lv, toBool(codegen(expr, true, hoist_literals).front()));
  }
  for (auto expr : quals) {
    filter_lv = cgen_state_->ir_builder_.CreateAnd(filter_lv, toBool(codegen(expr, true, hoist_literals).front()));
  }
  CHECK(filter_lv->getType()->isIntegerTy(1));

  const bool needs_error_check = group_by_and_aggregate.codegen(filter_lv, device_type, hoist_literals);

  if (needs_error_check) {
    // check whether the row processing was successful; currently, it can
    // fail by running out of group by buffer slots
    bool done_splitting = false;
    for (auto bb_it = query_func->begin(); bb_it != query_func->end() && !done_splitting; ++bb_it) {
      for (auto inst_it = bb_it->begin(); inst_it != bb_it->end(); ++inst_it) {
        if (!llvm::isa<llvm::CallInst>(*inst_it)) {
          continue;
        }
        auto& filter_call = llvm::cast<llvm::CallInst>(*inst_it);
        if (std::string(filter_call.getCalledFunction()->getName()) == unique_name("row_process", is_nested_)) {
          auto next_inst_it = inst_it;
          ++next_inst_it;
          auto new_bb = bb_it->splitBasicBlock(next_inst_it);
          auto& br_instr = bb_it->back();
          llvm::IRBuilder<> ir_builder(&br_instr);
          llvm::Value* err_lv = inst_it;
          auto& error_code_arg = query_func->getArgumentList().back();
          err_lv = ir_builder.CreateCall(cgen_state_->module_->getFunction("merge_error_code"),
            std::vector<llvm::Value*> { err_lv, &error_code_arg });
          err_lv = ir_builder.CreateICmp(llvm::ICmpInst::ICMP_NE, err_lv, ll_int(int32_t(0)));
          auto& last_bb = query_func->back();
          llvm::ReplaceInstWithInst(&br_instr, llvm::BranchInst::Create(&last_bb, new_bb, err_lv));
          done_splitting = true;
          break;
        }
      }
    }
    CHECK(done_splitting);
  }

  if (!needs_error_check && cgen_state_->uses_div_) {
    bool done_div_zero_check = false;
    for (auto it = llvm::inst_begin(query_func), e = llvm::inst_end(query_func); it != e; ++it) {
      if (!llvm::isa<llvm::CallInst>(*it)) {
        continue;
      }
      auto& filter_call = llvm::cast<llvm::CallInst>(*it);
      if (std::string(filter_call.getCalledFunction()->getName()) == unique_name("row_process", is_nested_)) {
        done_div_zero_check = true;
        auto& error_code_arg = query_func->getArgumentList().back();
        ++it;
        llvm::CallInst::Create(cgen_state_->module_->getFunction("merge_error_code"),
          std::vector<llvm::Value*> { &filter_call, &error_code_arg }, "", &*it);
        break;
      }
    }
    CHECK(done_div_zero_check);
  }

  // iterate through all the instruction in the query template function and
  // replace the call to the filter placeholder with the call to the actual filter
  for (auto it = llvm::inst_begin(query_func), e = llvm::inst_end(query_func); it != e; ++it) {
    if (!llvm::isa<llvm::CallInst>(*it)) {
      continue;
    }
    auto& filter_call = llvm::cast<llvm::CallInst>(*it);
    if (std::string(filter_call.getCalledFunction()->getName()) == unique_name("row_process", is_nested_)) {
      std::vector<llvm::Value*> args;
      for (size_t i = 0; i < filter_call.getNumArgOperands(); ++i) {
        args.push_back(filter_call.getArgOperand(i));
      }
      args.insert(args.end(), col_heads.begin(), col_heads.end());
      llvm::ReplaceInstWithInst(&filter_call, llvm::CallInst::Create(cgen_state_->row_func_, args, ""));
      break;
    }
  }

  is_nested_ = false;

  for (const auto& agg_info : agg_infos) {
    plan_state_->init_agg_vals_.push_back(std::get<2>(agg_info));
  }

  if (device_type == ExecutorDeviceType::GPU && cgen_state_->must_run_on_cpu_) {
    return {};
  }

  return Executor::CompilationResult {
    device_type == ExecutorDeviceType::CPU
      ? optimizeAndCodegenCPU(query_func, hoist_literals, opt_level, cgen_state_->module_)
      : optimizeAndCodegenGPU(query_func, hoist_literals, opt_level, cgen_state_->module_,
                              is_group_by, cuda_mgr),
    cgen_state_->getLiterals(),
    query_mem_desc
  };
}

namespace {

void optimizeIR(llvm::Function* query_func, llvm::Module* module,
                const bool hoist_literals, const ExecutorOptLevel opt_level,
                const std::string& debug_dir, const std::string& debug_file) {
  llvm::legacy::PassManager pass_manager;
  pass_manager.add(llvm::createAlwaysInlinerPass());
  pass_manager.add(llvm::createPromoteMemoryToRegisterPass());
  pass_manager.add(llvm::createInstructionSimplifierPass());
  pass_manager.add(llvm::createInstructionCombiningPass());
  if (!debug_dir.empty()) {
    CHECK(!debug_file.empty());
    pass_manager.add(llvm::createDebugIRPass(false, false, debug_dir, debug_file));
  }
  if (hoist_literals) {
    pass_manager.add(llvm::createLICMPass());
  }
  if (opt_level == ExecutorOptLevel::LoopStrengthReduction) {
    pass_manager.add(llvm::createLoopStrengthReducePass());
  }
  pass_manager.run(*module);

  // optimizations might add attributes to the function
  // and libNVVM doesn't understand all of them; play it
  // safe and clear all attributes
  llvm::AttributeSet no_attributes;
  query_func->setAttributes(no_attributes);
}

template<class T>
std::string serialize_llvm_object(const T* llvm_obj) {
  std::stringstream ss;
  llvm::raw_os_ostream os(ss);
  llvm_obj->print(os);
  return ss.str();
}

}  // namespace

std::vector<void*> Executor::getCodeFromCache(
    const CodeCacheKey& key,
    const std::map<CodeCacheKey, std::pair<CodeCacheVal, llvm::Module*>>& cache) {
  auto it = cache.find(key);
  if (it != cache.end()) {
    delete cgen_state_->module_;
    cgen_state_->module_ = it->second.second;
    std::vector<void*> native_functions;
    for (auto& native_code : it->second.first) {
      native_functions.push_back(std::get<0>(native_code));
    }
    return native_functions;
  }
  return {};
}

void Executor::addCodeToCache(const CodeCacheKey& key,
                              const std::vector<std::tuple<
                                void*,
                                llvm::ExecutionEngine*,
                                GpuCompilationContext*>
                              >& native_code,
                              llvm::Module* module,
                              std::map<CodeCacheKey, std::pair<CodeCacheVal, llvm::Module*>>& cache) {
  CHECK(!native_code.empty());
  CodeCacheVal cache_val;
  for (const auto& native_func : native_code) {
    cache_val.emplace_back(std::get<0>(native_func),
      std::unique_ptr<llvm::ExecutionEngine>(std::get<1>(native_func)),
      std::unique_ptr<GpuCompilationContext>(std::get<2>(native_func)));
  }
  auto it_ok = cache.insert(std::make_pair(key, std::make_pair(std::move(cache_val), module)));
  CHECK(it_ok.second);
}

std::vector<void*> Executor::optimizeAndCodegenCPU(llvm::Function* query_func,
                                                   const bool hoist_literals,
                                                   const ExecutorOptLevel opt_level,
                                                   llvm::Module* module) {
  const CodeCacheKey key { serialize_llvm_object(query_func), serialize_llvm_object(cgen_state_->row_func_) };
  auto cached_code = getCodeFromCache(key, cpu_code_cache_);
  if (!cached_code.empty()) {
    return cached_code;
  }
  // run optimizations
  optimizeIR(query_func, module, hoist_literals, opt_level, debug_dir_, debug_file_);

  auto init_err = llvm::InitializeNativeTarget();
  CHECK(!init_err);
  llvm::InitializeAllTargetMCs();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  std::string err_str;
  llvm::EngineBuilder eb(module);
  eb.setErrorStr(&err_str);
  eb.setUseMCJIT(true);
  eb.setEngineKind(llvm::EngineKind::JIT);
  llvm::TargetOptions to;
  to.EnableFastISel = true;
  eb.setTargetOptions(to);
  auto execution_engine = eb.create();
  CHECK(execution_engine);

  if (llvm::verifyFunction(*query_func)) {
    LOG(FATAL) << "Generated invalid code. ";
  }

  execution_engine->finalizeObject();

  auto native_code = execution_engine->getPointerToFunction(query_func);
  CHECK(native_code);
  addCodeToCache(key, { { std::make_tuple(native_code, execution_engine, nullptr) } }, module, cpu_code_cache_);

  return { native_code };
}

namespace {

const std::string cuda_llir_prologue =
R"(
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v32:32:32-v64:64:64-v128:128:128-n16:32:64"
target triple = "nvptx64-nvidia-cuda"

declare i32 @pos_start_impl(i32*);
declare i32 @pos_step_impl();
declare i8 @thread_warp_idx(i8);
declare i64* @init_shared_mem(i64*, i32);
declare i64* @init_shared_mem_nop(i64*, i32);
declare void @write_back(i64*, i64*, i32);
declare void @write_back_nop(i64*, i64*, i32);
declare void @init_group_by_buffer_impl(i64*, i64*, i32, i32, i32);
declare i64* @get_group_value(i64*, i32, i64*, i32, i32, i64*);
declare i64* @get_group_value_fast(i64*, i64, i64, i32);
declare i64* @get_group_value_one_key(i64*, i32, i64*, i32, i64, i64, i32, i64*);
declare void @agg_count_shared(i64*, i64);
declare void @agg_count_skip_val_shared(i64*, i64, i64);
declare void @agg_count_double_shared(i64*, double);
declare void @agg_sum_shared(i64*, i64);
declare void @agg_sum_skip_val_shared(i64*, i64, i64);
declare void @agg_sum_double_shared(i64*, double);
declare void @agg_max_shared(i64*, i64);
declare void @agg_max_skip_val_shared(i64*, i64, i64);
declare void @agg_max_double_shared(i64*, double);
declare void @agg_min_shared(i64*, i64);
declare void @agg_min_skip_val_shared(i64*, i64, i64);
declare void @agg_min_double_shared(i64*, double);
declare void @agg_id_shared(i64*, i64);
declare void @agg_id_double_shared(i64*, double);
declare i64 @ExtractFromTime(i32, i64);
declare i64 @string_decode(i8*, i64);
declare i1 @string_like(i8*, i32, i8*, i32, i8);
declare i1 @string_ilike(i8*, i32, i8*, i32, i8);
declare i1 @string_lt(i8*, i32, i8*, i32);
declare i1 @string_le(i8*, i32, i8*, i32);
declare i1 @string_gt(i8*, i32, i8*, i32);
declare i1 @string_ge(i8*, i32, i8*, i32);
declare i1 @string_eq(i8*, i32, i8*, i32);
declare i1 @string_ne(i8*, i32, i8*, i32);
declare i32 @merge_error_code(i32, i32*);

)";

}  // namespace

std::vector<void*> Executor::optimizeAndCodegenGPU(llvm::Function* query_func,
                                                   const bool hoist_literals,
                                                   const ExecutorOptLevel opt_level,
                                                   llvm::Module* module,
                                                   const bool no_inline,
                                                   const CudaMgr_Namespace::CudaMgr* cuda_mgr) {
  CHECK(cuda_mgr);
  const CodeCacheKey key { serialize_llvm_object(query_func), serialize_llvm_object(cgen_state_->row_func_) };
  auto cached_code = getCodeFromCache(key, gpu_code_cache_);
  if (!cached_code.empty()) {
    return cached_code;
  }

  auto get_group_value_func = module->getFunction("get_group_value_one_key");
  CHECK(get_group_value_func);
  get_group_value_func->setAttributes(llvm::AttributeSet {});

  bool row_func_not_inlined = false;
  if (no_inline) {
    for (auto it = llvm::inst_begin(cgen_state_->row_func_), e = llvm::inst_end(cgen_state_->row_func_);
         it != e; ++it) {
      if (llvm::isa<llvm::CallInst>(*it)) {
        auto& get_gv_call = llvm::cast<llvm::CallInst>(*it);
        if (get_gv_call.getCalledFunction()->getName() == "get_group_value" ||
            get_gv_call.getCalledFunction()->getName() == "string_decode") {
          llvm::AttributeSet no_inline_attrs;
          no_inline_attrs = no_inline_attrs.addAttribute(cgen_state_->context_, 0, llvm::Attribute::NoInline);
          cgen_state_->row_func_->setAttributes(no_inline_attrs);
          row_func_not_inlined = true;
          break;
        }
      }
    }
  }

  // run optimizations
  optimizeIR(query_func, module, hoist_literals, opt_level, "", "");

  std::stringstream ss;
  llvm::raw_os_ostream os(ss);

  for (const auto like_pattern : cgen_state_->str_constants_) {
    like_pattern->print(os);
  }

  if (row_func_not_inlined) {
    llvm::AttributeSet no_attributes;
    cgen_state_->row_func_->setAttributes(no_attributes);
    cgen_state_->row_func_->print(os);
  }

  query_func->print(os);

  char nvvm_annotations[1024];
  auto func_name = query_func->getName().str();
  snprintf(nvvm_annotations, sizeof(nvvm_annotations), hoist_literals ?
R"(
!nvvm.annotations = !{!0}
!0 = metadata !{void (i8**,
                      i8*,
                      i64*,
                      i64*,
                      i64*,
                      i64**,
                      i64**,
                      i32*)* @%s, metadata !"kernel", i32 1}
)" :
R"(
!nvvm.annotations = !{!0}
!0 = metadata !{void (i8**,
                      i64*,
                      i64*,
                      i64*,
                      i64**,
                      i64**,
                      i32*)* @%s, metadata !"kernel", i32 1}
)", func_name.c_str());

  auto cuda_llir = cuda_llir_prologue + ss.str() +
    std::string(nvvm_annotations);

  std::vector<void*> native_functions;
  std::vector<std::tuple<void*, llvm::ExecutionEngine*, GpuCompilationContext*>> cached_functions;

  for (int device_id = 0; device_id < cuda_mgr->getDeviceCount(); ++device_id) {
    auto gpu_context = new GpuCompilationContext(cuda_llir, func_name, "./QueryEngine/cuda_mapd_rt.a",
      device_id, cuda_mgr, block_size_x_);
    auto native_code = gpu_context->kernel();
    CHECK(native_code);
    native_functions.push_back(native_code);
    cached_functions.emplace_back(native_code, nullptr, gpu_context);
  }

  addCodeToCache(key, cached_functions, module, gpu_code_cache_);

  return native_functions;
}

int8_t Executor::warpSize() const {
  CHECK(catalog_->get_dataMgr().cudaMgr_);
  const auto& dev_props = catalog_->get_dataMgr().cudaMgr_->deviceProperties;
  CHECK(!dev_props.empty());
  return dev_props.front().warpSize;
}

llvm::Value* Executor::toDoublePrecision(llvm::Value* val) {
  if (val->getType()->isIntegerTy()) {
    auto val_width = static_cast<llvm::IntegerType*>(val->getType())->getBitWidth();
    CHECK_LE(val_width, 64);
    const auto cast_op = val_width == 1
      ? llvm::Instruction::CastOps::ZExt
      : llvm::Instruction::CastOps::SExt;
    return val_width < 64
      ? cgen_state_->ir_builder_.CreateCast(cast_op, val, get_int_type(64, cgen_state_->context_))
      : val;
  }
  // real (not dictionary-encoded) strings; store the pointer to the payload
  if (val->getType()->isPointerTy()) {
    const auto val_ptr_type = static_cast<llvm::PointerType*>(val->getType());
    CHECK(val_ptr_type->getElementType()->isIntegerTy(8));
    return cgen_state_->ir_builder_.CreatePointerCast(val, get_int_type(64, cgen_state_->context_));
  }
  CHECK(val->getType()->isFloatTy() || val->getType()->isDoubleTy());
  return val->getType()->isFloatTy()
    ? cgen_state_->ir_builder_.CreateFPExt(val, llvm::Type::getDoubleTy(cgen_state_->context_))
    : val;
}

llvm::Value* Executor::groupByColumnCodegen(Analyzer::Expr* group_by_col,
                                            const bool hoist_literals) {
  auto group_key = codegen(group_by_col, true, hoist_literals).front();
  cgen_state_->group_by_expr_cache_.push_back(group_key);
  group_key = cgen_state_->ir_builder_.CreateBitCast(
    toDoublePrecision(group_key), get_int_type(64, cgen_state_->context_));
  return group_key;
}

void Executor::allocateLocalColumnIds(const std::list<int>& global_col_ids) {
  for (const int col_id : global_col_ids) {
    const auto local_col_id = plan_state_->global_to_local_col_ids_.size();
    const auto it_ok = plan_state_->global_to_local_col_ids_.insert(std::make_pair(col_id, local_col_id));
    plan_state_->local_to_global_col_ids_.push_back(col_id);
    // enforce uniqueness of the column ids in the scan plan
    CHECK(it_ok.second);
  }
}

int Executor::getLocalColumnId(const int global_col_id, const bool fetch_column) const {
  const auto it = plan_state_->global_to_local_col_ids_.find(global_col_id);
  CHECK(it != plan_state_->global_to_local_col_ids_.end());
  if (fetch_column) {
    plan_state_->columns_to_fetch_.insert(global_col_id);
  }
  return it->second;
}

bool Executor::skipFragment(
    const Fragmenter_Namespace::FragmentInfo& fragment,
    const std::list<Analyzer::Expr*>& simple_quals) {
  for (const auto simple_qual : simple_quals) {
    const auto comp_expr = dynamic_cast<const Analyzer::BinOper*>(simple_qual);
    if (!comp_expr) {
      // is this possible?
      return false;
    }
    const auto lhs = comp_expr->get_left_operand();
    const auto lhs_col = dynamic_cast<const Analyzer::ColumnVar*>(lhs);
    if (!lhs_col || !lhs_col->get_table_id() || lhs_col->get_rte_idx()) {
      return false;
    }
    const auto rhs = comp_expr->get_right_operand();
    const auto rhs_const = dynamic_cast<const Analyzer::Constant*>(rhs);
    if (!rhs_const) {
      // is this possible?
      return false;
    }
    if (lhs->get_type_info() != rhs->get_type_info()) {
      // is this possible?
      return false;
    }
    if (!lhs->get_type_info().is_integer() && !lhs->get_type_info().is_time()) {
      return false;
    }
    const int col_id = lhs_col->get_column_id();
    auto chunk_meta_it = fragment.chunkMetadataMap.find(col_id);
    CHECK(chunk_meta_it != fragment.chunkMetadataMap.end());
    const auto& chunk_type = lhs->get_type_info();
    const auto chunk_min = extract_min_stat(chunk_meta_it->second.chunkStats, chunk_type);
    const auto chunk_max = extract_max_stat(chunk_meta_it->second.chunkStats, chunk_type);
    const auto rhs_val = codegenIntConst(rhs_const)->getSExtValue();
    switch (comp_expr->get_optype()) {
    case kGE:
      if (chunk_max < rhs_val) {
        return true;
      }
      break;
    case kGT:
      if (chunk_max <= rhs_val) {
        return true;
      }
      break;
    case kLE:
      if (chunk_min > rhs_val) {
        return true;
      }
      break;
    case kLT:
      if (chunk_min >= rhs_val) {
        return true;
      }
      break;
    default:
      break;
    }
  }
  return false;
}

std::map<std::tuple<int, size_t, size_t>, std::shared_ptr<Executor>> Executor::executors_;
std::mutex Executor::execute_mutex_;
