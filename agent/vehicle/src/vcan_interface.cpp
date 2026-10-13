#include <cabinflow/agent/vcan_interface.hpp>

#include <cerrno>
#include <cstring>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cabinflow::agent::vehicle_can {
namespace {
struct OwnedDescriptor {
    int fd;
    ~OwnedDescriptor() { if (fd >= 0) ::close(fd); }
};

bool failure(std::string& detail, const char* operation) {
    detail = std::string(operation) + ": " + std::strerror(errno);
    return false;
}
}  // namespace

bool is_vcan_interface(unsigned interface_index, std::string& detail) {
    OwnedDescriptor descriptor{::socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE)};
    if (descriptor.fd < 0) return failure(detail, "vcan kind netlink socket");
    struct {
        nlmsghdr header;
        ifinfomsg link;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.header.nlmsg_seq = 1;
    request.link.ifi_family = AF_UNSPEC;
    request.link.ifi_index = static_cast<int>(interface_index);
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (::sendto(descriptor.fd, &request, request.header.nlmsg_len, 0,
                 reinterpret_cast<const sockaddr*>(&kernel), sizeof(kernel)) !=
        static_cast<ssize_t>(request.header.nlmsg_len)) return failure(detail, "vcan kind netlink send");
    pollfd pending{descriptor.fd, POLLIN, 0};
    const auto ready = ::poll(&pending, 1, 250);
    if (ready <= 0 || (pending.revents & POLLIN) == 0) {
        detail = "kernel vcan kind query unavailable or timed out";
        return false;
    }
    alignas(nlmsghdr) char response[8192]{};
    sockaddr_nl sender{};
    socklen_t sender_size = sizeof(sender);
    const auto bytes = ::recvfrom(descriptor.fd, response, sizeof(response), MSG_TRUNC,
                                  reinterpret_cast<sockaddr*>(&sender), &sender_size);
    if (bytes < 0) return failure(detail, "vcan kind netlink receive");
    if (bytes > static_cast<ssize_t>(sizeof(response)) || sender.nl_pid != 0) {
        detail = "invalid kernel vcan kind response";
        return false;
    }
    int remaining = static_cast<int>(bytes);
    for (auto* header = reinterpret_cast<nlmsghdr*>(response); NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
        if (header->nlmsg_seq != request.header.nlmsg_seq || header->nlmsg_type != RTM_NEWLINK ||
            header->nlmsg_len < NLMSG_LENGTH(sizeof(ifinfomsg))) continue;
        const auto* link = static_cast<const ifinfomsg*>(NLMSG_DATA(header));
        if (link->ifi_index != static_cast<int>(interface_index)) continue;
        int attributes_size = static_cast<int>(IFLA_PAYLOAD(header));
        for (auto* attribute = IFLA_RTA(link); RTA_OK(attribute, attributes_size);
             attribute = RTA_NEXT(attribute, attributes_size)) {
            if ((attribute->rta_type & NLA_TYPE_MASK) != IFLA_LINKINFO) continue;
            int nested_size = static_cast<int>(RTA_PAYLOAD(attribute));
            for (auto* nested = static_cast<rtattr*>(RTA_DATA(attribute)); RTA_OK(nested, nested_size);
                 nested = RTA_NEXT(nested, nested_size)) {
                if ((nested->rta_type & NLA_TYPE_MASK) != IFLA_INFO_KIND) continue;
                if (RTA_PAYLOAD(nested) == 5 && std::memcmp(RTA_DATA(nested), "vcan", 5) == 0) return true;
                detail = "vcan0 is not a kernel vcan interface; physical CAN is forbidden";
                return false;
            }
        }
    }
    detail = "kernel did not identify interface as kind vcan; refusing CAN access";
    return false;
}

}  // namespace cabinflow::agent::vehicle_can
