/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <fstream>
#include <iostream>

#include "all.h"
#include "json.hpp"

// 加载默认配置：从 master_config.json 文件中读取系统全局配置项
// 并存储到全局 KV 数据库 key_sql 中，供后续系统组件使用
void load_default_config()
{
    std::ifstream file("../master_config.json");
    if (!file.is_open()) {
        return;
    }
    nlohmann::json req_body;
    try {
        file >> req_body;
    } catch (...) {
        file.close();
        return;
    }
    file.close();

    for (auto it = req_body.begin(); it != req_body.end(); ++it) {
        if (req_body[it.key()].is_number()) {
            key_sql[(std::string)it.key()] = (int)it.value();
        }
        if (req_body[it.key()].is_string()) {
            key_sql[(std::string)it.key()] = (std::string)it.value();
        }
    }
}
