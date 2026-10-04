#pragma once

#include <string>
#include <cockpit_text.pb.h>

namespace cabinflow::agent {

// Router 和终态消费者共用字段规则；歌词/回答文字不能成为另一个命令入口。
inline bool is_valid_music_command(const v1::MusicCommand& command) noexcept {
    using Command = v1::MusicCommand;
    switch (command.action()) {
    case Command::SEARCH:
        return !command.keyword().empty() && command.keyword().size() <= 16 * 1024 &&
               command.keyword().find_first_not_of(" \t\r\n") != std::string::npos &&
               command.result_index() == 0;
    case Command::SELECT:
        return command.keyword().empty() && command.result_index() > 0;
    case Command::PLAY:
    case Command::PAUSE:
    case Command::PREVIOUS:
    case Command::NEXT:
        return command.keyword().empty() && command.result_index() == 0;
    default:
        return false;
    }
}

}  // namespace cabinflow::agent
