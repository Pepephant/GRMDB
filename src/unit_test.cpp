/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#undef NDEBUG

#define private public

#include "record/rm.h"
#include "storage/buffer_pool_manager.h"

#undef private

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "replacer/lru_replacer.h"
#include "storage/disk_manager.h"

const std::string TEST_DB_NAME = "BufferPoolManagerTest_db";  // 以数据库名作为根目录
const std::string TEST_FILE_NAME = "basic";                   // 测试文件的名字
const std::string TEST_FILE_NAME_CCUR = "concurrency";        // 测试文件的名字
const std::string TEST_FILE_NAME_BIG = "bigdata";             // 测试文件的名字
constexpr int MAX_FILES = 32;
constexpr int MAX_PAGES = 128;
constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;

// 创建BufferPoolManager
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());

std::unordered_map<int, char *> mock;  // fd -> buffer

char *mock_get_page(int fd, int page_no) { return &mock[fd][page_no * PAGE_SIZE]; }

void check_disk(int fd, int page_no) {
    char buf[PAGE_SIZE];
    disk_manager->read_page(fd, page_no, buf, PAGE_SIZE);
    char *mock_buf = mock_get_page(fd, page_no);
    assert(memcmp(buf, mock_buf, PAGE_SIZE) == 0);
}

void check_disk_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
}

void check_cache(int fd, int page_no) {
    Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
    char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE];
    assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);
    buffer_pool_manager->unpin_page(PageId{fd, page_no}, false);
}

void check_cache_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_cache(fd, page_no);
        }
    }
}

void rand_buf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        int rand_ch = rand() & 0xff;
        buf[i] = rand_ch;
    }
}

int rand_fd() {
    assert(mock.size() == MAX_FILES);
    int fd_idx = rand() % MAX_FILES;
    auto it = mock.begin();
    for (int i = 0; i < fd_idx; i++) {
        it++;
    }
    return it->first;
}

struct rid_hash_t {
    size_t operator()(const Rid &rid) const { return (rid.page_no << 16) | rid.slot_no; }
};

struct rid_equal_t {
    bool operator()(const Rid &x, const Rid &y) const { return x.page_no == y.page_no && x.slot_no == y.slot_no; }
};

void check_equal(const RmFileHandle *file_handle,
                 const std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> &mock) {
    // Test all records
    for (auto &entry : mock) {
        Rid rid = entry.first;
        auto mock_buf = (char *)entry.second.c_str();
        auto rec = file_handle->get_record(rid, nullptr);
        assert(memcmp(mock_buf, rec->data, file_handle->file_hdr_.record_size) == 0);
    }
    // Randomly get record
    for (int i = 0; i < 10; i++) {
        Rid rid = {.page_no = 1 + rand() % (file_handle->file_hdr_.num_pages - 1),
                   .slot_no = rand() % file_handle->file_hdr_.num_records_per_page};
        bool mock_exist = mock.count(rid) > 0;
        bool rm_exist = file_handle->is_record(rid);
        assert(rm_exist == mock_exist);
    }
    // Test RM scan
    size_t num_records = 0;
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        assert(mock.count(scan.rid()) > 0);
        auto rec = file_handle->get_record(scan.rid(), nullptr);
        assert(memcmp(rec->data, mock.at(scan.rid()).c_str(), file_handle->file_hdr_.record_size) == 0);
        num_records++;
    }
    assert(num_records == mock.size());
}

