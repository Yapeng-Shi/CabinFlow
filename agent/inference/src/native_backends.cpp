#include "cabinflow/agent/inference/native_backends.hpp"
#include "wav.hpp"

#include "chat.h"
#include "llama.h"
#include "sampling.h"
#include "sherpa-onnx/c-api/c-api.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cabinflow::agent::inference {
namespace {

template <class T, auto Destroy>
using SdkOwner = std::unique_ptr<T, decltype(Destroy)>;

bool IsCancelled(const CancellationCheck& check) { return check && check(); }

template <class T>
BackendResult<T> Cancelled() {
    return {{}, BackendError::kCancelled, "Inference was cancelled"};
}

bool IsBlank(std::string_view text) {
    return text.empty() || std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

bool InvalidText(std::string_view text) {
    return IsBlank(text) || text.find('\0') != std::string_view::npos;
}

std::string RequireFile(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) == 0) {
        throw std::runtime_error("Missing or empty model file: " + path.string());
    }
    return path.string();
}

struct LlamaLibrary {
    LlamaLibrary() { llama_backend_init(); }
    ~LlamaLibrary() { llama_backend_free(); }
};

void InitializeLlama() {
    // 进程级 SDK 初始化只执行一次；模型和 context 仍由各后端实例独占。
    static LlamaLibrary library;
}

}  // namespace

struct SherpaAsrBackend::Impl {
    explicit Impl(const std::string& directory)
        : recognizer(nullptr, SherpaOnnxDestroyOnlineRecognizer) {
        const std::filesystem::path root(directory);
        const auto encoder = RequireFile(root / "encoder-epoch-99-avg-1.int8.onnx");
        const auto decoder = RequireFile(root / "decoder-epoch-99-avg-1.int8.onnx");
        const auto joiner = RequireFile(root / "joiner-epoch-99-avg-1.int8.onnx");
        const auto tokens = RequireFile(root / "tokens.txt");
        SherpaOnnxOnlineRecognizerConfig config{};
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.model_config.transducer.encoder = encoder.c_str();
        config.model_config.transducer.decoder = decoder.c_str();
        config.model_config.transducer.joiner = joiner.c_str();
        config.model_config.tokens = tokens.c_str();
        config.model_config.num_threads = 2;
        config.model_config.provider = "cpu";
        config.decoding_method = "greedy_search";
        recognizer.reset(SherpaOnnxCreateOnlineRecognizer(&config));
        if (!recognizer) throw std::runtime_error("Sherpa ASR model loading failed: " + directory);
    }

    SdkOwner<const SherpaOnnxOnlineRecognizer, SherpaOnnxDestroyOnlineRecognizer> recognizer;
};

SherpaAsrBackend::SherpaAsrBackend(std::string model_directory)
    : impl_(std::make_unique<Impl>(model_directory)) {}
SherpaAsrBackend::~SherpaAsrBackend() = default;

BackendResult<std::string> SherpaAsrBackend::transcribe(
    std::string_view wav, const CancellationCheck& cancelled) {
    if (IsCancelled(cancelled)) return Cancelled<std::string>();
    auto decoded = detail::DecodeAsrWav(wav);
    if (!decoded) return {{}, decoded.error, std::move(decoded.detail)};
    if (IsCancelled(cancelled)) return Cancelled<std::string>();
    try {
        const auto* recognizer = impl_->recognizer.get();
        SdkOwner<const SherpaOnnxOnlineStream, SherpaOnnxDestroyOnlineStream> stream(
            SherpaOnnxCreateOnlineStream(recognizer), SherpaOnnxDestroyOnlineStream);
        if (!stream) return {{}, BackendError::kInferenceFailed, "Sherpa ASR stream creation failed"};
        SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), 16000, decoded.value.data(),
                                           static_cast<int32_t>(decoded.value.size()));
        // 按 1.11.3 streaming transducer 的结束流程补 0.3 秒尾部，再通知输入终止。
        const std::array<float, 4800> tail{};
        SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), 16000, tail.data(), tail.size());
        SherpaOnnxOnlineStreamInputFinished(stream.get());
        while (SherpaOnnxIsOnlineStreamReady(recognizer, stream.get())) {
            if (IsCancelled(cancelled)) return Cancelled<std::string>();
            SherpaOnnxDecodeOnlineStream(recognizer, stream.get());
        }
        if (IsCancelled(cancelled)) return Cancelled<std::string>();
        SdkOwner<const SherpaOnnxOnlineRecognizerResult, SherpaOnnxDestroyOnlineRecognizerResult> result(
            SherpaOnnxGetOnlineStreamResult(recognizer, stream.get()),
            SherpaOnnxDestroyOnlineRecognizerResult);
        if (!result || !result->text || IsBlank(result->text)) {
            return {{}, BackendError::kInferenceFailed, "Sherpa ASR returned no recognized text"};
        }
        std::string text(result->text);
        if (IsCancelled(cancelled)) return Cancelled<std::string>();
        return {std::move(text), BackendError::kNone, {}};
    } catch (const std::exception& error) {
        return {{}, BackendError::kInferenceFailed, error.what()};
    }
}

