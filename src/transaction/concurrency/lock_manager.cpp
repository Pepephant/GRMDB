/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

#include <algorithm>

bool LockManager::lock(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn->get_state() == TransactionState::ABORTED) {
        return false;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    std::unique_lock<std::mutex> lock(latch_);

    // Find or create the lock request queue for this lock_data_id
    if (lock_table_.find(lock_data_id) == lock_table_.end()) {
        auto lock_request_que = std::make_shared<LockRequestQueue>();
        lock_table_[lock_data_id] = lock_request_que;
    }
    auto queue = lock_table_[lock_data_id];
    lock.unlock();
    // add que->latch_
    std::unique_lock<std::mutex> lck(queue->latch_);
    // Check if the transaction already holds a lock on this lock_data_id
    auto it = std::find_if(queue->request_queue_.begin(), queue->request_queue_.end(),
                           [txn](const std::shared_ptr<LockRequest> &req) {
                               return req->txn_id_ == txn->get_transaction_id();
                           });

    if (it != queue->request_queue_.end()) {
        // If the transaction already holds a lock, check if an upgrade is needed
        if ((*it)->lock_mode_ != lock_mode) {
            // Upgrade the lock
            if (can_upgrade((*it)->lock_mode_, lock_mode)) {
                if(queue->upgrading_ != INVALID_TXN_ID) {
                    txn->set_state(TransactionState::ABORTED);
                    throw TransactionAbortException(txn->get_transaction_id(),AbortReason::UPGRADE_CONFLICT);
                }
                queue->upgrading_ = txn->get_transaction_id();
                // whether other transaction have the group_lock
                bool is_other_group_lock = false;
                for (const auto &item: queue->request_queue_) {
                    if (item->txn_id_ != (*it)->txn_id_) {
                        is_other_group_lock = is_group_lock(item->lock_mode_, queue->group_lock_mode_);
                    }
                }
                if (!is_other_group_lock) {
                    (*it)->lock_mode_ = lock_mode;
                    (*it)->granted_ = true;
                    queue->group_lock_mode_ = calculate_group_lock_mode(queue->group_lock_mode_,(*it)->lock_mode_);
                    if (queue->upgrading_ == txn->get_transaction_id()) {
                        queue->upgrading_ = INVALID_TXN_ID;
                    }
                    return true;
                } else if (is_compatible(queue->group_lock_mode_, lock_mode)) {
                    (*it)->lock_mode_ = lock_mode;
                    (*it)->granted_ = true;
                    queue->group_lock_mode_ = calculate_group_lock_mode(queue->group_lock_mode_,(*it)->lock_mode_);
                    if (queue->upgrading_ == txn->get_transaction_id()) {
                        queue->upgrading_ = INVALID_TXN_ID;
                    }
                    return true;
                } else {
                    // Wait-Die strategy for lock upgrade
                    (*it)->granted_ = false;
                    (*it)->lock_mode_ = lock_mode;
                    wait_die(*queue, *(*it), txn);
                    // Wait for the lock to be granted
                    queue->cv_.wait(lck, [&]() { return (*it)->granted_; });
                    queue->group_lock_mode_ = calculate_group_lock_mode(queue->group_lock_mode_,(*it)->lock_mode_);
                    if (queue->upgrading_ == txn->get_transaction_id()) {
                        queue->upgrading_ = INVALID_TXN_ID;
                    }
                    return true;
                }
            }else{
                // Already holding the higher lock
                return true;
            }
        } else {
            // Already holding the requested lock
            return true;
        }
    }

    // Create a new lock request
    auto request = std::make_shared<LockRequest>(txn->get_transaction_id(), lock_mode);
    if (queue->request_queue_.empty()) {
        request->granted_ = true;
        queue->request_queue_.push_back(request);
        queue->group_lock_mode_ = calculate_group_lock_mode(queue->group_lock_mode_,lock_mode);
        txn->get_lock_set()->emplace(lock_data_id);
        return true;
    }
    // Add the request to the queue
    queue->request_queue_.push_back(request);
    if (is_compatible(queue->group_lock_mode_, lock_mode)) {
        request->granted_ = true;
        queue->group_lock_mode_ = calculate_group_lock_mode(queue->group_lock_mode_,lock_mode);
    } else {
        // Wait-die strategy for deadlock prevention
        wait_die(*queue, *request, txn);// Wait for the lock to be granted
        queue->cv_.wait(lck, [&]() { return request->granted_; });
        queue->group_lock_mode_ = calculate_group_lock_mode(queue->group_lock_mode_,request->lock_mode_);
    }
    //request->granted_ = true;
    txn->get_lock_set()->emplace(lock_data_id);
    return true;
}