// std::cout can call this, for example: std::cout << rid
std::ostream &operator<<(std::ostream &os, const Rid &rid) {
    return os << '(' << rid.page_no << ", " << rid.slot_no << ')';
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_BIG，记录其文件描述符fd */

class BigStorageTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_BIG)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_BIG);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_BIG);
        assert(disk_manager_->is_file(TEST_FILE_NAME_BIG));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_BIG);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_BIG);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST(LRUReplacerTest, SampleTest) {
    LRUReplacer lru_replacer(7);

    // Scenario: unpin six elements, i.e. add them to the replacer.
    lru_replacer.unpin(1);
    lru_replacer.unpin(2);
    lru_replacer.unpin(3);
    lru_replacer.unpin(4);
    lru_replacer.unpin(5);
    lru_replacer.unpin(6);
    lru_replacer.unpin(1);
    EXPECT_EQ(6, lru_replacer.Size());

    // Scenario: get three victims from the lru.
    int value;
    lru_replacer.victim(&value);
    EXPECT_EQ(1, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(2, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(3, value);

    // Scenario: pin elements in the replacer.
    // Note that 3 has already been victimized, so pinning 3 should have no effect.
    lru_replacer.pin(3);
    lru_replacer.pin(4);
    EXPECT_EQ(2, lru_replacer.Size());

    // Scenario: unpin 4. We expect that the reference bit of 4 will be set to 1.
    lru_replacer.unpin(4);

    // Scenario: continue looking for victims. We expect these victims.
    lru_replacer.victim(&value);
    EXPECT_EQ(5, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(6, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(4, value);
}


TEST(LRUReplacerTest, MixTest) {
    int result;
    int value_size = 10000;
    auto lru_replacer = new LRUReplacer(value_size);
    std::vector<int> value(value_size);
    for (int i = 0; i < value_size; i++) {
        value[i] = i;
    }
    auto rng = std::default_random_engine{};
    std::shuffle(value.begin(), value.end(), rng);

    for (int i = 0; i < value_size; i++) {
        lru_replacer->unpin(value[i]);
    }
    EXPECT_EQ(value_size, lru_replacer->Size());

    // pin and unpin 777
    lru_replacer->pin(777);
    lru_replacer->unpin(777);
    // pin and unpin 0
    EXPECT_EQ(1, lru_replacer->victim(&result));
    EXPECT_EQ(value[0], result);
    lru_replacer->unpin(value[0]);

    for (int i = 0; i < value_size / 2; i++) {
        if (value[i] != value[0] && value[i] != 777) {
            lru_replacer->pin(value[i]);
            lru_replacer->unpin(value[i]);
        }
    }

    std::vector<int> lru_array;
    for (int i = value_size / 2; i < value_size; ++i) {
        if (value[i] != value[0] && value[i] != 777) {
            lru_array.push_back(value[i]);
        }
    }
    lru_array.push_back(777);
    lru_array.push_back(value[0]);
    for (int i = 0; i < value_size / 2; ++i) {
        if (value[i] != value[0] && value[i] != 777) {
            lru_array.push_back(value[i]);
        }
    }
    EXPECT_EQ(value_size, lru_replacer->Size());

    for (int e : lru_array) {
        EXPECT_EQ(true, lru_replacer->victim(&result));
        EXPECT_EQ(e, result);
    }
    EXPECT_EQ(value_size - lru_array.size(), lru_replacer->Size());

    delete lru_replacer;
}

/**
 * @brief 并发测试LRUReplacer
 * @note lab1 计分：10 points
 */
TEST(LRUReplacerTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;
    for (int run = 0; run < num_runs; run++) {
        int value_size = 1000;
        std::shared_ptr<LRUReplacer> lru_replacer{new LRUReplacer(value_size)};
        std::vector<std::thread> threads;
        int result;
        std::vector<int> value(value_size);
        for (int i = 0; i < value_size; i++) {
            value[i] = i;
        }
        auto rng = std::default_random_engine{};
        std::shuffle(value.begin(), value.end(), rng);

        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([tid, &lru_replacer, &value]() {
                int share = 1000 / 5;
                for (int i = 0; i < share; i++) {
                    lru_replacer->unpin(value[tid * share + i]);
                }
            }));
        }

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
        std::vector<int> out_values;
        for (int i = 0; i < value_size; i++) {
            EXPECT_EQ(1, lru_replacer->victim(&result));
            out_values.push_back(result);
        }
        std::sort(value.begin(), value.end());
        std::sort(out_values.begin(), out_values.end());
        EXPECT_EQ(value, out_values);
        EXPECT_EQ(0, lru_replacer->victim(&result));
    }
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME，记录其文件描述符fd */
class BufferPoolManagerTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_FILE_NAME);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME);
        assert(disk_manager_->is_file(TEST_FILE_NAME));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };

    /**
     * @brief 将buf填充size个字节的随机数据
     */
    void rand_buf(char *buf, int size) {
        srand((unsigned)time(nullptr));
        for (int i = 0; i < size; i++) {
            int rand_ch = rand() & 0xff;
            buf[i] = rand_ch;
        }
    }

    /**
     * @brief 随机获取mock中的键
     */
    int rand_fd(std::unordered_map<int, char *> mock) {
        assert(mock.size() == MAX_FILES);
        int fd_idx = rand() % MAX_FILES;
        auto it = mock.begin();
        for (int i = 0; i < fd_idx; i++) {
            it++;
        }
        return it->first;
    }
};