struct LlamaBackend::Impl {
    Impl(const std::string& path, int threads, int context_size, int token_limit)
        : model(nullptr, llama_model_free), context(nullptr, llama_free),
          max_new_tokens(token_limit) {
        if (threads <= 0 || context_size <= 0 || token_limit <= 0 || token_limit >= context_size) {
            throw std::runtime_error("Llama requires positive threads/context/tokens and tokens < context");
        }
        RequireFile(path);
        InitializeLlama();
        auto model_params = llama_model_default_params();
        model_params.n_gpu_layers = 0;
        model.reset(llama_model_load_from_file(path.c_str(), model_params));
        if (!model) throw std::runtime_error("Llama model loading failed: " + path);
        if (llama_model_has_encoder(model.get())) {
            throw std::runtime_error("Llama backend requires a decoder-only language model");
        }
        vocab = llama_model_get_vocab(model.get());
        if (!vocab || !llama_model_chat_template(model.get(), nullptr)) {
            throw std::runtime_error("Llama model must provide a vocabulary and chat template");
        }
        templates = common_chat_templates_init(model.get(), "");
        if (!templates) throw std::runtime_error("Llama chat template initialization failed");
        auto params = llama_context_default_params();
        params.n_ctx = static_cast<uint32_t>(context_size);
        params.n_batch = static_cast<uint32_t>(std::min(context_size, 512));
        params.n_ubatch = static_cast<uint32_t>(std::min(context_size, 128));
        params.n_threads = threads;
        params.n_threads_batch = threads;
        params.offload_kqv = false;
        params.op_offload = false;
        context.reset(llama_init_from_model(model.get(), params));
        if (!context) throw std::runtime_error("Llama context creation failed");
    }

    // 声明顺序确保 context/templates 先释放，模型最后释放，避免 SDK 借用悬空。
    SdkOwner<llama_model, llama_model_free> model;
    SdkOwner<llama_context, llama_free> context;
    common_chat_templates_ptr templates;
    const llama_vocab* vocab{};
    int max_new_tokens;
};

LlamaBackend::LlamaBackend(std::string model_path, int threads, int context_size, int max_new_tokens)
    : impl_(std::make_unique<Impl>(model_path, threads, context_size, max_new_tokens)) {}
LlamaBackend::~LlamaBackend() = default;

