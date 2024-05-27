/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    /*My fields*/
    std::unique_ptr<RmRecord> record_;
    std::vector<ColMeta> left_cols_;
    std::vector<ColMeta> right_cols_;
    std::vector<ColMeta> all_cols_;
   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        std::swap(left_, right_);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

        left_cols_ = left_->cols();
        right_cols_ = right_->cols();
        all_cols_.insert(all_cols_.end(), left_cols_.begin(), left_cols_.end());
        all_cols_.insert(all_cols_.end(), right_cols_.begin(), right_cols_.end());
    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();

        while (!left_->is_end()) {
            auto left_tuple = left_->Next();

            if (right_->is_end()) {
                left_->nextTuple();
                right_->beginTuple();
                continue;
            }

            while (!right_->is_end()) {
                auto right_tuple = right_->Next();
                right_->nextTuple();
                auto tuple = make_record(left_tuple.get(), right_tuple.get());
                if (evaluate_join(tuple.get())) {
                    record_ = std::move(tuple);
                    return ;
                }
            }
        }

        isend = true;
    }

    void nextTuple() override {

        while (!left_->is_end()) {
            auto left_tuple = left_->Next();

            if (right_->is_end()) {
                left_->nextTuple();
                right_->beginTuple();
                continue;
            }

            while (!right_->is_end()) {
                auto right_tuple = right_->Next();
                right_->nextTuple();
                auto tuple = make_record(left_tuple.get(), right_tuple.get());
                if (evaluate_join(tuple.get())) {
                    record_ = std::move(tuple);
                    return ;
                }
            }
        }

        isend = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(record_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override {
        return isend;
    }

    const std::vector<ColMeta> & cols() const override {
        return cols_;
    }

    size_t tupleLen() const override {
        return len_;
    }

private:
    bool evaluate_join(RmRecord* tuple) {

        for (auto& cond: fed_conds_) {
            auto left = getValue(cond.lhs_col, tuple);
            Value right{};
            if (cond.is_rhs_val) {
                right = cond.rhs_val;
            } else {
                right = getValue(cond.rhs_col, tuple);
            }

            bool pred_cond = false;
            if (left.type == TYPE_INT && right.type == TYPE_INT) {
                pred_cond = ValueComp(left.int_val, right.int_val, cond.op);
            } else if (left.type == TYPE_FLOAT && right.type == TYPE_INT) {
                pred_cond = ValueComp(left.float_val, static_cast<float>(right.int_val), cond.op);
            } else if (left.type == TYPE_INT && right.type == TYPE_FLOAT) {
                pred_cond = ValueComp(static_cast<float>(left.int_val), right.float_val, cond.op);
            } else if (left.type == TYPE_FLOAT && right.type == TYPE_FLOAT) {
                pred_cond = ValueComp(left.float_val, right.float_val, cond.op);
            } else if (left.type == TYPE_STRING && right.type == TYPE_STRING) {
                pred_cond = ValueComp(left.str_val, right.str_val, cond.op);
            } else {
                auto lhs = cond.lhs_col.tab_name + ":" + cond.lhs_col.col_name;
                auto rhs = cond.rhs_col.tab_name + ":" + cond.rhs_col.col_name;
                throw IncompatibleTypeError(lhs, rhs);
            }

            if (!pred_cond) { return false; }
        }
        return true;
    }

    std::unique_ptr<RmRecord> make_record(RmRecord* tuple1, RmRecord* tuple2) {
        int new_size = tuple1->size + tuple2->size;
        RmRecord tuple(new_size);
        char* new_data = tuple.data;

        memcpy(new_data, tuple1->data, tuple1->size);
        memcpy(new_data + tuple1->size, tuple2->data, tuple2->size);

        return std::make_unique<RmRecord>(tuple);
    }

    Value getValue(TabCol col, RmRecord* tuple) {
        auto col_iter = get_col(cols_, col);
        auto col_meta = cols_[col_iter - cols_.begin()];
        char* col_raw = new char[col_meta.len];
        memset(col_raw, 0, col_meta.len);
        memcpy(col_raw, tuple->data + col_meta.offset, col_meta.len);

        Value val{};
        val.type = col_meta.type;

        if (col_meta.type == TYPE_INT) {
            auto int_val = *(int *)(col_raw);
            val.int_val = int_val;
        } else if (col_meta.type == TYPE_FLOAT) {
            auto float_val = *(float *)(col_raw);
            val.float_val = float_val;
        } else if (col_meta.type == TYPE_STRING) {
            auto str_val = std::string((char *)col_raw, col_meta.len);
            str_val.resize(strlen(str_val.c_str()));
            val.str_val = str_val;
        }

        return val;
    }

    inline bool ValueComp(int left, int right, CompOp op) {
        switch (op) {
            case OP_EQ: return left == right;
            case OP_NE: return left != right;
            case OP_GE: return left >= right;
            case OP_LE: return left <= right;
            case OP_GT: return left > right;
            case OP_LT: return left < right;
            default: break;
        }
        return false;
    }

    inline bool ValueComp(float left, float right, CompOp op) {
        switch (op) {
            case OP_EQ: return left == right;
            case OP_NE: return left != right;
            case OP_GE: return left >= right;
            case OP_LE: return left <= right;
            case OP_GT: return left > right;
            case OP_LT: return left < right;
            default: break;
        }
        return false;
    }

    inline bool ValueComp(std::string left, std::string right, CompOp op) {
        switch (op) {
            case OP_EQ: return left == right;
            case OP_NE: return left != right;
            case OP_GE: return left >= right;
            case OP_LE: return left <= right;
            case OP_GT: return left > right;
            case OP_LT: return left < right;
            default: break;
        }
        return false;
    }
};