// NOLINTNEXTLINE
TEST_F(BufferPoolManagerTest, SampleTest) {
    // create BufferPoolManager
    const size_t buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager);
    // create tmp PageId
    int fd = BufferPoolManagerTest::fd_;
    PageId page_id_temp = {.fd = fd, .page_no = INVALID_PAGE_ID};
    auto *page0 = bpm->new_page(&page_id_temp);

    // Scenario: The buffer pool is empty. We should be able to create a new page.
    ASSERT_NE(nullptr, page0);
    EXPECT_EQ(0, page_id_temp.page_no);

    // Scenario: Once we have a page, we should be able to read and write content.
    snprintf(page0->get_data(), sizeof(page0->get_data()), "Hello");
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    for (size_t i = 1; i < buffer_pool_size; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
    for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
        EXPECT_EQ(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
    // there would still be one cache frame left for reading page 0.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(true, bpm->unpin_page(PageId{fd, i}, true));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: We should be able to fetch the data we wrote a while ago.
    page0 = bpm->fetch_page(PageId{fd, 0});
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));
    EXPECT_EQ(true, bpm->unpin_page(PageId{fd, 0}, true));
    // new_page again, and now all buffers are pinned. Page 0 would be failed to fetch.
    EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    EXPECT_EQ(nullptr, bpm->fetch_page(PageId{fd, 0}));

    bpm->flush_all_pages(fd);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_CCUR，记录其文件描述符fd */

// Add by jiawen
class BufferPoolManagerConcurrencyTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_CCUR)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_CCUR);
        assert(disk_manager_->is_file(TEST_FILE_NAME_CCUR));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_CCUR);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;

    // get fd
    int fd = BufferPoolManagerConcurrencyTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
        std::shared_ptr<BufferPoolManager> bpm{new BufferPoolManager(50, disk_manager)};

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, fd]() {  // NOLINT
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                std::vector<PageId> page_ids;
                for (int i = 0; i < 10; i++) {
                    auto new_page = bpm->new_page(&temp_page_id);
                    EXPECT_NE(nullptr, new_page);
                    ASSERT_NE(nullptr, new_page);
                    strcpy(new_page->get_data(), std::to_string(temp_page_id.page_no).c_str());  // NOLINT
                    page_ids.push_back(temp_page_id);
                }
                for (int i = 0; i < 10; i++) {
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[i], true));
                }
                for (int j = 0; j < 10; j++) {
                    auto page = bpm->fetch_page(page_ids[j]);
                    EXPECT_NE(nullptr, page);
                    ASSERT_NE(nullptr, page);
                    EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[j], true));
                }
                for (int j = 0; j < 10; j++) {
                    EXPECT_EQ(1, bpm->delete_page(page_ids[j]));
                }
                bpm->flush_all_pages(fd);  // add this test by jiawen
            }));
        }  // end loop tid=[0,num_threads)

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
    }  // end loop run=[0,num_runs)
}

