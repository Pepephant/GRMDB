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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    std::unique_ptr<IxScan> ix_scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        Iid lower, upper;
        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        auto ih = sm_manager_->ihs_.at(ix_name).get();
        auto bpm = sm_manager_->get_bpm();

        ShowCondition();
        GetBound(&lower, &upper, ih);
        std::fstream outfile;
        outfile.open("output.txt", std::ios::out | std::ios::app);
        outfile << ih->leaf_begin().page_no << ", " << ih->leaf_end().page_no << ", " << ih->leaf_end().slot_no << ", ";
        outfile << "(" << lower.page_no << "," << lower.slot_no << "), ";
        outfile << "(" << upper.page_no << "," << upper.slot_no << ")\n";
        ix_scan_ = std::make_unique<IxScan>(ih, lower, upper, bpm);
        rid_ = ix_scan_->rid();
    }

    void nextTuple() override {
        ix_scan_->next();
        if (!ix_scan_->is_end()) {
            rid_ = ix_scan_->rid();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return sm_manager_->fhs_.at(tab_name_)->get_record(rid_, nullptr);
    }

    bool is_end() const override { return ix_scan_->is_end(); };

    size_t tupleLen() const override { return len_; };

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    };

    Rid &rid() override { return rid_; }

private:
    auto Op2str(CompOp op) -> std::string {
        std::map<CompOp, std::string> swap_op = {
                {OP_EQ, "="}, {OP_NE, "!="}, {OP_LT, ">"}, {OP_GT, "<"}, {OP_LE, ">="}, {OP_GE, "<="},
        };
        return swap_op.at(op);
    }

    void ShowCondition() {
        std::fstream outfile;
        outfile.open("output.txt", std::ios::out | std::ios::app);

        outfile << "\nUse Index: ";
        std::cout << "Use Index: ";
        for (auto& index_col_name : index_col_names_) {
            std::cout << index_col_name << " ";
            outfile << index_col_name << " ";
        }
        std::cout << "\n";
        outfile << "\n";
        outfile.close();

        for (auto& cond: conds_) {
            std::cout << cond.lhs_col.col_name << Op2str(cond.op);
            if (cond.is_rhs_val) {
                switch (cond.rhs_val.type) {
                    case TYPE_INT:
                        std::cout << cond.rhs_val.int_val;
                        break;
                    case TYPE_FLOAT:
                        std::cout << cond.rhs_val.float_val;
                        break;
                    case TYPE_STRING:
                        std::cout << cond.rhs_val.str_val;
                        break;
                    default:
                        break;
                }
            } else {
                std::cout << cond.rhs_col.col_name;
            }
            std::cout << ",  ";
        }
        std::cout << "\n";
    }

    void GetBound(Iid* lower, Iid* upper, IxIndexHandle* ih) {
        if (conds_.size() == 0) {
            *lower = ih->leaf_begin();
            *upper = ih->leaf_end();
        }

        if (conds_.size() == 1) {
            GetBoundSingle(lower, upper, ih);
            return ;
        }

        if (conds_.size() == 2) {
            if (conds_[0].lhs_col.col_name == conds_[1].lhs_col.col_name) {
                GetBoundSingle(lower, upper, ih);
                return ;
            }
        }

        GetBoundMultiple(lower, upper, ih);
    }

    void GetBoundMultiple(Iid* lower, Iid* upper, IxIndexHandle* ih) {
        int key_size = index_meta_.col_tot_len;
        auto tab_meta = sm_manager_->db_.get_table(tab_name_);
        char* key = new char[key_size];
        memset(key, 0, key_size);
        int i = 0;
        int offset = 0;
        int num_key = static_cast<int>(conds_.size());

        for (auto& cond: conds_) {
            int col_len = tab_meta.get_col(cond.lhs_col.col_name)->len;
            memcpy(key + offset, cond.rhs_val.raw->data, col_len);

            if (i == num_key - 1) {
                switch (cond.op) {
                    case OP_GT:
                        *lower = ih->upper_bound(key, num_key);
                        *upper = ih->leaf_end();
                        break;
                    case OP_GE:
                        *lower = ih->lower_bound(key, num_key);
                        *upper = ih->leaf_end();
                        break;
                    case OP_LT:
                        *lower = ih->leaf_begin();
                        *upper = ih->lower_bound(key, num_key);
                        break;
                    case OP_LE:
                        *lower = ih->leaf_begin();
                        *upper = ih->upper_bound(key, num_key);
                        break;
                    case OP_EQ:
                        *lower = ih->lower_bound(key, num_key);
                        *upper = ih->upper_bound(key, num_key);
                    default:
                        break;
                }
            }

            offset += col_len;
            i++;
        }
    }

    void GetBoundSingle(Iid* lower, Iid* upper, IxIndexHandle* ih) {
        int key_size = index_meta_.col_tot_len;
        auto tab_meta = sm_manager_->db_.get_table(tab_name_);
        char* key = new char[key_size];
        int num_key = 1;

        if (conds_.size() == 1) {
            auto cond = conds_[0];
            int col_len = tab_meta.get_col(cond.lhs_col.col_name)->len;

            memset(key, 0, key_size);
            memcpy(key, cond.rhs_val.raw->data, col_len);

            if (cond.op == OP_EQ) {
                *lower = ih->lower_bound(key, num_key);
                *upper = ih->upper_bound(key, num_key);
            } else if (cond.op == OP_LE) {
                *lower = ih->leaf_begin();
                *upper = ih->upper_bound(key, num_key);
            } else if (cond.op == OP_LT) {
                *lower = ih->leaf_begin();
                *upper = ih->lower_bound(key, num_key);
            } else if (cond.op == OP_GE) {
                *lower = ih->lower_bound(key, num_key);
                *upper = ih->leaf_end();
            } else if (cond.op == OP_GT) {
                *lower = ih->upper_bound(key, num_key);
                *upper = ih->leaf_end();
            }
        } else {
            for (auto& cond: conds_) {
                int col_len = tab_meta.get_col(cond.lhs_col.col_name)->len;
                memset(key, 0, key_size);
                memcpy(key, cond.rhs_val.raw->data, col_len);
                switch (cond.op) {
                    case OP_GT:
                        *lower = ih->upper_bound(key, num_key); break;
                    case OP_GE:
                        *lower = ih->lower_bound(key, num_key); break;
                    case OP_LT:
                        *upper = ih->lower_bound(key, num_key); break;
                    case OP_LE:
                        *upper = ih->upper_bound(key, num_key); break;
                    default:
                        break;
                }
            }
        }
    }
};