#include "unit_test.h"
#include "gtest/gtest.h"

//const char *server_host = "127.0.0.1";
//int server_port = PORT_DEFAULT;
//
//TEST(IndexTest1, withoutIndex) {
//    int sockfd = init_tcp_sock(server_host, server_port);
//    std::string sql;
//
//    int scale = 2000;
//    std::vector<int> entries;
//
//    for (int i = 0; i < scale; i++) {
//        entries.push_back(i);
//    }
//    std::random_shuffle(entries.begin(), entries.end());
//    for (int i = 0; i < scale / 2; i++) {
//        entries.pop_back();
//    }
//
//    sql = "create table t(v int, s char(500), f float);";
//    send_recv_sql(sockfd, sql);
//
//    for (auto& entry: entries) {
//        std::stringstream ss;
//        ss << "insert into t values (" << entry << ", '" << entry * 1000 <<"', " << entry + 0.5 << ");";
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//
//    for (int i = -10; i < scale + 10; i++) {
//        std::stringstream ss;
//        int offset = i % 3;
//        if (i % 5 == 1) {
//            ss << "select * from t where v > " << i << " and v <= " << i + offset << " order by v;";
//        } else if (i % 5 == 2) {
//            ss << "select * from t where v >= " << i << " and v < " << i + offset << " order by v;";
//        } else if (i % 5 == 3) {
//            ss << "select * from t where v >= " << i << " and v <= " << i + offset << " order by v;";
//        } else if (i % 5 == 4) {
//            ss << "select * from t where v > " << i << " and v < " << i + offset << " order by v;";
//        } else {
//            ss << "select * from t where v = " << i << " order by v;";
//        }
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//
//    for (int i = -10; i < scale + 10; i++) {
//        std::stringstream ss;
//        if (i % 5 == 1) {
//            ss << "select * from t where s > '" << i * 1000 <<"' and s <= '"<< (i + 1) * 1000 << "' order by s;";
//        } else if (i % 5 == 2) {
//            ss << "select * from t where s >= '" << i * 1000 <<"' and s < '"<< (i + 1) * 1000 << "' order by s;";
//        } else if (i % 5 == 3) {
//            ss << "select * from t where s >= '" << i * 1000 <<"' and s <= '"<< (i + 1) * 1000 << "' order by s;";
//        } else if (i % 5 == 4) {
//            ss << "select * from t where s > '" << i * 1000 <<"' and s < '"<< (i + 1) * 1000 << "' order by s;";
//        } else {
//            ss << "select * from t where s = '" << i * 1000 << "' order by s;";
//        }
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//    for (int i = -10; i < scale + 10; i++) {
//        std::stringstream ss;
//        int offset = i % 3;
//        if (i % 5 == 1) {
//            ss << "select * from t where f > " << i << " and f <= " << i + 0.5 + offset << " order by f;";
//        } else if (i % 5 == 2) {
//            ss << "select * from t where f >= " << i << " and f < " << i + 0.5 + offset << " order by f;";
//        } else if (i % 5 == 3) {
//            ss << "select * from t where f >= " << i << " and f <= " << i + 0.5 + offset << " order by f;";
//        } else if (i % 5 == 4) {
//            ss << "select * from t where f > " << i << " and f < " << i + 0.5 + offset << " order by f;";
//        } else {
//            ss << "select * from t where f = " << i + 0.5<< " order by f;";
//        }
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//}
//
//TEST(IndexTest1, withIndex) {
//    int sockfd = init_tcp_sock(server_host, server_port);
//    std::string sql;
//
//    int scale = 2000;
//    std::vector<int> entries;
//
//    for (int i = 0; i < scale; i++) {
//        entries.push_back(i);
//    }
//    std::random_shuffle(entries.begin(), entries.end());
//    for (int i = 0; i < scale / 2; i++) {
//        entries.pop_back();
//    }
//
//    sql = "create table t(v int, s char(500), f float);";
//    send_recv_sql(sockfd, sql);
//
//    sql = "create index t(f);";
//    send_recv_sql(sockfd, sql);
//    sql = "create index t(v);";
//    send_recv_sql(sockfd, sql);
//    sql = "create index t(s);";
//    send_recv_sql(sockfd, sql);
//
//    for (auto& entry: entries) {
//        std::stringstream ss;
//        ss << "insert into t values (" << entry << ", '" << entry * 1000 <<"', " << entry + 0.5 << ");";
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//
//    for (int i = -10; i < scale + 10; i++) {
//        std::stringstream ss;
//        int offset = i % 3;
//        if (i % 5 == 1) {
//            ss << "select * from t where v > " << i << " and v <= " << i + offset << ";";
//        } else if (i % 5 == 2) {
//            ss << "select * from t where v >= " << i << " and v < " << i + offset << ";";
//        } else if (i % 5 == 3) {
//            ss << "select * from t where v >= " << i << " and v <= " << i + offset << ";";
//        } else if (i % 5 == 4) {
//            ss << "select * from t where v > " << i << " and v < " << i + offset << ";";
//        } else {
//            ss << "select * from t where v = " << i << ";";
//        }
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//
//    for (int i = -10; i < scale + 10; i++) {
//        std::stringstream ss;
//        if (i % 5 == 1) {
//            ss << "select * from t where s > '" << i * 1000 <<"' and s <= '"<< (i + 1) * 1000 << "';";
//        } else if (i % 5 == 2) {
//            ss << "select * from t where s >= '" << i * 1000 <<"' and s < '"<< (i + 1) * 1000 << "';";
//        } else if (i % 5 == 3) {
//            ss << "select * from t where s >= '" << i * 1000 <<"' and s <= '"<< (i + 1) * 1000 << "';";
//        } else if (i % 5 == 4) {
//            ss << "select * from t where s > '" << i * 1000 <<"' and s < '"<< (i + 1) * 1000 << "';";
//        } else {
//            ss << "select * from t where s = '" << i * 1000 << "';";
//        }
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//    for (int i = -10; i < scale + 10; i++) {
//        std::stringstream ss;
//        int offset = i % 3;
//        if (i % 5 == 1) {
//            ss << "select * from t where f > " << i << " and f <= " << i + 0.5 + offset << ";";
//        } else if (i % 5 == 2) {
//            ss << "select * from t where f >= " << i << " and f < " << i + 0.5 + offset << ";";
//        } else if (i % 5 == 3) {
//            ss << "select * from t where f >= " << i << " and f <= " << i + 0.5 + offset << ";";
//        } else if (i % 5 == 4) {
//            ss << "select * from t where f > " << i << " and f < " << i + 0.5 + offset << ";";
//        } else {
//            ss << "select * from t where f = " << i + 0.5<< ";";
//        }
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//}
//
//TEST(IndexTest2, withoutIndex) {
//    int sockfd = init_tcp_sock(server_host, server_port);
//    std::string sql;
//
//    int scale = 30;
//    std::vector<int> entries;
//
//    for (int i = 0; i < scale; i++) {
//        entries.push_back(i);
//    }
//
//    std::sort(entries.begin(), entries.end(), [](const int a, const int b) {
//        return std::to_string(a) < std::to_string(b);
//    });
//
//    sql = "create table t(v int, s char(500), f float);";
//    send_recv_sql(sockfd, sql);
//
//    for (auto& entry: entries) {
//        std::stringstream ss;
//        ss << "insert into t values (" << entry << ", '" << entry * 1000 <<"', " << entry + 0.5 << ");";
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//
//    for (auto& entry: entries) {
//        std::stringstream ss;
//        ss << "select * from t where s > '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s >= '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s < '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s <= '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s = '" << entry * 1000 << "';";
//    }
//
//    close(sockfd);
//}
//
//TEST(IndexTest2, withIndex) {
//    int sockfd = init_tcp_sock(server_host, server_port);
//    std::string sql;
//
//    int scale = 30;
//    std::vector<int> entries;
//
//    for (int i = 0; i < scale; i++) {
//        entries.push_back(i);
//    }
//
//    sql = "create table t(v int, s char(500), f float);";
//    send_recv_sql(sockfd, sql);
//
//    sql = "create index t(f);";
//    send_recv_sql(sockfd, sql);
//    sql = "create index t(v);";
//    send_recv_sql(sockfd, sql);
//    sql = "create index t(s);";
//    send_recv_sql(sockfd, sql);
//
//    for (auto& entry: entries) {
//        std::stringstream ss;
//        ss << "insert into t values (" << entry << ", '" << entry * 1000 <<"', " << entry + 0.5 << ");";
//        sql = ss.str();
//        send_recv_sql(sockfd, sql);
//    }
//
//    std::sort(entries.begin(), entries.end(), [](const int a, const int b) {
//        return std::to_string(a) < std::to_string(b);
//    });
//
//    for (auto& entry: entries) {
//        std::stringstream ss;
//        ss << "select * from t where s > '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s >= '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s < '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s <= '" << entry * 1000 << "';";
//        send_recv_sql(sockfd, ss.str()); ss.str("");
//        ss << "select * from t where s = '" << entry * 1000 << "';";
//    }
//
//    close(sockfd);
//}

int main(int argc, char *argv[]) {
    const char *server_host = "127.0.0.1";  // 127.0.0.1 192.168.31.25
    int server_port = PORT_DEFAULT;
    std::string test_name = argv[1];


    int sockfd;

    sockfd = init_tcp_sock(server_host, server_port);

    if (sockfd < 0) {
        return 1;
    }

    std::ifstream test;
    std::string sql;

    test.open(test_name);
    while(std::getline(test, sql)) {
        send_recv_sql(sockfd, sql);
    }

    close(sockfd);

    return 0;
}