BackendResult<std::string> LlamaBackend::generate(
    std::string_view text, const CancellationCheck& cancelled) {
    if (IsCancelled(cancelled)) return Cancelled<std::string>();
    if (InvalidText(text)) return {{}, BackendError::kInvalidInput, "LLM input text is empty or contains NUL"};
    try {
        // 每次请求清除 KV cache（历史 token 状态）并新建采样器，topic 间不共享对话。
        llama_memory_clear(llama_get_memory(impl_->context.get()), true);
        common_chat_templates_inputs inputs;
        common_chat_msg message;
        message.role = "user";
        message.content = std::string(text);
        inputs.messages.push_back(std::move(message));
        inputs.use_jinja = true;
        inputs.enable_thinking = false;
        inputs.chat_template_kwargs["enable_thinking"] = "false";
        const auto chat = common_chat_templates_apply(impl_->templates.get(), inputs);
        if (chat.prompt.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return {{}, BackendError::kInvalidInput, "LLM prompt exceeds tokenizer byte limit"};
        }
        const int32_t required = llama_tokenize(impl_->vocab, chat.prompt.data(),
            static_cast<int32_t>(chat.prompt.size()), nullptr, 0, true, true);
        if (required >= 0 || required == std::numeric_limits<int32_t>::min()) {
            return {{}, BackendError::kInferenceFailed, "Llama could not size prompt tokens"};
        }
        const auto token_count = static_cast<uint32_t>(-required);
        if (static_cast<uint64_t>(token_count) + impl_->max_new_tokens > llama_n_ctx(impl_->context.get())) {
            return {{}, BackendError::kInvalidInput, "LLM prompt plus output budget exceeds context"};
        }
        std::vector<llama_token> tokens(token_count);
        const int32_t actual = llama_tokenize(impl_->vocab, chat.prompt.data(),
            static_cast<int32_t>(chat.prompt.size()), tokens.data(), tokens.size(), true, true);
        if (actual != static_cast<int32_t>(tokens.size())) {
            return {{}, BackendError::kInferenceFailed, "Llama prompt tokenization failed"};
        }
        common_params_sampling sampling;
        sampling.seed = 42;
        sampling.temp = 0.7F;
        sampling.top_p = 0.8F;
        sampling.top_k = 20;
        sampling.min_p = 0.0F;
        sampling.penalty_last_n = 64;
        sampling.penalty_repeat = 1.0F;
        sampling.penalty_freq = 0.0F;
        sampling.penalty_present = 1.5F;
        SdkOwner<common_sampler, common_sampler_free> sampler(
            common_sampler_init(impl_->model.get(), sampling), common_sampler_free);
        if (!sampler) return {{}, BackendError::kInferenceFailed, "Llama sampler creation failed"};
        // 与锁定 CLI 一致，presence penalty 的最近 token 窗口包含原始 prompt。
        for (const auto token : tokens) common_sampler_accept(sampler.get(), token, false);
        for (size_t offset = 0; offset < tokens.size();) {
            if (IsCancelled(cancelled)) return Cancelled<std::string>();
            const auto count = std::min(tokens.size() - offset,
                                        static_cast<size_t>(llama_n_batch(impl_->context.get())));
            auto batch = llama_batch_get_one(tokens.data() + offset, static_cast<int32_t>(count));
            const int status = llama_decode(impl_->context.get(), batch);
            if (IsCancelled(cancelled)) return Cancelled<std::string>();
            if (status != 0) return {{}, BackendError::kInferenceFailed,
                                    "Llama prompt decode failed with status " + std::to_string(status)};
            offset += count;
        }
        std::string output;
        for (int produced = 0; produced < impl_->max_new_tokens; ++produced) {
            if (IsCancelled(cancelled)) return Cancelled<std::string>();
            llama_token token = common_sampler_sample(sampler.get(), impl_->context.get(), -1);
            if (IsCancelled(cancelled)) return Cancelled<std::string>();
            if (llama_vocab_is_eog(impl_->vocab, token)) {
                common_chat_syntax syntax;
                syntax.format = chat.format;
                syntax.reasoning_format = COMMON_REASONING_FORMAT_NONE;
                syntax.thinking_forced_open = chat.thinking_forced_open;
                syntax.parse_tool_calls = false;
                auto answer = common_chat_parse(output, false, syntax).content;
                if (IsCancelled(cancelled)) return Cancelled<std::string>();
                if (IsBlank(answer)) return {{}, BackendError::kInferenceFailed, "Llama returned an empty answer"};
                return {std::move(answer), BackendError::kNone, {}};
            }
            common_sampler_accept(sampler.get(), token, false);
            const int32_t piece_size = llama_token_to_piece(impl_->vocab, token, nullptr, 0, 0, false);
            if (piece_size == std::numeric_limits<int32_t>::min() || piece_size > 0) {
                return {{}, BackendError::kInferenceFailed, "Llama could not size token text"};
            }
            if (piece_size < 0) {
                std::string piece(static_cast<size_t>(-piece_size), '\0');
                const int32_t written = llama_token_to_piece(impl_->vocab, token, piece.data(),
                    static_cast<int32_t>(piece.size()), 0, false);
                if (written != static_cast<int32_t>(piece.size())) {
                    return {{}, BackendError::kInferenceFailed, "Llama token text conversion failed"};
                }
                output += piece;
            }
            auto batch = llama_batch_get_one(&token, 1);
            const int status = llama_decode(impl_->context.get(), batch);
            if (IsCancelled(cancelled)) return Cancelled<std::string>();
            if (status != 0) return {{}, BackendError::kInferenceFailed,
                                    "Llama token decode failed with status " + std::to_string(status)};
        }
        return {{}, BackendError::kInferenceFailed, "Llama reached max_new_tokens before end of generation"};
    } catch (const std::exception& error) {
        return {{}, BackendError::kInferenceFailed, error.what()};
    }
}

