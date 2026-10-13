#include "cabinflow/agent/inference/native_backends.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <thread>

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cerr << "usage: rag_probe SITE_PACKAGES MODEL_DIRECTORY INDEX_DIRECTORY QUERY\n";
            return 2;
        }
        using namespace cabinflow::agent::inference;
        PythonRagBackend backend(argv[1], argv[2], argv[3], 2);
        // 取消必须丢弃结果；下一次显式调用仍能工作，不复用取消时的缓存。
        const auto cancelled = backend.search(argv[4], [] { return true; });
        if (cancelled.error != BackendError::kCancelled || cancelled.value.match)
            throw std::runtime_error("RAG cancellation contract failed");
        const auto invalid = backend.search({}, [] { return false; });
        if (invalid.error != BackendError::kInvalidInput || invalid.value.match)
            throw std::runtime_error("RAG invalid input contract failed");
        unsigned checks = 0;
        const auto cancelled_after_encode = backend.search(argv[4], [&] { return ++checks >= 2; });
        if (cancelled_after_encode.error != BackendError::kCancelled || cancelled_after_encode.value.match)
            throw std::runtime_error("RAG post-encode cancellation contract failed");
        BackendResult<RagSearchResult> result;
        // 查询在 worker，析构在主线程：验证生产组合采用的 GIL/解释器所有权。
        std::thread worker([&] { result = backend.search(argv[4], [] { return false; }); });
        worker.join();
        if (!result) throw std::runtime_error(result.detail);
        nlohmann::json report = {{"matched", result.value.match.has_value()}, {"simulated_knowledge", true}};
        if (result.value.match) {
            const auto& match = *result.value.match;
            report["document"] = {{"id", match.id}, {"section", match.section},
                                  {"subsection", match.subsection}, {"text", match.text},
                                  {"similarity", match.similarity}};
        }
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rag_probe failed: " << error.what() << '\n';
        return 1;
    }
}
