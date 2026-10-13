#include "pcm_frame_buffer.hpp"
#include <stdexcept>
#include <iostream>

void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
int main() {
    try {
        PcmFrameBuffer buffer;
        std::array<float, WakeEngine::kFrameSamples> frame{};
        std::string pcm;
        for (int i = 0; i < WakeEngine::kFrameSamples; ++i) { pcm.push_back('\0'); pcm.push_back(static_cast<char>(0x80)); }
        require(buffer.append(pcm.data(), 1) && !buffer.pop(frame), "odd byte cannot form a frame");
        for (std::size_t i = 1; i < pcm.size(); ++i) {
            require(buffer.append(pcm.data() + i, 1), "bounded fragmented input");
            if (i + 1 < pcm.size()) require(!buffer.pop(frame), "partial frame never decoded");
        }
        require(buffer.pop(frame) && buffer.size() == 0 && !buffer.pop(frame), "frame emitted once");
        for (auto sample : frame) require(sample == -1.0F, "little endian signed PCM preserved");
        const std::string full(PcmFrameBuffer::kCapacity, '\0');
        require(buffer.append(full.data(), full.size()), "exact1s bound allowed");
        require(!buffer.append("x", 1) && buffer.size() == full.size(), "overflow rejected before modifying buffer");
        buffer.clear(); require(buffer.size() == 0 && !buffer.append(nullptr, 1), "reset and invalid buffer");
        require(buffer.append(pcm.data(), pcm.size()) && buffer.pop(frame), "new epoch does not retain old bytes");
        std::cout << "pcm_frame_buffer_test passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