struct SherpaTtsBackend::Impl {
    explicit Impl(const std::string& directory) : tts(nullptr, SherpaOnnxDestroyOfflineTts) {
        const std::filesystem::path root(directory);
        const auto model = RequireFile(root / "model.onnx");
        const auto tokens = RequireFile(root / "tokens.txt");
        const auto lexicon = RequireFile(root / "lexicon.txt");
        const auto dictionary = (root / "dict").string();
        for (const auto* file : {"hmm_model.utf8", "idf.utf8", "jieba.dict.utf8", "user.dict.utf8",
                                 "stop_words.utf8", "pos_dict/char_state_tab.utf8",
                                 "pos_dict/prob_emit.utf8", "pos_dict/prob_start.utf8",
                                 "pos_dict/prob_trans.utf8"}) {
            RequireFile(root / "dict" / file);
        }
        const auto rules = RequireFile(root / "date.fst") + "," + RequireFile(root / "number.fst") +
                           "," + RequireFile(root / "new_heteronym.fst");
        SherpaOnnxOfflineTtsConfig config{};
        config.model.vits.model = model.c_str();
        config.model.vits.tokens = tokens.c_str();
        config.model.vits.lexicon = lexicon.c_str();
        config.model.vits.dict_dir = dictionary.c_str();
        config.model.vits.noise_scale = 0.667F;
        config.model.vits.noise_scale_w = 0.8F;
        config.model.vits.length_scale = 1.0F;
        config.model.num_threads = 2;
        config.model.provider = "cpu";
        config.rule_fsts = rules.c_str();
        config.max_num_sentences = 1;
        config.silence_scale = 1.0F;
        tts.reset(SherpaOnnxCreateOfflineTts(&config));
        if (!tts) throw std::runtime_error("Sherpa TTS model loading failed: " + directory);
    }

    SdkOwner<const SherpaOnnxOfflineTts, SherpaOnnxDestroyOfflineTts> tts;
};

SherpaTtsBackend::SherpaTtsBackend(std::string model_directory)
    : impl_(std::make_unique<Impl>(model_directory)) {}
SherpaTtsBackend::~SherpaTtsBackend() = default;

BackendResult<WavAudio> SherpaTtsBackend::synthesize(
    std::string_view text, const CancellationCheck& cancelled) {
    if (IsCancelled(cancelled)) return Cancelled<WavAudio>();
    if (InvalidText(text)) return {{}, BackendError::kInvalidInput, "TTS input text is empty or contains NUL"};
    try {
        const std::string owned_text(text);
        SdkOwner<const SherpaOnnxGeneratedAudio, SherpaOnnxDestroyOfflineTtsGeneratedAudio> audio(
            SherpaOnnxOfflineTtsGenerate(impl_->tts.get(), owned_text.c_str(), 0, 1.0F),
            SherpaOnnxDestroyOfflineTtsGeneratedAudio);
        // 单次 SDK 调用无法硬中断；返回后检查取消并丢弃完整结果，避免过期音频发布。
        if (IsCancelled(cancelled)) return Cancelled<WavAudio>();
        if (!audio) return {{}, BackendError::kInferenceFailed, "Sherpa TTS generation failed"};
        auto result = detail::EncodePcm16Wav(audio->samples, audio->n, audio->sample_rate);
        if (IsCancelled(cancelled)) return Cancelled<WavAudio>();
        return result;
    } catch (const std::exception& error) {
        return {{}, BackendError::kInferenceFailed, error.what()};
    }
}

}  // namespace cabinflow::agent::inference