// TODO: fix detected memory leaks found by Google Test
TEST(StorageTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    /** Test disk_manager */
    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = std::to_string(i) + ".txt";
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // open without create
        try {
            disk_manager->open_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }

        disk_manager->create_file(filename);
        assert(disk_manager->is_file(filename));
        try {
            disk_manager->create_file(filename);
            assert(false);
        } catch (const FileExistsError &e) {
        }

        // open file
        int fd = disk_manager->open_file(filename);
        char *tmp = new char[PAGE_SIZE * MAX_PAGES];  // TODO: fix error in detected memory leaks

        mock[fd] = tmp;
        fd2name[fd] = filename;

        disk_manager->set_fd2pageno(fd, 0);  // diskmanager在fd对应的文件中从0开始分配page_no
    }

    /** Test buffer_pool_manager*/
    int num_pages = 0;
    char init_buf[PAGE_SIZE];
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(PAGE_SIZE, init_buf);  // 将init_buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);
            int page_no = tmp_page_id.page_no;
            assert(page_no != INVALID_PAGE_ID);
            assert(page_no == i);

            memcpy(page->get_data(), init_buf, PAGE_SIZE);
            buffer_pool_manager->unpin_page(PageId{fd, page_no}, true);

            char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE]
            memcpy(mock_buf, init_buf, PAGE_SIZE);

            num_pages++;

            check_cache(fd, page_no);  // 调用了fetch_page, unpin_page
        }
    }
    check_cache_all();

    assert(num_pages == TEST_BUFFER_POOL_SIZE);

    /** Test flush_all_pages() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd();
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = mock_get_page(fd, page_no);
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(PAGE_SIZE, init_buf);
        memcpy(page->get_data(), init_buf, PAGE_SIZE);
        memcpy(mock_buf, init_buf, PAGE_SIZE);

        buffer_pool_manager->unpin_page(page->get_page_id(), true);
        // BufferPool::mark_dirty(page);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            check_disk(fd, page_no);
        }
        // flush entire file
        if (rand() % 100 == 0) {
            buffer_pool_manager->flush_all_pages(fd);
        }
        // re-open file
        if (rand() % 100 == 0) {
            disk_manager->close_file(fd);
            auto filename = fd2name[fd];
            char *buf = mock[fd];
            fd2name.erase(fd);
            mock.erase(fd);
            int new_fd = disk_manager->open_file(filename);
            mock[new_fd] = buf;
            fd2name[new_fd] = filename;
        }
        // assert equal in cache
        check_cache(fd, page_no);
    }
    check_cache_all();

    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    // close and destroy files
    for (auto &entry : fd2name) {
        int fd = entry.first;
        auto &filename = entry.second;
        disk_manager->close_file(fd);
        disk_manager->destroy_file(filename);
        try {
            disk_manager->destroy_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }
    }
}

TEST(RecordManagerTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> mock;

    std::string filename = "abc.txt";

    int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE
    // test files
    {
        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
        rm_manager->destroy_file(filename);
    }
    // test pages
    rm_manager->create_file(filename, record_size);
    auto file_handle = rm_manager->open_file(filename);

    char write_buf[PAGE_SIZE];
    size_t add_cnt = 0;
    size_t upd_cnt = 0;
    size_t del_cnt = 0;
    for (int round = 0; round < 1000; round++) {
        double insert_prob = 1. - mock.size() / 250.;
        double dice = rand() * 1. / RAND_MAX;
        if (mock.empty() || dice < insert_prob) {
            rand_buf(file_handle->file_hdr_.record_size, write_buf);
            Rid rid = file_handle->insert_record(write_buf, nullptr);
            mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
            add_cnt++;
            //            std::cout << "insert " << rid << '\n'; // operator<<(cout,rid)
        } else {
            // update or erase random rid
            int rid_idx = rand() % mock.size();
            auto it = mock.begin();
            for (int i = 0; i < rid_idx; i++) {
                it++;
            }
            auto rid = it->first;
            if (rand() % 2 == 0) {
                // update
                rand_buf(file_handle->file_hdr_.record_size, write_buf);
                file_handle->update_record(rid, write_buf, nullptr);
                mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
                upd_cnt++;
                //                std::cout << "update " << rid << '\n';
            } else {
                // erase
                file_handle->delete_record(rid, nullptr);
                mock.erase(rid);
                del_cnt++;
                //                std::cout << "delete " << rid << '\n';
            }
        }
        // Randomly re-open file
        if (round % 50 == 0) {
            rm_manager->close_file(file_handle.get());
            file_handle = rm_manager->open_file(filename);
        }
        check_equal(file_handle.get(), mock);
    }
    assert(mock.size() == add_cnt - del_cnt);
    std::cout << "insert " << add_cnt << '\n' << "delete " << del_cnt << '\n' << "update " << upd_cnt << '\n';
    // clean up
    rm_manager->close_file(file_handle.get());
    rm_manager->destroy_file(filename);
}

TEST(RecordManagerTest, MultipleFilesTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::vector<std::string> filenames;
    constexpr int MAX_FILES = 32;

    for (int i = 0; i < MAX_FILES; i++) {
        std::string filename = std::to_string(i) + ".txt";
        filenames.push_back(filename);
    }

    for (int i = 0; i < MAX_FILES; i++) {
        std::string filename = filenames[i];

        int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE

        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }

        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        // printf("file_handle->file_hdr_.num_pages=%d\n", file_handle->file_hdr_.num_pages);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
    }

    for (int i = 0; i < MAX_FILES; i++) {
        std::string filename = filenames[i];
        rm_manager->destroy_file(filename);
    }
}


TEST_F(BufferPoolManagerTest, LargeScaleTest) {
    const int scale = 10000;

    // create BufferPoolManager
    const int buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(static_cast<size_t>(buffer_pool_size), disk_manager);

    // create and open file
    int fd = BufferPoolManagerTest::fd_;
    // create tmp PageId
    PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};

    std::vector<PageId> page_ids;
    for (int i = 0; i < scale / buffer_pool_size; i++) {
        for (int j = 0; j < buffer_pool_size; j++) {
            auto new_page = bpm->new_page(&tmp_page_id);
            EXPECT_NE(nullptr, new_page);
            strcpy(new_page->get_data(), std::to_string(tmp_page_id.page_no).c_str());
            page_ids.push_back(tmp_page_id);
        }
        for (unsigned int j = page_ids.size() - buffer_pool_size; j < page_ids.size(); j++) {
            EXPECT_EQ(true, bpm->unpin_page(page_ids[j], true));
        }
    }

    for (int i = 0; i < scale; i++) {
        auto page = bpm->fetch_page(page_ids[i]);
        EXPECT_NE(nullptr, page);
        EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[i].page_no).c_str(), page->get_data()));
        EXPECT_EQ(true, bpm->unpin_page(page_ids[i], true));
        page_ids.push_back(tmp_page_id);
    }

    for (int i = 0; i < scale; i++) {
        EXPECT_EQ(true, bpm->delete_page(page_ids[i]));
    }
}

/**
 * @brief 多文件测试
 * @note 生成若干测试文件multiple_files_test_*
 * @note lab1 计分：10 points
 */
