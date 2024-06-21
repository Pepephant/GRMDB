/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//#undef NDEBUG
//
//#define private public
//
//#include "record/rm.h"
//#include "storage/buffer_pool_manager.h"
//#include "system/sm_manager.h"
//
//#undef private
//
//#include <algorithm>
//#include <cassert>
//#include <cstdio>
//#include <cstdlib>
//#include <cstring>
//#include <ctime>
//#include <iostream>
//#include <memory>
//#include <random>
//#include <set>
//#include <string>
//#include <thread>  // NOLINT
//#include <unordered_map>
//#include <vector>
//
//#include "gtest/gtest.h"
//#include "replacer/lru_replacer.h"
//#include "storage/disk_manager.h"
//
//constexpr int MAX_FILES = 32;
//constexpr int MAX_PAGES = 128;
//constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;
//
//// 创建BufferPoolManager
//auto disk_manager = std::make_unique<DiskManager>();
//auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());
//
//std::unordered_map<int, char *> mock;  // fd -> buffer
//
//
//class IndexTests : public ::testing::Test {
//public:
//    std::unique_ptr<DiskManager> disk_manager_;
//    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
//    std::unique_ptr<RmManager> rm_manager_;
//    std::unique_ptr<IxManager> ix_manager_;
//    std::unique_ptr<SmManager> sm_manager_;
//    std::string TEST_INDEX_NAME = "TEST_INDEX";
//
//    int key_len_;
//    char* key_buf_;
//
//    std::string tab_name_ = "test1";
//    std::vector<std::string> col_names_;
//
//public:
//    // This function is called before every test.
//    void SetUp() override {
//        ::testing::Test::SetUp();
//        // For each test, we create a new DiskManager
//
//        disk_manager_ = std::make_unique<DiskManager>();
//        buffer_pool_manager_ = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager_.get());
//        rm_manager_ = std::make_unique<RmManager>(disk_manager_.get(), buffer_pool_manager_.get());
//        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), buffer_pool_manager_.get());
//        sm_manager_ = std::make_unique<SmManager>(disk_manager_.get(), buffer_pool_manager_.get(), rm_manager_.get(), ix_manager_.get());
//
//        // 如果测试目录不存在，则先创建测试目录
//        if (disk_manager_->is_dir(TEST_INDEX_NAME)) {
//            disk_manager_->destroy_dir(TEST_INDEX_NAME);
//        }
//        assert(!disk_manager_->is_dir(TEST_INDEX_NAME));
//
//        sm_manager_->create_db(TEST_INDEX_NAME);
//        sm_manager_->open_db(TEST_INDEX_NAME);
//    }
//
//    IxIndexHandle* GetIh(int length) {
//        key_len_ = length;
//
//        std::string col_name = "id";
//        std::vector<ColDef> col_defs;
//        ColDef col_def{col_name, TYPE_STRING, length};
//        col_defs.push_back(col_def);
//        col_names_.push_back(col_name);
//
//        sm_manager_->create_table(tab_name_, col_defs, nullptr);
//        sm_manager_->create_index(tab_name_, col_names_, nullptr);
//        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, col_names_);
//        return sm_manager_->ihs_.at(index_name).get();
//    }
//
//    auto SetKey(int key) -> char* {
//        key_buf_ = new char[key_len_];
//        memset(key_buf_, 0, key_len_);
//        std::string key_str = std::to_string(key);
//        memcpy(key_buf_, key_str.c_str(), key_str.size());
//        return key_buf_;
//    }
//
//// This function is called after every test.
//    void TearDown() override {
//        sm_manager_->drop_index(tab_name_, col_names_, nullptr);
//        sm_manager_->drop_table(tab_name_, nullptr);
//        sm_manager_->close_db();
//        sm_manager_->drop_db(TEST_INDEX_NAME);
//    };
//
//    void check_leaf(const IxIndexHandle *ih) {
//        // check leaf list
//        page_id_t leaf_no = ih->file_hdr_->first_leaf_;
//        while (leaf_no != IX_LEAF_HEADER_PAGE) {
//            IxNodeHandle *curr = ih->fetch_node(leaf_no);
//            IxNodeHandle *prev = ih->fetch_node(curr->get_prev_leaf());
//            IxNodeHandle *next = ih->fetch_node(curr->get_next_leaf());
//            // Ensure prev->next == curr && next->prev == curr
//            ASSERT_EQ(prev->get_next_leaf(), leaf_no);
//            ASSERT_EQ(next->get_prev_leaf(), leaf_no);
//            leaf_no = curr->get_next_leaf();
//            buffer_pool_manager_->unpin_page(curr->get_page_id(), false);
//            buffer_pool_manager_->unpin_page(prev->get_page_id(), false);
//            buffer_pool_manager_->unpin_page(next->get_page_id(), false);
//        }
//    }
//
//    void check_tree(const IxIndexHandle *ih, int now_page_no) {
//        IxNodeHandle *node = ih->fetch_node(now_page_no);
//        if (node->is_leaf_page()) {
//            buffer_pool_manager_->unpin_page(node->get_page_id(), false);
//            return;
//        }
//        for (int i = 0; i < node->get_size(); i++) {                 // 遍历node的所有孩子
//            IxNodeHandle *child = ih->fetch_node(node->value_at(i));  // 第i个孩子
//            // check parent
//            assert(child->get_parent_page_no() == now_page_no);
//            // check first key
//            int node_key = node->key_at(i);  // node的第i个key
//            int child_first_key = child->key_at(0);
//            int child_last_key = child->key_at(child->get_size() - 1);
//            if (i != 0) {
//                // 除了第0个key之外，node的第i个key与其第i个孩子的第0个key的值相同
//                ASSERT_EQ(node_key, child_first_key);
//            }
//            if (i + 1 < node->get_size()) {
//                // 满足制约大小关系
//                ASSERT_LT(child_last_key, node->key_at(i + 1));  // child_last_key < node->KeyAt(i + 1)
//            }
//
//            buffer_pool_manager_->unpin_page(child->get_page_id(), false);
//
//            check_tree(ih, node->value_at(i));  // 递归子树
//        }
//        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
//    }
//
//
//};
//
//TEST_F(IndexTests, NodeTest1) {
//    auto ih = IndexTests::GetIh(5);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//    std::vector<int> delete_keys;
//    std::vector<int> remain_keys;
//
//    for (int j = 1; j <= 311; j++) {
//        keys.push_back(j);
//        if ( j % 2 == 0 ) {
//            delete_keys.push_back(j);
//        } else {
//            remain_keys.push_back(j);
//        }
//    }
//
//    std::random_shuffle(keys.begin(), keys.end());
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->insert_entry(key_buf, {0, key}, nullptr);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 1);
//        EXPECT_EQ(res.back(), rid);
//    }
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 1);
//        EXPECT_EQ(res.back(), rid);
//    }
//
//    for (auto& key: delete_keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->delete_entry(key_buf, nullptr);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 0);
//    }
//}
//
//TEST_F(IndexTests, InsertTest1) {
//    auto ih = IndexTests::GetIh(300);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//    for (int j = 1; j <= 18900; j++) {
//        keys.push_back(j);
//    }
//
//    std::random_shuffle(keys.begin(), keys.end());
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->insert_entry(key_buf, {0, key}, nullptr);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 1);
//        EXPECT_EQ(res.back(), rid);
//    }
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 1);
//        EXPECT_EQ(res.back(), rid);
//    }
//
//    std::sort(keys.begin(), keys.end(), [](int x,int y){return std::to_string(x) < std::to_string(y);});
//    auto lower = ih->leaf_begin();
//    auto upper = ih->leaf_end();
//    auto bpm = buffer_pool_manager_.get();
//    int num_records = 0;
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, keys.size());
//}
//
//TEST_F(IndexTests, DeleteTest1) {
//    auto ih = IndexTests::GetIh(300);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//    std::vector<int> delete_keys;
//    std::vector<int> remain_keys;
//
//    for (int j = 1; j <= 10000; j++) {
//        keys.push_back(j);
//        if (j % 1320 != 0) {
//            delete_keys.push_back(j);
//        } else {
//            remain_keys.push_back(j);
//        }
//    }
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->insert_entry(key_buf, {0, key}, nullptr);
//    }
//
//    for (auto& key: delete_keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->delete_entry(key_buf, nullptr);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 0);
//    }
//
//    for (auto& key: remain_keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_NE(res.size(), 0);
//        EXPECT_EQ(res.front(), rid);
//    }
//
//    ih->show_tree();
//    ih->check_scan();
//
//    std::sort(remain_keys.begin(), remain_keys.end(), [](int x,int y){return std::to_string(x) < std::to_string(y);});
//    auto lower = ih->leaf_begin();
//    auto upper = ih->leaf_end();
//    auto bpm = buffer_pool_manager_.get();
//    int num_records = 0;
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, remain_keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, remain_keys.size());
//}
//
//TEST_F(IndexTests, DeleteTest2) {
//    auto ih = IndexTests::GetIh(300);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//    std::vector<int> delete_keys;
//    std::vector<int> remain_keys;
//
//    for (int j = 1; j <= 10000; j++) {
//        keys.push_back(j);
//        if (j % 1320 != 1) {
//            delete_keys.push_back(j);
//        } else {
//            remain_keys.push_back(j);
//        }
//    }
//
//    std::random_shuffle(keys.begin(), keys.end());
//    std::random_shuffle(delete_keys.begin(), delete_keys.end());
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->insert_entry(key_buf, {0, key}, nullptr);
//    }
//
//    for (auto& key: delete_keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->delete_entry(key_buf, nullptr);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 0);
//    }
//
//    for (auto& key: remain_keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_NE(res.size(), 0);
//        EXPECT_EQ(res.front(), rid);
//    }
//
//    std::sort(remain_keys.begin(), remain_keys.end(), [](int x,int y){return std::to_string(x) < std::to_string(y);});
//    auto lower = ih->leaf_begin();
//    auto upper = ih->leaf_end();
//    auto bpm = buffer_pool_manager_.get();
//    int num_records = 0;
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, remain_keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, remain_keys.size());
//
//    for (auto& key: remain_keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->delete_entry(key_buf, nullptr);
//        EXPECT_EQ(res.size(), 0);
//    }
//
//    EXPECT_EQ(ih->getFileHdr()->num_pages_, 2);
//    EXPECT_EQ(ih->getFileHdr()->root_page_, IX_NO_PAGE);
//}
//
//TEST_F(IndexTests, MixTest1) {
//    auto ih = IndexTests::GetIh(300);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//    std::vector<int> delete_keys;
//    std::vector<int> remain_keys;
//
//    for (int j = 1; j <= 10000; j++) {
//        keys.push_back(j);
//    }
//
//    std::random_shuffle(keys.begin(), keys.end());
//
//    for (int i = 0; i < keys.size(); i++) {
//        std::vector<Rid> res;
//        Rid rid{0, keys[i]};
//        char* key_buf = IndexTests::SetKey(keys[i]);
//
//        remain_keys.push_back(keys[i]);
//        ih->insert_entry(key_buf, rid, nullptr);
//
//        if (i % 126 != 0) {
//            int del = remain_keys[rand() % remain_keys.size()];
//            auto it = std::find(remain_keys.begin(), remain_keys.end(), del);
//            remain_keys.erase(it);
//
//            key_buf = IndexTests::SetKey(del);
//            ih->delete_entry(key_buf, nullptr);
//            delete_keys.push_back(del);
//        }
//    }
//
//    for (auto& key: delete_keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 0);
//    }
//
//    for (auto& key: remain_keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_NE(res.size(), 0);
//        EXPECT_EQ(res.front(), rid);
//    }
//
//    std::sort(remain_keys.begin(), remain_keys.end(), [](int x,int y){return std::to_string(x) < std::to_string(y);});
//    auto lower = ih->leaf_begin();
//    auto upper = ih->leaf_end();
//    auto bpm = buffer_pool_manager_.get();
//    int num_records = 0;
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, remain_keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, remain_keys.size());
//
//    for (auto& key: remain_keys) {
//        char* key_buf = IndexTests::SetKey(key);
//        ih->delete_entry(key_buf, nullptr);
//    }
//
//    EXPECT_EQ(ih->getFileHdr()->num_pages_, 2);
//    EXPECT_EQ(ih->getFileHdr()->root_page_, IX_NO_PAGE);
//
//    lower = ih->leaf_begin();
//    upper = ih->leaf_end();
//    num_records = 0;
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        num_records++;
//    }
//    EXPECT_EQ(num_records, 0);
//}
//
//TEST_F(IndexTests, IxScanTest) {
//    auto ih = IndexTests::GetIh(300);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//    std::vector<int> delete_keys;
//    std::vector<int> remain_keys;
//
//    for (int j = 1; j <= 10000; j++) {
//        keys.push_back(j);
//    }
//
//    std::random_shuffle(keys.begin(), keys.end());
//
//    for (int i = 0; i < keys.size(); i++) {
//        std::vector<Rid> res;
//        Rid rid{0, keys[i]};
//        char* key_buf = IndexTests::SetKey(keys[i]);
//
//        remain_keys.push_back(keys[i]);
//        ih->insert_entry(key_buf, rid, nullptr);
//
//        if (i % 12 == 0) {
//            int del = remain_keys[rand() % remain_keys.size()];
//            auto it = std::find(remain_keys.begin(), remain_keys.end(), del);
//            remain_keys.erase(it);
//
//            key_buf = IndexTests::SetKey(del);
//            ih->delete_entry(key_buf, nullptr);
//            delete_keys.push_back(del);
//        }
//    }
//
//    for (auto& key: delete_keys) {
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_EQ(res.size(), 0);
//    }
//
//    for (auto& key: remain_keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_NE(res.size(), 0);
//        EXPECT_EQ(res.front(), rid);
//    }
//
//    std::sort(remain_keys.begin(), remain_keys.end(), [](int x,int y){return std::to_string(x) < std::to_string(y);});
//    auto lower = ih->lower_bound(SetKey(remain_keys.front()));
//    auto upper = ih->upper_bound(SetKey(remain_keys.back()));
//    auto bpm = buffer_pool_manager_.get();
//    int num_records = 0;
//
//    std::cout << "lower: " << lower.page_no << ", " <<  lower.slot_no << "\n";
//    std::cout << "upper: " << upper.page_no << ", " <<  upper.slot_no << "\n";
//
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, remain_keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, remain_keys.size());
//
//    for (int i = 0; i < remain_keys.size() / 2; i++) {
//        remain_keys.pop_back();
//    }
//
//    lower = ih->lower_bound(SetKey(remain_keys.front()));
//    upper = ih->upper_bound(SetKey(remain_keys.back()));
//    num_records = 0;
//
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, remain_keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, remain_keys.size());
//
//    std::reverse(remain_keys.begin(), remain_keys.end());
//    for (int i = 0; i < remain_keys.size() / 2; i++) {
//        remain_keys.pop_back();
//    }
//    std::reverse(remain_keys.begin(), remain_keys.end());
//
//    lower = ih->lower_bound(SetKey(remain_keys.front()));
//    upper = ih->upper_bound(SetKey(remain_keys.back()));
//    num_records = 0;
//
//    for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//        Rid rid{0, remain_keys[num_records]};
//        num_records++;
//        EXPECT_EQ(scan.rid(), rid);
//    }
//    EXPECT_EQ(num_records, remain_keys.size());
//}
//
//TEST_F(IndexTests, IxScanTest1) {
//    auto ih = IndexTests::GetIh(10);
//
//    IxFileHdr* ix_hdr = ih->getFileHdr();
//    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";
//
//    std::vector<int> keys;
//
//    for (int j = 1; j <= 3000; j++) {
//        keys.push_back(j);
//    }
//
//    for (auto& key: keys) {
//        std::vector<Rid> res;
//        Rid rid{0, key};
//        char* key_buf = IndexTests::SetKey(key);
//        ih->insert_entry(key_buf, rid, nullptr);
//    }
//
//    for (auto& key: keys) {
//        Rid rid = {.page_no = 0, .slot_no = key};
//        std::vector<Rid> res;
//        char* key_buf = IndexTests::SetKey(key);
//        ih->get_value(key_buf, &res, nullptr);
//        EXPECT_NE(res.size(), 0);
//        EXPECT_EQ(res.front(), rid);
//    }
//
//    // ih->show_tree();
//    // ih->check_scan();
//
//    auto bpm = buffer_pool_manager_.get();
//    for (auto& key: keys) {
//        auto lower = ih->lower_bound(SetKey(key));
//        auto upper = ih->upper_bound(SetKey(key));
//        int num_records = 0;
//        if (key == 20) {
//            std::cout << "Here";
//        }
//        for (IxScan scan(ih, lower, upper, bpm); !scan.is_end(); scan.next()) {
//            Rid rid{0, key};
//            num_records++;
//            EXPECT_EQ(scan.rid(), rid);
//        }
//        EXPECT_EQ(num_records, 1);
//    }
//}

