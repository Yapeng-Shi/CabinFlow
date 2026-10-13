#include "cabinflow/agent/inference/native_backends.hpp"

#include <pybind11/embed.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace cabinflow::agent::inference {
namespace {
namespace py = pybind11;
std::atomic<bool> interpreter_used{false};

bool claim_interpreter() {
    if (interpreter_used.exchange(true))
        throw std::runtime_error("RAG Python interpreter is single-use per process");
    return false;  // CPython 不接管应用的信号处理。
}

void require_directory(const std::string& path) {
    if (!std::filesystem::path(path).is_absolute() || !std::filesystem::is_directory(path))
        throw std::invalid_argument("RAG requires explicit existing absolute directories");
}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead == 0) return false;
        if (lead < 0x80) continue;
        unsigned count;
        std::uint32_t code;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; code = lead & 0x1f; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; code = lead & 0xf; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; code = lead & 7; }
        else return false;
        if (i + count > text.size()) return false;
        for (unsigned j = 0; j < count; ++j) {
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((count == 2 && code < 0x800) || (count == 3 && code < 0x10000) ||
            (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) return false;
    }
    return true;
}
}  // namespace

struct __attribute__((visibility("hidden"))) PythonRagBackend::Impl {
    // 逆序销毁：先重新取得 GIL，再释放 Python 对象，最后终结解释器。
    py::scoped_interpreter interpreter{claim_interpreter(), 0, nullptr, false};
    py::object searcher;
    std::unique_ptr<py::gil_scoped_release> release;
    const std::thread::id owner_thread{std::this_thread::get_id()};

    Impl(const std::string& site, const std::string& model, const std::string& index, int threads) {
        std::string failure;
        try {
            require_directory(site);
            require_directory(model);
            require_directory(index);
            auto paths = py::module_::import("sys").attr("path");
            paths.attr("insert")(0, site);
            paths.attr("insert")(0, CABINFLOW_RAG_PYTHON_MODULE_DIR);
            auto module = py::module_::import("cabinflow_rag");
            searcher = module.attr("Searcher")(site, model, index, threads);
        } catch (const py::error_already_set& error) {
            // Python 异常对象必须在解释器仍存活且持有 GIL 时销毁。
            failure = error.what();
        }
        if (!failure.empty()) throw std::runtime_error("RAG initialization failed: " + failure);
        // 上面所有临时 Python 对象已释放；应用等待 worker 时不能一直持有 GIL。
        release = std::make_unique<py::gil_scoped_release>();
    }
    ~Impl() {
        if (std::this_thread::get_id() != owner_thread) std::terminate();
    }
};

PythonRagBackend::PythonRagBackend(std::string site, std::string model, std::string index, int threads)
    : impl_(std::make_unique<Impl>(site, model, index, threads)) {}
PythonRagBackend::~PythonRagBackend() = default;

BackendResult<RagSearchResult> PythonRagBackend::search(
    std::string_view query, const CancellationCheck& cancelled) {
    if (query.empty() || query.size() > 16 * 1024 || !valid_utf8(query))
        return {{}, BackendError::kInvalidInput, "invalid_rag_query"};
    py::gil_scoped_acquire gil;
    if (cancelled()) return {{}, BackendError::kCancelled, "work_cancelled"};
    try {
        auto found = impl_->searcher.attr("search")(std::string(query));
        // Torch 调用不硬中断；取消后丢弃结果，调用返回后才能清理 work。
        if (cancelled()) return {{}, BackendError::kCancelled, "work_cancelled"};
        RagSearchResult result;
        if (!found.is_none()) {
            auto row = found.cast<py::dict>();
            RagDocument document{row["id"].cast<std::uint32_t>(), row["section"].cast<std::string>(),
                row["subsection"].cast<std::string>(), row["text"].cast<std::string>(),
                row["similarity"].cast<float>()};
            if (document.section.empty() || document.subsection.empty() || document.text.empty() ||
                document.text.size() > 16 * 1024 || !valid_utf8(document.text) ||
                !std::isfinite(document.similarity) || document.similarity < 0.5f || document.similarity > 1.00001f)
                return {{}, BackendError::kInferenceFailed, "invalid_rag_document"};
            result.match = std::move(document);
        }
        return {std::move(result), BackendError::kNone, {}};
    } catch (const py::error_already_set& error) {
        return {{}, BackendError::kInferenceFailed, error.what()};
    } catch (const py::cast_error& error) {
        return {{}, BackendError::kInferenceFailed, error.what()};
    }
}
}  // namespace cabinflow::agent::inference