TEST_F(BufferPoolManagerTest, MultipleFilesTest) {
    const size_t buffer_size = MAX_FILES * MAX_PAGES / 2;
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(buffer_size, disk_manager_.get());

    // mock记录生成文件的(文件fd, page在内存中的首地址)
    // page在内存中的首地址是page在内存中的备份
    std::unordered_map<int, char *> mock;  // fd -> page address

    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = "multiple_files_test_" + std::to_string(i);
        if (disk_manager_->is_file(filename)) {
            disk_manager_->destroy_file(filename);
        }
        // create and open file
        disk_manager_->create_file(filename);
        int fd = disk_manager_->open_file(filename);

        mock[fd] = new char[PAGE_SIZE * MAX_PAGES];  // 申请PAGE_SIZE * MAX_PAGES个字节的内存空间，mock[fd]记录其首地址
        fd2name[fd] = filename;

        disk_manager_->set_fd2pageno(fd, 0);  // 设置diskmanager在fd对应的文件中从0开始分配page_no
    }

    char buf[PAGE_SIZE] = {0};

    /** Test new_page(), unpin_page() */
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(buf, PAGE_SIZE);  // 生成buf，将buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);  // pin the page
            int page_no = tmp_page_id.page_no;
            EXPECT_EQ(page_no, i);

            memcpy(page->get_data(), buf, PAGE_SIZE);          // buf -> page
            char *mock_buf = &mock[fd][page_no * PAGE_SIZE];  // get mock address in (fd,page_no)
            memcpy(mock_buf, buf, PAGE_SIZE);                 // buf -> mock

            // check cache: page data == mock data
            EXPECT_EQ(memcmp(page->get_data(), mock_buf, PAGE_SIZE), 0);

            bool unpin_flag = buffer_pool_manager->unpin_page(page->get_page_id(), true);  // unpin the page
            EXPECT_EQ(unpin_flag, true);
        }
    }

    /** Test flush_all_pages(), fetch_page(), unpin_page() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);  // wirte all pages in fd file into disk
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            // check disk: disk data == mock data
            disk_manager_->read_page(fd, page_no, buf, PAGE_SIZE);  // read page from disk (disk -> buf)
            char *mock_buf = &mock[fd][page_no * PAGE_SIZE];        // get mock address in (fd,page_no)
            EXPECT_EQ(memcmp(buf, mock_buf, PAGE_SIZE), 0);
            // check disk: disk data == page data
            Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
            EXPECT_EQ(memcmp(buf, page->get_data(), PAGE_SIZE), 0);
            bool unpin_flag = buffer_pool_manager->unpin_page(page->get_page_id(), false);
            EXPECT_EQ(unpin_flag, true);
        }
    }

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd(mock);
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = &mock[fd][page_no * PAGE_SIZE];
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(buf, PAGE_SIZE);
        memcpy(page->get_data(), buf, PAGE_SIZE);
        memcpy(mock_buf, buf, PAGE_SIZE);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            // check disk: disk data == mock data
            disk_manager_->read_page(fd, page_no, buf, PAGE_SIZE);  // read page from disk (disk -> buf)
            char *mock_buf = &mock[fd][page_no * PAGE_SIZE];        // get mock address in (fd,page_no)
            EXPECT_EQ(memcmp(buf, mock_buf, PAGE_SIZE), 0);
        }
        // check cache: page data == mock data
        EXPECT_EQ(memcmp(page->get_data(), mock_buf, PAGE_SIZE), 0);

        bool unpin_flag = buffer_pool_manager->unpin_page(page->get_page_id(), true);  // unpin the page
        EXPECT_EQ(unpin_flag, true);
    }

    // close and destroy files
    for (auto &entry : fd2name) {
        int fd = entry.first;
        disk_manager_->close_file(fd);
        // auto &filename = entry.second;
        // disk_manager_->destroy_file(filename);
    }
}

/**
 * @brief 缓冲池并发测试（单文件）
 * @note 生成测试文件concurrency_test
 * @note lab1 计分：15 points
 */