#include <netdb.h>
#include <netinet/in.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <fstream>
#include <mutex>

#define MAX_MEM_BUFFER_SIZE 8192
#define PORT_DEFAULT 8765

bool is_exit_command(std::string &cmd) { return cmd == "exit" || cmd == "exit;" || cmd == "bye" || cmd == "bye;"; }

int init_unix_sock(const char *unix_sock_path) {
    int sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "failed to create unix socket. %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.sun_family = PF_UNIX;
    snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", unix_sock_path);

    if (connect(sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
        fprintf(stderr, "failed to connect to server. unix socket path '%s'. error %s", sockaddr.sun_path,
                strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int init_tcp_sock(const char *server_host, int server_port) {
    struct hostent *host;
    struct sockaddr_in serv_addr;

    if ((host = gethostbyname(server_host)) == NULL) {
        fprintf(stderr, "gethostbyname failed. errmsg=%d:%s\n", errno, strerror(errno));
        return -1;
    }

    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "create socket error. errmsg=%d:%s\n", errno, strerror(errno));
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    serv_addr.sin_addr = *((struct in_addr *)host->h_addr);
    bzero(&(serv_addr.sin_zero), 8);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr)) == -1) {
        fprintf(stderr, "Failed to connect. errmsg=%d:%s\n", errno, strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

void send_recv_sql(int sockfd, std::string sql) {
    int send_bytes;
    char recv_buf[MAX_MEM_BUFFER_SIZE];

    if((send_bytes = write(sockfd, sql.c_str(), sql.length() + 1)) == -1) {
        std::cerr << "send error: " << errno << ":" << strerror(errno) << " \n" << std::endl;
        exit(1);
    }

    int len = recv(sockfd, recv_buf, MAX_MEM_BUFFER_SIZE, 0);
    if (len < 0) {
        fprintf(stderr, "Connection was broken: %s\n", strerror(errno));
        return;
    } else if (len == 0) {
        printf("Connection has been closed\n");
        return;
    }

    // printf("%s\n", recv_buf);
}

int main(int argc, char *argv[]) {
    int ret = 0;

    const char *unix_socket_path = nullptr;
    const char *server_host = "127.0.0.1";  // 127.0.0.1 192.168.31.25
    int server_port = PORT_DEFAULT;
    int opt;
    std::string test_name = "./test_script/chunk.txt";

    // const char *prompt_str = "RucBase > ";

    int sockfd;
    // char send[MAXLINE];

    sockfd = init_tcp_sock(server_host, server_port);

    if (sockfd < 0) {
        return 1;
    }

    std::ifstream test;
    std::string sql;

    // 测试点1
    test.open(test_name);
    while(std::getline(test, sql)) {
        send_recv_sql(sockfd, sql);
    }

    close(sockfd);

    return 0;
}