void LockManager::wait_die(LockRequestQueue &request_queue, LockRequest &request, Transaction *txn) {
    txn_id_t txn_id = request.txn_id_;
    auto it = request_queue.request_queue_.begin();

    while (it != request_queue.request_queue_.end()) {
        if (txn->get_transaction_id() > (*it)->txn_id_) {
            // Current transaction has lower priority, abort it (die)
            txn->set_state(TransactionState::ABORTED);
            if(request_queue.upgrading_ == txn->get_transaction_id()) {
                request_queue.upgrading_ = INVALID_TXN_ID;
            }
            request_queue.request_queue_.erase(
                    remove_if(request_queue.request_queue_.begin(), request_queue.request_queue_.end(),
                              [txn_id](const std::shared_ptr<LockRequest> &req) { return req->txn_id_ == txn_id; }),
                    request_queue.request_queue_.end());

            throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
        }
        ++it;
    }
}

void LockManager::grant_lock(LockRequestQueue &request_queue) {
    for (auto &request: request_queue.request_queue_) {
        if (!request->granted_) {
            if (is_compatible(request_queue.group_lock_mode_, request->lock_mode_)) {
                request->granted_ = true;
                request_queue.group_lock_mode_ = calculate_group_lock_mode(request_queue.group_lock_mode_,request->lock_mode_);
            }else if(request->txn_id_ == request_queue.upgrading_) {
                request->granted_ = true;
                request_queue.group_lock_mode_ = calculate_group_lock_mode(request_queue.group_lock_mode_,request->lock_mode_);
            }
        }
    }
    request_queue.cv_.notify_all();
}

bool LockManager::can_upgrade(LockMode current_mode, LockMode requested_mode) {

    // Define valid upgrade paths
    static const bool upgrade_matrix[5][5] = {
            //    IS     IX      S      X     SIX
            /* IS */ {false, true,  false, true,  true},
            /* IX */
                     {false, false, false, true,  true},
            /* S  */
                     {false, false, false, true,  true},
            /* X  */
                     {false, false, false, false, false},
            /* SIX*/
                     {false, false, false, false, false}
    };

    return upgrade_matrix[static_cast<int>(current_mode)][static_cast<int>(requested_mode)];
}

LockManager::GroupLockMode LockManager::calculate_group_lock_mode(GroupLockMode group_lock_mode, LockMode lock_mode_) {
    switch (lock_mode_) {
        case LockMode::EXLUCSIVE:
            return GroupLockMode::X;
        case LockMode::S_IX:
            if(group_lock_mode != GroupLockMode::X){
                return GroupLockMode::SIX;
            }
        case LockMode::INTENTION_EXCLUSIVE:
            if (group_lock_mode != GroupLockMode::X && group_lock_mode != GroupLockMode::SIX) {
                return GroupLockMode::IX;
            }
        case LockMode::SHARED:
            if (group_lock_mode != GroupLockMode::X && group_lock_mode != GroupLockMode::SIX && group_lock_mode != GroupLockMode::IX) {
                return GroupLockMode::S;
            }
        case LockMode::INTENTION_SHARED:
            if (group_lock_mode == GroupLockMode::NON_LOCK) {
                return GroupLockMode::IS;
            }
        default:
            return group_lock_mode;
    }
}