TEST_F(BufferPoolManagerTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;
    const int buffer_pool_size = 50;

    // get disk manager
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    // create and open file
    int fd = BufferPoolManagerTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        std::shared_ptr<BufferPoolManager> bpm{
                new BufferPoolManager(static_cast<size_t>(buffer_pool_size), disk_manager)};

        PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
        std::vector<PageId> page_ids;
        for (int i = 0; i < buffer_pool_size; i++) {
            auto *new_page = bpm->new_page(&tmp_page_id);
            EXPECT_NE(nullptr, new_page);
            strcpy(new_page->get_data(), std::to_string(tmp_page_id.page_no).c_str());
            page_ids.push_back(tmp_page_id);
        }

        for (int i = 0; i < buffer_pool_size; i++) {
            if (i % 2 == 0) {
                EXPECT_EQ(true, bpm->unpin_page(page_ids[i], true));
            } else {
                EXPECT_EQ(true, bpm->unpin_page(page_ids[i], false));
            }
        }

        for (int i = 0; i < buffer_pool_size; i++) {
            auto *new_page = bpm->new_page(&tmp_page_id);
            EXPECT_NE(nullptr, new_page);
            EXPECT_EQ(true, bpm->unpin_page(tmp_page_id, true));
        }

        for (int j = 0; j < buffer_pool_size; j++) {
            auto *page = bpm->fetch_page(page_ids[j]);
            EXPECT_NE(nullptr, page);
            strcpy(page->get_data(), (std::string("Hard") + std::to_string(page_ids[j].page_no)).c_str());
        }

        for (int i = 0; i < buffer_pool_size; i++) {
            if (i % 2 == 0) {
                EXPECT_EQ(true, bpm->unpin_page(page_ids[i], false));
            } else {
                EXPECT_EQ(true, bpm->unpin_page(page_ids[i], true));
            }
        }

        for (int i = 0; i < buffer_pool_size; i++) {
            auto *new_page = bpm->new_page(&tmp_page_id);
            EXPECT_NE(nullptr, new_page);
            EXPECT_EQ(true, bpm->unpin_page(tmp_page_id, true));
        }

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, tid, page_ids, fd]() {
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                int j = (tid * 10);
                while (j < buffer_pool_size) {
                    if (j != tid * 10) {
                        auto *page_local = bpm->fetch_page(temp_page_id);
                        while (page_local == nullptr) {
                            page_local = bpm->fetch_page(temp_page_id);
                        }
                        EXPECT_NE(nullptr, page_local);
                        EXPECT_EQ(0,
                                  std::strcmp(std::to_string(temp_page_id.page_no).c_str(), (page_local->get_data())));
                        EXPECT_EQ(true, bpm->unpin_page(temp_page_id, false));
                        // If the page is still in buffer pool then put it in free list,
                        // else also we are happy
                        EXPECT_EQ(true, bpm->delete_page(temp_page_id));
                    }

                    auto *page = bpm->fetch_page(page_ids[j]);
                    while (page == nullptr) {
                        page = bpm->fetch_page(page_ids[j]);
                    }
                    EXPECT_NE(nullptr, page);
                    if (j % 2 == 0) {
                        EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                        EXPECT_EQ(true, bpm->unpin_page(page_ids[j], false));
                    } else {
                        EXPECT_EQ(0, std::strcmp((std::string("Hard") + std::to_string(page_ids[j].page_no)).c_str(),
                                                 (page->get_data())));
                        EXPECT_EQ(true, bpm->unpin_page(page_ids[j], false));
                    }
                    j = (j + 1);

                    page = bpm->new_page(&temp_page_id);
                    while (page == nullptr) {
                        page = bpm->new_page(&temp_page_id);
                    }
                    EXPECT_NE(nullptr, page);
                    strcpy(page->get_data(), std::to_string(temp_page_id.page_no).c_str());
                    // FLush page instead of unpining with true
                    EXPECT_EQ(true, bpm->flush_page(temp_page_id));
                    EXPECT_EQ(true, bpm->unpin_page(temp_page_id, false));

                    // Flood with new pages
                    for (int k = 0; k < 10; k++) {
                        PageId flood_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                        auto *flood_page = bpm->new_page(&flood_page_id);
                        while (flood_page == nullptr) {
                            flood_page = bpm->new_page(&flood_page_id);
                        }
                        EXPECT_NE(nullptr, flood_page);
                        EXPECT_EQ(true, bpm->unpin_page(flood_page_id, false));
                        // If the page is still in buffer pool then put it in free list,
                        // else also we are happy
                        EXPECT_EQ(true, bpm->delete_page(flood_page_id));
                    }
                }
            }));
        }

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }

        for (int j = 0; j < buffer_pool_size; j++) {
            EXPECT_EQ(true, bpm->delete_page(page_ids[j]));
        }
    }
}

