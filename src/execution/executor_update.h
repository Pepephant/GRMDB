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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

    /*My fields*/
    std::vector<ColMeta> cols_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        cols_ = tab_.cols;
        for (auto& rid: rids_) {
            auto tuple = fh_->get_record(rid, context_);
            RmRecord rec = *(tuple);

            if (!eval_condition(conds_, tuple.get())) {
                continue;
            }

            for (auto &clause: set_clauses_) {
                auto new_val = clause.rhs;
                auto col_meta = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta &col) {
                    return col.name == clause.lhs.col_name;
                });
                auto offset = col_meta->offset;
                new_val.init_raw(col_meta->len);
                memcpy(rec.data + offset, new_val.raw->data, col_meta->len);
            }

            fh_->update_record(rid, rec.data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

private:
    Value getValue(TabCol col, RmRecord* tuple) {
        auto col_iter = get_col(cols_, col);
        auto col_meta = cols_[col_iter - cols_.begin()];
        char* col_raw = new char[col_meta.len];
        memset(col_raw, 0, col_meta.len);
        memcpy(col_raw, tuple->data + col_meta.offset, col_meta.len);

        if (col_meta.type == TYPE_INT) {
            auto int_val = *(int *)(col_raw);
            return Value{.type = TYPE_INT, .int_val = int_val};
        } else if (col_meta.type == TYPE_FLOAT) {
            auto float_val = *(float *)(col_raw);
            return Value{.type = TYPE_FLOAT, .float_val = float_val};
        } else if (col_meta.type == TYPE_STRING) {
            auto str_val = std::string((char *)col_raw, col_meta.len);
            str_val.resize(strlen(str_val.c_str()));
            return Value{.type = TYPE_STRING, .str_val = str_val};
        }

        return Value{};
    }

    bool eval_condition(std::vector<Condition> conds, RmRecord* tuple) {
        for (auto& cond: conds_) {
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