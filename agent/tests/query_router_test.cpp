#include "query_router.hpp"
#include "query_classifier.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    try {
        edge_llm_rag::QueryClassifier original;
        std::vector<std::string> cases{"", "法国的首都是什么", "机油", "空调滤清器多久更换", "发动机警告灯",
            "自动泊车", "娱乐", "推荐旅行路线", "推荐一下旅游", "笑话", "导航怎么玩", "AAAS"};
        const std::vector<std::string> words{"故障", "发动机", "保养", "轮胎更换", "娱乐", "旅游", "推荐一下",
            "今天", "现在", "天气", "百科", "故事", "怎么走", "空调控制", "电瓶", "润滑"};
        for (const auto& a : words) for (const auto& b : words)
            for (int length : {0, 20, 50, 100}) cases.push_back(a + b + std::string(length, 'x'));
        for (const auto& query : cases) {
            const auto type = original.classify_query(query).query_type;
            const auto expected = type == edge_llm_rag::QueryClassification::EMERGENCY_QUERY ||
                type == edge_llm_rag::QueryClassification::FACTUAL_QUERY ? cabinflow::agent::v1::ANSWER_ROUTE_RAG_ONLY :
                type == edge_llm_rag::QueryClassification::CREATIVE_QUERY ? cabinflow::agent::v1::ANSWER_ROUTE_LLM_ONLY :
                cabinflow::agent::v1::ANSWER_ROUTE_HYBRID;
            if (cabinflow::agent::classify_answer_route(query) != expected)
                throw std::runtime_error("original route mismatch: " + query);
        }
        std::cout << "original tier routes matched for " << cases.size() << " cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