#define TEST_INDEX
#include "system/sm_manager.h"

class IndexTests : public ::testing::Test {
public:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<RmManager> rm_manager_;
    std::unique_ptr<IxManager> ix_manager_;
    std::unique_ptr<SmManager> sm_manager_;
    std::string TEST_INDEX_NAME = "TEST_INDEX";

    int key_len_;
    char* key_buf_;

    std::string tab_name_ = "test1";
    std::vector<std::string> col_names_;

public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager

        disk_manager_ = std::make_unique<DiskManager>();
        buffer_pool_manager_ = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager_.get());
        rm_manager_ = std::make_unique<RmManager>(disk_manager_.get(), buffer_pool_manager_.get());
        ix_manager_ = std::make_unique<IxManager>(disk_manager_.get(), buffer_pool_manager_.get());
        sm_manager_ = std::make_unique<SmManager>(disk_manager_.get(), buffer_pool_manager_.get(), rm_manager_.get(), ix_manager_.get());

        // 如果测试目录不存在，则先创建测试目录
        if (disk_manager_->is_dir(TEST_INDEX_NAME)) {
            disk_manager_->destroy_dir(TEST_INDEX_NAME);
        }
        assert(!disk_manager_->is_dir(TEST_INDEX_NAME));

        sm_manager_->create_db(TEST_INDEX_NAME);
        sm_manager_->open_db(TEST_INDEX_NAME);
    }

    IxIndexHandle* GetIh(int length) {
        key_len_ = length;

        std::string col_name = "id";
        std::vector<ColDef> col_defs;
        ColDef col_def{col_name, TYPE_STRING, length};
        col_defs.push_back(col_def);
        col_names_.push_back(col_name);

        sm_manager_->create_table(tab_name_, col_defs, nullptr);
        sm_manager_->create_index(tab_name_, col_names_, nullptr);
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, col_names_);
        return sm_manager_->ihs_.at(index_name).get();
    }

    auto SetKey(int key) -> char* {
        key_buf_ = new char[key_len_];
        memset(key_buf_, 0, key_len_);
        std::string key_str = std::to_string(key);
        memcpy(key_buf_, key_str.c_str(), key_str.size());
        return key_buf_;
    }

// This function is called after every test.
    void TearDown() override {
        sm_manager_->drop_index(tab_name_, col_names_, nullptr);
        sm_manager_->drop_table(tab_name_, nullptr);
        sm_manager_->close_db();
        sm_manager_->drop_db(TEST_INDEX_NAME);
    };
};

TEST_F(IndexTests, NodeTest1) {
    auto ih = IndexTests::GetIh(5);

    IxFileHdr* ix_hdr = ih->getFileHdr();
    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";

    std::vector<int> keys;
    std::vector<int> delete_keys;
    std::vector<int> remain_keys;

    for (int j = 1; j <= 311; j++) {
        keys.push_back(j);
        if ( j % 2 == 0 ) {
            delete_keys.push_back(j);
        } else {
            remain_keys.push_back(j);
        }
    }

    std::random_shuffle(keys.begin(), keys.end());

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->insert_entry(key_buf, {0, key}, nullptr);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 1);
        EXPECT_EQ(res.back(), rid);
    }

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 1);
        EXPECT_EQ(res.back(), rid);
    }

    for (auto& key: delete_keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->delete_entry(key_buf, nullptr);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 0);
    }
}

TEST_F(IndexTests, InsertTest1) {
    auto ih = IndexTests::GetIh(300);

    IxFileHdr* ix_hdr = ih->getFileHdr();
    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";

    std::vector<int> keys;
    for (int j = 1; j <= 1890; j++) {
        keys.push_back(j);
    }

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->insert_entry(key_buf, {0, key}, nullptr);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 1);
        EXPECT_EQ(res.back(), rid);
    }

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 1);
        EXPECT_EQ(res.back(), rid);
    }
}

TEST_F(IndexTests, InsertTest2) {
    auto ih = IndexTests::GetIh(300);

    IxFileHdr* ix_hdr = ih->getFileHdr();
    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";

    std::vector<int> keys;
    for (int j = 1; j <= 18900; j++) {
        keys.push_back(j);
    }

    std::random_shuffle(keys.begin(), keys.end());

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->insert_entry(key_buf, {0, key}, nullptr);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 1);
        EXPECT_EQ(res.back(), rid);
    }

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 1);
        EXPECT_EQ(res.back(), rid);
    }
}

