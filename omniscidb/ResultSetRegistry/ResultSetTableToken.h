/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "ResultSetTable.h"

#include "DataMgr/ChunkMetadata.h"
#include "SchemaMgr/TableInfo.h"

#include "arrow/api.h"

namespace hdk {

class ResultSetRegistry;
class ResultSetTableToken;

using ResultSetTableTokenPtr = std::shared_ptr<const ResultSetTableToken>;

class ResultSetTableToken : public std::enable_shared_from_this<ResultSetTableToken> {
 public:
  ResultSetTableToken() = default;
  ResultSetTableToken(TableInfoPtr tinfo,
                      size_t row_count,
                      std::shared_ptr<ResultSetRegistry> registry);

  ResultSetTableToken(ResultSetTableToken&& other) = default;
  ResultSetTableToken& operator=(ResultSetTableToken&& other) = default;

  ResultSetTableToken(const ResultSetTableToken& other) = delete;
  ResultSetTableToken& operator=(const ResultSetTableToken& other) = delete;

  ~ResultSetTableToken();

  bool empty() const { return !registry_; }

  TableInfoPtr tableInfo() const { return tinfo_; }

  int dbId() const { return tinfo_->db_id; }
  int tableId() const { return tinfo_->table_id; }

  // Column ID <-> Index mapping. Use a significant offset to early catch
  // cases of indexes used as IDs and vice versa.
  static int columnId(size_t col_idx) { return static_cast<int>(col_idx + 2000); }
  static size_t columnIndex(int col_id) { return static_cast<size_t>(col_id - 2000); }

  size_t rowCount() const { return row_count_; }

  size_t resultSetCount() const { return tinfo_->fragments; }
  ResultSetPtr resultSet(size_t idx) const;

  const std::string& tableName() const { return tinfo_->name; }

  ResultSetTableTokenPtr head(size_t n) const;
  ResultSetTableTokenPtr tail(size_t n) const;

  std::shared_ptr<arrow::Table> toArrow() const;

  std::vector<TargetValue> row(size_t row_idx,
                               bool translate_strings,
                               bool decimal_to_double) const;

  std::string toString() const {
    return "ResultSetTableToken(" + std::to_string(dbId()) + ":" +
           std::to_string(tableId()) + ")";
  }

  std::string description() const;
  std::string memoryDescription() const;
  std::string contentToString(bool header) const;

 private:
  void reset();

  TableInfoPtr tinfo_;
  size_t row_count_;
  std::shared_ptr<ResultSetRegistry> registry_;
};

}  // namespace hdk
