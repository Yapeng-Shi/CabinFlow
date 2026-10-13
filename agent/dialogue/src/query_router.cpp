#include "query_router.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace cabinflow::agent {
v1::AnswerRoute classify_answer_route(std::string_view query) {
    static const std::unordered_map<std::string, std::vector<std::string>> words{
        {"emergency", {"故障", "警告", "危险", "紧急", "异常", "失灵", "失效", "损坏",
            "发动机故障", "制动故障", "转向故障", "电气故障", "安全气囊", "ABS故障"}},
        {"technical", {"发动机", "制动", "变速箱", "电气", "空调", "转向", "悬挂", "轮胎",
            "机油", "冷却液", "制动液", "变速箱油", "电瓶", "发电机", "起动机"}},
        {"maintenance", {"保养", "维修", "更换", "检查", "清洁", "调整", "润滑", "紧固",
            "定期保养", "机油更换", "滤清器", "火花塞", "制动片", "轮胎更换"}},
        {"feature", {"自动泊车", "车道保持", "定速巡航", "导航", "娱乐", "空调控制",
            "座椅调节", "后视镜", "雨刷", "灯光", "音响", "蓝牙"}},
        {"question", {"什么", "怎么", "如何", "为什么", "哪里", "何时", "多少", "哪个",
            "吗", "呢", "嘛", "能不能", "可不可以", "有没有", "推荐一下", "怎么去", "去哪里", "怎么玩"}},
        {"creative", {"推荐", "建议", "想法", "创意", "优化", "改进", "设计", "规划",
            "旅游", "旅行", "出行", "景点", "门票", "酒店", "民宿", "机票", "火车票", "高铁",
            "行程", "路线", "攻略", "签证", "租车", "自驾", "海岛", "海滩", "公园", "博物馆",
            "古镇", "温泉", "夜市", "特产", "美食", "摄影", "网红", "打卡", "露营", "徒步",
            "游玩", "娱乐", "主题乐园", "游乐园", "迪士尼", "环球影城", "水上乐园",
            "演唱会", "音乐节", "展览", "赛事", "滑雪", "潜水", "骑行", "登山",
            "预订", "订票", "订酒店", "退改签", "行李", "登机", "值机", "改签", "延误", "转机",
            "天气", "笑话", "故事", "新闻", "百科", "科普", "翻译", "计算", "单位换算",
            "今天", "明天", "现在", "附近", "哪里有", "怎么走"}}};
    auto contains = [&](const char* category, const std::string& word) {
        const auto& list = words.at(category);
        return std::find(list.begin(), list.end(), word) != list.end();
    };
    std::vector<std::string> keywords;
    for (const auto& [category, list] : words) {
        static_cast<void>(category);
        for (const auto& word : list)
            if (query.find(word) != std::string_view::npos) keywords.push_back(word);
    }
    float factual = 0, creative = 0;
    unsigned technical = 0;
    bool emergency = false;
    // 保留跨类别重复词的计数及 UTF-8 字节长度；去重或改成字数会改变旧路由。
    for (const auto& word : keywords) {
        emergency = emergency || contains("emergency", word);
        if (contains("technical", word)) { factual += 0.4f; ++technical; }
        if (contains("maintenance", word)) factual += 0.4f;
        if (contains("feature", word)) factual += 0.5f;
        if (contains("creative", word)) creative += 0.3f;
    }
    const float complexity = std::min(1.f, static_cast<float>(query.size()) / 100.f) * 0.3f +
        std::min(1.f, static_cast<float>(keywords.size()) / 10.f) * 0.4f +
        std::min(1.f, static_cast<float>(technical) / 5.f) * 0.3f;
    if (emergency || factual >= 0.5f) return v1::ANSWER_ROUTE_RAG_ONLY;
    if (creative > 0.6f) return v1::ANSWER_ROUTE_LLM_ONLY;
    if (complexity > 0.6f) return v1::ANSWER_ROUTE_HYBRID;
    return v1::ANSWER_ROUTE_HYBRID;  // UNKNOWN 也保持原来的混合路由。
}
}  // namespace cabinflow::agent