bool LockManager::is_compatible(GroupLockMode group_lock_mode, LockMode lock_mode) {
    // Compatibility matrix
    static const bool compatibility_matrix[6][5] = {
            //   IS    IX    S     X     SIX
            {true,  true,  true,  true, true},  // NON_LOCK
            {true,  true,  true,  false, false},  // IS
            {true,  true,  false, false, false}, // IX
            {true,  false, true,  false, false}, // S
            {false, false, false, false, false}, // X
            {false, false, false, false, false}  // SIX
    };

    return compatibility_matrix[static_cast<int>(group_lock_mode)][static_cast<int>(lock_mode)];
}

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    // 封装锁数据标识符 (LockDataId)
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);

    // 申请共享锁
    return lock(txn, lock_data_id, LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    return lock(txn, lock_data_id, LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction *txn, int tab_fd) {
    // 封装锁数据标识符 (LockDataId)
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    // 调用内部的通用锁函数，申请共享锁
    return lock(txn, lock_data_id, LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction *txn, int tab_fd) {
    // 封装锁数据标识符 (LockDataId)
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    // 申请排他锁
    return lock(txn, lock_data_id, LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction *txn, int tab_fd) {
    // 封装锁数据标识符 (LockDataId)
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);

    // 调用内部的通用锁函数，申请意向共享锁 (IS)
    return lock(txn, lock_data_id, LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction *txn, int tab_fd) {
    // 封装锁数据标识符 (LockDataId)
    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);

    // 调用内部的通用锁函数，申请意向排他锁 (IX)
    return lock(txn, lock_data_id, LockMode::INTENTION_EXCLUSIVE);
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction *txn, LockDataId lock_data_id) {
//    if (txn->get_state() != TransactionState::COMMITTED && txn->get_state() != TransactionState::ABORTED) {
//        txn->set_state(TransactionState::ABORTED);
//        return false;
//    }
    // txn->set_state(TransactionState::SHRINKING);
    std::unique_lock<std::mutex> lock(latch_);

    // 查找锁请求队列
    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        lock.unlock();
        return false; // 如果找不到锁请求队列，返回 false
    }
    lock.unlock();

    std::unique_lock<std::mutex> lck(it->second->latch_);
    LockRequestQueue &request_queue = *it->second;

    // 查找当前事务的锁请求
    auto req_it = std::find_if(request_queue.request_queue_.begin(), request_queue.request_queue_.end(),
                               [txn](const std::shared_ptr<LockRequest> &req) {
                                   return req->txn_id_ == txn->get_transaction_id();
                               });

    if (req_it == request_queue.request_queue_.end()) {
        return false; // 如果找不到当前事务的锁请求，返回 false
    }

    // 移除锁请求
    request_queue.request_queue_.erase(req_it);

    // 更新组锁模式
    // Recalculate the group lock mode
    request_queue.group_lock_mode_ = calculate_group_lock_mode_remove(request_queue);
    if (request_queue.upgrading_ == txn->get_transaction_id()) {
        request_queue.upgrading_ = INVALID_TXN_ID;
    }
    // 如果请求队列为空，移除锁数据标识符
    if (request_queue.request_queue_.empty()) {
        std::unique_lock<std::mutex> lock_(latch_);
        lock_table_.erase(it);
        lock_.unlock();
    } else {
        // 重新授予锁
        grant_lock(request_queue);
    }
    return true;
}

bool LockManager::is_group_lock(LockManager::LockMode lockMode, LockManager::GroupLockMode groupMode) {
    // Compatibility matrix
    static const bool compatibility_matrix[6][5] = {
            //   IS    IX    S     X     SIX
            {false, false, false, false, false},  // NON_LOCK
            {true,  false, false, false, false},  // IS
            {false, true,  false, false, false}, // IX
            {false, false, true,  false, false}, // S
            {false, false, false, true,  false}, // X
            {false, false, false, false, true}  // SIX
    };

    return compatibility_matrix[static_cast<int>(groupMode)][static_cast<int>(lockMode)];
}

LockManager::GroupLockMode LockManager::calculate_group_lock_mode_remove(const LockRequestQueue &request_queue) {
    GroupLockMode strongest_lock_mode = GroupLockMode::NON_LOCK;

    for (const auto &request: request_queue.request_queue_) {
        if(request->granted_){
            switch (request->lock_mode_) {
                case LockMode::EXLUCSIVE:
                    return GroupLockMode::X;
                case LockMode::S_IX:
                    strongest_lock_mode = GroupLockMode::SIX;
                    break;
                case LockMode::INTENTION_EXCLUSIVE:
                    if (strongest_lock_mode != GroupLockMode::SIX) {
                        strongest_lock_mode = GroupLockMode::IX;
                    }
                    break;
                case LockMode::SHARED:
                    if (strongest_lock_mode != GroupLockMode::SIX && strongest_lock_mode != GroupLockMode::IX) {
                        strongest_lock_mode = GroupLockMode::S;
                    }
                    break;
                case LockMode::INTENTION_SHARED:
                    if (strongest_lock_mode == GroupLockMode::NON_LOCK) {
                        strongest_lock_mode = GroupLockMode::IS;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    return strongest_lock_mode;
}