TEST_F(IndexTests, DeleteTest1) {
    auto ih = IndexTests::GetIh(300);

    IxFileHdr* ix_hdr = ih->getFileHdr();
    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";

    std::vector<int> keys;
    std::vector<int> delete_keys;
    std::vector<int> remain_keys;

    for (int j = 1; j <= 10000; j++) {
        keys.push_back(j);
        if (j % 2 != 0) {
            delete_keys.push_back(j);
        } else {
            remain_keys.push_back(j);
        }
    }

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->insert_entry(key_buf, {0, key}, nullptr);
    }

    for (auto& key: delete_keys) {
        std::vector<Rid> res;
        char* key_buf = IndexTests::SetKey(key);
        ih->delete_entry(key_buf, nullptr);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 0);
    }

    for (auto& key: remain_keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_NE(res.size(), 0);
        EXPECT_EQ(res.front(), rid);
    }

    for (auto& key: remain_keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->delete_entry(key_buf, nullptr);
        EXPECT_EQ(res.size(), 0);
    }

    EXPECT_EQ(ih->getFileHdr()->num_pages_, 2);
    EXPECT_EQ(ih->getFileHdr()->root_page_, IX_NO_PAGE);
}

TEST_F(IndexTests, DeleteTest2) {
    auto ih = IndexTests::GetIh(300);

    IxFileHdr* ix_hdr = ih->getFileHdr();
    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";

    std::vector<int> keys;
    std::vector<int> delete_keys;
    std::vector<int> remain_keys;

    for (int j = 1; j <= 10000; j++) {
        keys.push_back(j);
        if (j % 5 != 1) {
            delete_keys.push_back(j);
        } else {
            remain_keys.push_back(j);
        }
    }

    std::random_shuffle(keys.begin(), keys.end());
    std::random_shuffle(delete_keys.begin(), delete_keys.end());

    for (auto& key: keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->insert_entry(key_buf, {0, key}, nullptr);
    }

    for (auto& key: delete_keys) {
        std::vector<Rid> res;
        char* key_buf = IndexTests::SetKey(key);
        ih->delete_entry(key_buf, nullptr);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 0);
    }

    for (auto& key: remain_keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_NE(res.size(), 0);
        EXPECT_EQ(res.front(), rid);
    }

    for (auto& key: remain_keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->delete_entry(key_buf, nullptr);
        EXPECT_EQ(res.size(), 0);
    }

    EXPECT_EQ(ih->getFileHdr()->num_pages_, 2);
    EXPECT_EQ(ih->getFileHdr()->root_page_, IX_NO_PAGE);
}

TEST_F(IndexTests, MixTest1) {
    auto ih = IndexTests::GetIh(300);

    IxFileHdr* ix_hdr = ih->getFileHdr();
    std::cout << "Size of tree node: " << ix_hdr->btree_order_ << "\n\n";

    std::vector<int> keys;
    std::vector<int> delete_keys;
    std::vector<int> remain_keys;

    for (int j = 1; j <= 10000; j++) {
        keys.push_back(j);
    }

    std::random_shuffle(keys.begin(), keys.end());

    for (int i = 0; i < keys.size(); i++) {
        std::vector<Rid> res;
        Rid rid{0, keys[i]};
        char* key_buf = IndexTests::SetKey(keys[i]);

        remain_keys.push_back(keys[i]);
        ih->insert_entry(key_buf, rid, nullptr);

        if (i % 2 == 0) {
            int del = remain_keys[rand() % remain_keys.size()];
            auto it = std::find(remain_keys.begin(), remain_keys.end(), del);
            remain_keys.erase(it);

            key_buf = IndexTests::SetKey(del);
            ih->delete_entry(key_buf, nullptr);
            delete_keys.push_back(del);
        }
    }

    for (auto& key: delete_keys) {
        std::vector<Rid> res;
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_EQ(res.size(), 0);
    }

    for (auto& key: remain_keys) {
        std::vector<Rid> res;
        Rid rid{0, key};
        char* key_buf = IndexTests::SetKey(key);
        ih->get_value(key_buf, &res, nullptr);
        EXPECT_NE(res.size(), 0);
        EXPECT_EQ(res.front(), rid);
    }

    for (auto& key: remain_keys) {
        char* key_buf = IndexTests::SetKey(key);
        ih->delete_entry(key_buf, nullptr);
    }

    EXPECT_EQ(ih->getFileHdr()->num_pages_, 2);
    EXPECT_EQ(ih->getFileHdr()->root_page_, IX_NO_PAGE);
}