#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>

#include <revolution/so.h>

int main()
{
    int failures = 0;
    auto check = [&failures](bool passed, const char* message) {
        if (!passed)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };
    auto check_eq = [&failures]<typename A, typename B>(
        const A& actual,
        const B& expected,
        const char* message) -> const A&
    {
        if (!(actual == expected))
        {
            std::cerr << message
                      << " | actual: " << actual
                      << " | expected: " << expected << '\n';
            ++failures;
        }
        return actual;
    };

    auto check_neq = [&failures]<typename A, typename B>(
        const A& actual, const B& expected, const char* message) -> const A&
    {
        if (!(actual != expected))
        {
            std::cerr << message
                      << " | actual: " << actual
                      << " | expected: " << expected << '\n';
            ++failures;
        }
        return actual;
    };

    auto check_gt = [&failures]<typename A, typename B>(
        const A& actual, const B& expected, const char* message) -> const A&
    {
        if (!(actual > expected))
        {
            std::cerr << message
                      << " | actual: " << actual
                      << " | expected: " << expected << '\n';
            ++failures;
        }
        return actual;
    };

    auto check_gte = [&failures]<typename A, typename B>(
        const A& actual, const B& expected, const char* message) -> const A&
    {
        if (!(actual >= expected))
        {
            std::cerr << message
                      << " | actual: " << actual
                      << " | expected: " << expected << '\n';
            ++failures;
        }
        return actual;
    };

    auto check_lt = [&failures]<typename A, typename B>(
        const A& actual, const B& expected, const char* message) -> const A&
    {
        if (!(actual < expected))
        {
            std::cerr << message
                      << " | actual: " << actual
                      << " | expected: " << expected << '\n';
            ++failures;
        }
        return actual;
    };

    auto check_lte = [&failures]<typename A, typename B>(
        const A& actual, const B& expected, const char* message) -> const A&
    {
        if (!(actual <= expected))
        {
            std::cerr << message
                      << " | actual: " << actual
                      << " | expected: " << expected << '\n';
            ++failures;
        }
        return actual;
    };

    // -------------------------------

    // helper functions
    u16 a = SOHtoNs(0x1234);
    check_eq(((u8*)&a)[0], 0x12, "byteorder: SOHtoNs(0x1234) byte 0 in memory");
    check_eq(((u8*)&a)[1], 0x34, "byteorder: SOHtoNs(0x1234) byte 1 in memory");

    u32 b = SOHtoNl(0x12345678);
    check_eq(((u8*)&b)[0], 0x12, "byteorder: SOHtoNl(0x12345678) byte 0 in memory");
    check_eq(((u8*)&b)[1], 0x34, "byteorder: SOHtoNl(0x12345678) byte 1 in memory");
    check_eq(((u8*)&b)[2], 0x56, "byteorder: SOHtoNl(0x12345678) byte 2 in memory");
    check_eq(((u8*)&b)[3], 0x78, "byteorder: SOHtoNl(0x12345678) byte 3 in memory");

    check_eq(SONtoHs(a), 0x1234,
         "byteorder: SNtoH(SOHtoNs(0x1234))");

    check_eq(SONtoHl(b), 0x12345678,
         "byteorder: SNtoHl(SOHtoNl(0x12345678))");

    // lifecycle
    SOLibraryConfig validCfg = {.alloc = (SOAlloc)0xf0cacc1a, .free = (SOFree)0xbadc0ffee0ddf00d};
    for (int i=1; i<=2; i++)
    {
        std::cerr << "lifecycle: iteration " << i << '\n';
        check_eq(SOStartup(), SO_ENETRESET, "lifecycle: SOStartup before SOInit (TERMINATED)");
        check_eq(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_ENETRESET, "lifecycle: SOSocket2 before SOInit (TERMINATED)");
        check_eq(SOFinish(), SO_EALREADY, "lifecycle: SOFinish before SOInit (TERMINATED)");
        check_eq(SOGetHostID(), 0l, "lifecycle: SOGetHostID before SOInit (TERMINATED)");
        check_eq(SOInit(NULL), SO_EINVAL, "lifecycle: SOInit(NULL)");

        SOLibraryConfig invalidCfg = {.alloc = NULL, .free = NULL};
        check_eq(SOInit(&invalidCfg), SO_EINVAL, "lifecycle: SOInit with alloc and free both NULL");
        invalidCfg.alloc = (SOAlloc)0xdeadbeef;
        check_eq(SOInit(&invalidCfg), SO_EINVAL, "lifecycle: SOInit with free NULL");

        check_eq(SOInit(&validCfg), SO_SUCCESS, "lifecycle: SOInit with a valid config (TERMINATED -> READY)");
        check_eq(SOInit(&validCfg), SO_EALREADY, "lifecycle: second SOInit (already READY)");

        check_eq(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_EINVAL, "lifecycle: SOSocket2 after SOInit, before SOStartup (READY)");

        check_eq(SOStartup(), SO_SUCCESS, "lifecycle: SOStartup after SOInit (READY -> ACTIVE)");
        check_eq(SOStartup(), SO_EALREADY, "lifecycle: second SOStartup (already ACTIVE)");

        check_eq(SOFinish(), SO_EINPROGRESS, "lifecycle: SOFinish while ACTIVE (SOCleanup must come first)");
        check_eq(SOCleanup(), SO_SUCCESS, "lifecycle: SOCleanup while ACTIVE (ACTIVE -> READY)");
        check_eq(SOCleanup(), SO_EALREADY, "lifecycle: second SOCleanup (already READY)");
        check_eq(SOFinish(), SO_SUCCESS, "lifecycle: SOFinish after SOCleanup (READY -> TERMINATED)");
        check_eq(SOFinish(), SO_EALREADY, "lifecycle: second SOFinish (already TERMINATED)");
    }
    
    
    // socket table checks
    check_eq(SOInit(&validCfg), SO_SUCCESS, "table: SOInit before the socket table tests");
    check_eq(SOStartup(), SO_SUCCESS, "table: SOStartup before the socket table tests");
    int s1 = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "table: first SOSocket2 must return a descriptor >= 0");
    int s2 = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "table: second SOSocket2 must return a descriptor >= 0");
    check_neq(s1, s2, "table: two open sockets must get different descriptors");
    check_eq(SOClose(s1), SO_SUCCESS, "table: SOClose on the first socket");
    check_gte(SOFcntl(s2, SO_F_GETFL, 0), 0, "table: second socket must stay valid after closing the first (no compaction)");
    check_eq(SOFcntl(s1, SO_F_GETFL, 0), SO_EBADF, "table: SOFcntl on the closed first socket must fail");
    check_eq(SOClose(s1), SO_EBADF, "table: SOClose on an already closed descriptor");
    check_eq(SOClose(-1), SO_EBADF, "table: SOClose(-1)");
    check_eq(SOClose(99), SO_EBADF, "table: SOClose(99), out of range");
    check_eq(SOSocket2(SO_PF_INET6, SO_SOCK_DGRAM, 0), SO_EAFNOSUPPORT, "table: SOSocket2 with SO_PF_INET6");
    check_eq(SOClose(s2), SO_SUCCESS, "table: SOClose on the second socket");

    for (int i=0; i<32; i++)
    {
        auto chk = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "table: SOSocket2 while filling every slot");
        if (chk < 0)
            std::cerr << "table: could not fill the table, failed at socket " << i << '\n';
    }

    int invalid_s = 21; // in these tests it won't be invalid
    check_eq(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), SO_EMFILE, "table: SOSocket2 with all 32 slots in use");
    check_eq(SOClose(invalid_s), SO_SUCCESS, "table: SOClose on slot 21 to free one slot");
    check_eq(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), invalid_s, "table: SOSocket2 must reuse the freed slot 21");
    check_eq(SOClose(invalid_s), SO_SUCCESS, "table: SOClose on slot 21, left free for the invalid-descriptor tests");
    // now invalid_s is invalid

    // bind checks
    int test_s = 0;
    SOSockAddrIn invalidLenAddr = {
        .len = 100,
        .family = SO_PF_INET,
        .port = SOHtoNs(6767),
        .addr = { .addr = SOHtoNl(0x7f000001) } // 127.0.0.1
    };
    check_eq(SOBind(test_s, &invalidLenAddr), SO_EINVAL, "bind: SOBind with len=100 (must equal sizeof(SOSockAddr))");
    check_eq(SOBind(test_s, NULL), SO_EINVAL, "bind: SOBind with a NULL address");
    SOSockAddrIn addr = {
        .len = sizeof(SOSockAddrIn),
        .family = SO_PF_INET,
        .port = SOHtoNs(6767),
        .addr = { .addr = SOHtoNl(0x7f000001) } // 127.0.0.1
    };
    check_eq(SOBind(test_s, &addr), SO_SUCCESS, "bind: SOBind on 127.0.0.1:6767");
    check_eq(SOBind(invalid_s, &addr), SO_EBADF, "bind: SOBind on a closed descriptor");
    check_eq(SOBind(1, &addr), SO_EADDRINUSE, "bind: second socket binding the same 127.0.0.1:6767");

    // SOFcntl 

    int base_flags = check_eq(SOFcntl(test_s, SO_F_GETFL, 0), 0, "fcntl: SO_F_GETFL on a fresh socket (blocking)");
    SOFcntl(test_s, SO_F_SETFL, base_flags | SO_O_NONBLOCK);
    int nonblock_flags = check_eq(SOFcntl(test_s, SO_F_GETFL, 0), SO_O_NONBLOCK, "fcntl: SO_F_GETFL after setting SO_O_NONBLOCK");
    SOFcntl(test_s, SO_F_SETFL, nonblock_flags & (~SO_O_NONBLOCK));
    check_eq(SOFcntl(test_s, SO_F_GETFL, 0), 0, "fcntl: SO_F_GETFL after clearing SO_O_NONBLOCK (GameSpy SetSockBlocking)");
    check_eq(SOFcntl(test_s, 67, 0), SO_EINVAL, "fcntl: unknown command 67");
    check_eq(SOFcntl(invalid_s, SO_F_GETFL, 0), SO_EBADF, "fcntl: SO_F_GETFL on a closed descriptor");

    // Final cleanup
    check_eq(SOCleanup(), SO_SUCCESS, "cleanup: SOCleanup with sockets still open (ACTIVE -> READY)");
    check_eq(SOClose(invalid_s), SO_EINVAL, "cleanup: SOClose on a free slot after SOCleanup (READY)");
    check_eq(SOClose(test_s), SO_EINVAL, "cleanup: SOClose on a socket already closed by SOCleanup (READY)");
    check_eq(SOStartup(), SO_SUCCESS, "cleanup: SOStartup after SOCleanup (READY -> ACTIVE)");
    test_s = check_eq(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "cleanup: first SOSocket2 after restart must get slot 0");
    check_eq(SOClose(test_s), SO_SUCCESS, "cleanup: SOClose on the restarted socket");
    check_eq(SOCleanup(), SO_SUCCESS, "cleanup: SOCleanup while ACTIVE (ACTIVE -> READY)");

    // UDP
    check_eq(SOStartup(), SO_SUCCESS, "udp: SOStartup before the UDP tests");
    auto loopback = [](u16 port) {
        SOSockAddrIn a = {
            .len = sizeof(SOSockAddrIn),
            .family = SO_PF_INET,
            .port = SOHtoNs(port),
            .addr = { .addr = SOHtoNl(0x7f000001) },
        };
        return a;
    };
    SOSockAddrIn rx_addr = loopback(6768);
    SOSockAddrIn tx_addr = loopback(6769);

    int rx = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "udp: SOSocket2 for the receiver");
    int tx = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "udp: SOSocket2 for the sender");
    check_eq(SOBind(rx, &rx_addr), SO_SUCCESS, "udp: SOBind receiver on 127.0.0.1:6768");
    check_eq(SOBind(tx, &tx_addr), SO_SUCCESS, "udp: SOBind sender on 127.0.0.1:6769");
    // Before any recv: a broken send must not hang the suite
    check_eq(SOFcntl(rx, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "udp: make the receiver non-blocking");

    u8 buf[64];
    SOSockAddrIn from = { .len = sizeof(SOSockAddrIn) };
    check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, &from), SO_EAGAIN, "udp: SORecvFrom on an empty non-blocking socket");

    const u8 payload[] = { 0x00, 0x01, 0x7f, 0x80, 0xff, 'M', 'S', 'C' };
    check_eq(SOSendTo(tx, payload, sizeof(payload), 0, &rx_addr), (int)sizeof(payload), "udp: SOSendTo returns the bytes sent");

    from = { .len = sizeof(SOSockAddrIn) };
    int got = check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, &from), (int)sizeof(payload), "udp: SORecvFrom returns the datagram size");
    check(got == (int)sizeof(payload) && std::memcmp(buf, payload, sizeof(payload)) == 0, "udp: received bytes differ from the payload");
    check_eq(from.len, sizeof(SOSockAddrIn), "udp: SORecvFrom sender len");
    check_eq(from.family, SO_PF_INET, "udp: SORecvFrom sender family");
    check_eq(from.port, SOHtoNs(6769), "udp: SORecvFrom sender port (network order)");
    check_eq(from.addr.addr, SOHtoNl(0x7f000001), "udp: SORecvFrom sender address (network order)");
    check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, &from), SO_EAGAIN, "udp: queue must be empty after one recv");

    // A zero-length datagram is a real packet: 0, not SO_EAGAIN
    check_eq(SOSendTo(tx, NULL, 0, 0, &rx_addr), 0, "udp: SOSendTo of an empty datagram");
    check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, &from), 0, "udp: SORecvFrom of an empty datagram");

    // sockFrom == NULL is the SORecv path in the SDK: data, no sender address
    check_eq(SOSendTo(tx, payload, sizeof(payload), 0, &rx_addr), (int)sizeof(payload), "udp: SOSendTo before the NULL-sockFrom recv");
    check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, NULL), (int)sizeof(payload), "udp: SORecvFrom with sockFrom NULL (SORecv path)");

    // Argument validation, same order as SendTo/RecvFrom in soBasic.c
    SOSockAddrIn bad_len = rx_addr;
    bad_len.len = 100;
    check_eq(SOSendTo(tx, payload, sizeof(payload), 1, &rx_addr), SO_EINVAL, "udp: SOSendTo with flags != 0");
    check_eq(SOSendTo(tx, payload, sizeof(payload), 0, &bad_len), SO_EINVAL, "udp: SOSendTo with len=100 address");
    check_eq(SOSendTo(tx, payload, -1, 0, &rx_addr), SO_EINVAL, "udp: SOSendTo with negative len");
    check_eq(SOSendTo(tx, NULL, 4, 0, &rx_addr), SO_EINVAL, "udp: SOSendTo with NULL buf and len > 0");
    check_eq(SOSendTo(invalid_s, payload, sizeof(payload), 0, &rx_addr), SO_EBADF, "udp: SOSendTo on a closed descriptor");
    check_eq(SORecvFrom(rx, buf, sizeof(buf), 1, &from), SO_EINVAL, "udp: SORecvFrom with flags != 0");
    check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, &bad_len), SO_EINVAL, "udp: SORecvFrom with len=100 address");
    check_eq(SORecvFrom(rx, NULL, 4, 0, &from), SO_EINVAL, "udp: SORecvFrom with NULL buf and len > 0");
    check_eq(SORecvFrom(invalid_s, buf, sizeof(buf), 0, &from), SO_EBADF, "udp: SORecvFrom on a closed descriptor");

    // SOGetSockName
    SOSockAddrIn name = { .len = sizeof(SOSockAddrIn) };
    check_eq(SOGetSockName(rx, &name), SO_SUCCESS, "getsockname: SOGetSockName on the bound receiver");
    check_eq(name.len, sizeof(SOSockAddrIn), "getsockname: len");
    check_eq(name.family, SO_PF_INET, "getsockname: family");
    check_eq(name.port, SOHtoNs(6768), "getsockname: port (network order)");
    check_eq(name.addr.addr, SOHtoNl(0x7f000001), "getsockname: address (network order)");

    check_eq(SOGetSockName(rx, NULL), SO_EINVAL, "getsockname: NULL address (unlike SORecvFrom, not optional)");
    name = { .len = 100 };
    check_eq(SOGetSockName(rx, &name), SO_EINVAL, "getsockname: len=100 address");
    name = { .len = sizeof(SOSockAddrIn) };
    check_eq(SOGetSockName(invalid_s, &name), SO_EBADF, "getsockname: closed descriptor");

    // Port 0 lets the kernel pick a free port; SOGetSockName is how you learn it
    int eph = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "getsockname: SOSocket2 for the ephemeral-port socket");
    SOSockAddrIn eph_addr = loopback(0);
    check_eq(SOBind(eph, &eph_addr), SO_SUCCESS, "getsockname: SOBind on 127.0.0.1:0");
    eph_addr = { .len = sizeof(SOSockAddrIn) };
    check_eq(SOGetSockName(eph, &eph_addr), SO_SUCCESS, "getsockname: SOGetSockName after binding to port 0");
    check_neq(eph_addr.port, 0, "getsockname: the kernel must have assigned a real port");
    check_eq(SOFcntl(eph, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "getsockname: make the ephemeral socket non-blocking");
    check_eq(SOSendTo(tx, payload, sizeof(payload), 0, &eph_addr), (int)sizeof(payload), "getsockname: SOSendTo to the address SOGetSockName returned");
    check_eq(SORecvFrom(eph, buf, sizeof(buf), 0, NULL), (int)sizeof(payload), "getsockname: the datagram must arrive on the ephemeral port");
    check_eq(SOClose(eph), SO_SUCCESS, "getsockname: SOClose on the ephemeral-port socket");

    // SOPoll. At this point rx is non-blocking and its queue is empty.
    SOPollFD pfd[3];
    check_eq(SOPoll(NULL, 1, 0), SO_EINVAL, "poll: NULL fds");
    check_eq(SOPoll(pfd, 0, 0), 0, "poll: nfds == 0");
    // Positive timeouts need OSTicksToMilliseconds (bus clock): rejected for now
    pfd[0] = { .fd = rx, .events = SO_POLLRDNORM, .revents = 0 };
    check_eq(SOPoll(pfd, 1, 1), SO_EINVAL, "poll: positive timeout (unsupported for now)");

    // revents is pre-filled with garbage: SOPoll must always overwrite it
    pfd[0] = { .fd = rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    check_eq(SOPoll(pfd, 1, 0), 0, "poll: empty socket, timeout 0, nothing ready");
    check_eq(pfd[0].revents, 0, "poll: empty socket revents");

    pfd[0] = { .fd = tx, .events = SO_POLLWRNORM, .revents = 0xffff };
    check_eq(SOPoll(pfd, 1, 0), 1, "poll: a UDP socket is always writable");
    check_eq(pfd[0].revents, SO_POLLWRNORM, "poll: writable revents (WRNORM is 0x8, POLLOUT would be 0x4)");

    // SO_POLLIN is RDNORM|RDBAND: only RDNORM must come back for normal data
    check_eq(SOSendTo(tx, payload, sizeof(payload), 0, &rx_addr), (int)sizeof(payload), "poll: SOSendTo to make the receiver readable");
    pfd[0] = { .fd = rx, .events = SO_POLLIN, .revents = 0xffff };
    check_eq(SOPoll(pfd, 1, 0), 1, "poll: socket with a pending datagram");
    check_eq(pfd[0].revents, SO_POLLRDNORM, "poll: readable revents (RDNORM only, no RDBAND)");
    // Data is already there, so the infinite timeout must return immediately
    pfd[0] = { .fd = rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    check_eq(SOPoll(pfd, 1, SO_INFTIM), 1, "poll: SO_INFTIM with data ready returns at once");
    check_eq(pfd[0].revents, SO_POLLRDNORM, "poll: SO_INFTIM revents");

    // Invalid descriptors don't fail the call: they get NVAL and count as ready
    pfd[0] = { .fd = rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[1] = { .fd = invalid_s, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[2] = { .fd = tx, .events = SO_POLLWRNORM, .revents = 0xffff };
    check_eq(SOPoll(pfd, 3, 0), 3, "poll: readable + closed + writable");
    check_eq(pfd[0].revents, SO_POLLRDNORM, "poll: mixed array, readable socket revents");
    check_eq(pfd[1].revents, SO_POLLNVAL, "poll: mixed array, closed descriptor revents");
    check_eq(pfd[2].revents, SO_POLLWRNORM, "poll: mixed array, writable socket revents");

    check_eq(SORecvFrom(rx, buf, sizeof(buf), 0, NULL), (int)sizeof(payload), "poll: drain the receiver");

    // Empty + invalid: only the NVAL entry counts
    pfd[0] = { .fd = rx, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[1] = { .fd = -1, .events = SO_POLLRDNORM, .revents = 0xffff };
    pfd[2] = { .fd = 99, .events = SO_POLLRDNORM, .revents = 0xffff };
    check_eq(SOPoll(pfd, 3, 0), 2, "poll: empty + fd -1 + fd 99 out of range");
    check_eq(pfd[0].revents, 0, "poll: empty socket next to invalid ones");
    check_eq(pfd[1].revents, SO_POLLNVAL, "poll: fd -1 revents");
    check_eq(pfd[2].revents, SO_POLLNVAL, "poll: fd 99 revents");


    // SOGetHostID. The detected address depends on the machine running the
    // suite (it is loopback in a sandbox without network), so the default
    // case only checks properties that hold everywhere.
    u32 host = (u32)SOGetHostID();
    check_neq(host, 0u, "hostid: SOGetHostID while ACTIVE must not be 0");
    check_eq((u32)SOGetHostID(), host, "hostid: SOGetHostID must be stable while ACTIVE");
    if (sizeof(long) > sizeof(u32))
        check_eq((unsigned long)SOGetHostID() >> 32, 0ul, "hostid: no sign extension in the upper 32 bits of long");
    std::cerr << "hostid: detected " << (host & 0xff) << '.' << ((host >> 8) & 0xff) << '.'
              << ((host >> 16) & 0xff) << '.' << (host >> 24) << '\n';

    // Whatever was detected must belong to this host: bind to it and loop a datagram
    SOSockAddrIn self = { .len = sizeof(SOSockAddrIn), .family = SO_PF_INET, .port = 0, .addr = { .addr = host } };
    int self_s = check_gte(SOSocket2(SO_PF_INET, SO_SOCK_DGRAM, 0), 0, "hostid: SOSocket2 for the self socket");
    check_eq(SOBind(self_s, &self), SO_SUCCESS, "hostid: SOBind on the SOGetHostID address (must be local)");
    check_eq(SOGetSockName(self_s, &self), SO_SUCCESS, "hostid: SOGetSockName on the self socket");
    check_eq(SOFcntl(self_s, SO_F_SETFL, SO_O_NONBLOCK), SO_SUCCESS, "hostid: make the self socket non-blocking");
    check_eq(SOSendTo(self_s, payload, sizeof(payload), 0, &self), (int)sizeof(payload), "hostid: SOSendTo to our own SOGetHostID address");
    check_eq(SORecvFrom(self_s, buf, sizeof(buf), 0, NULL), (int)sizeof(payload), "hostid: the datagram sent to ourselves must arrive");
    check_eq(SOClose(self_s), SO_SUCCESS, "hostid: SOClose on the self socket");

    // The address is captured by SOStartup, so the override needs a restart.
    // The caller may already have set the variable: keep it to restore later.
    const char* env_iface = std::getenv("MSCHARGED_NET_IFACE");
    std::string saved_iface = env_iface != nullptr ? env_iface : "";
    setenv("MSCHARGED_NET_IFACE", "lo", 1);
    check_eq(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before forcing the interface");
    check_eq(SOGetHostID(), 0l, "hostid: SOGetHostID after SOCleanup (READY)");
    check_eq(SOStartup(), SO_SUCCESS, "hostid: SOStartup with MSCHARGED_NET_IFACE=lo");
    check_eq((u32)SOGetHostID(), SOHtoNl(0x7f000001), "hostid: MSCHARGED_NET_IFACE=lo must give 127.0.0.1");

    setenv("MSCHARGED_NET_IFACE", "msc-does-not-exist", 1);
    check_eq(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before forcing a missing interface");
    check_eq(SOStartup(), SO_SUCCESS, "hostid: SOStartup with a missing interface");
    check_eq((u32)SOGetHostID(), SOHtoNl(0x7f000001), "hostid: a missing interface must fall back to 127.0.0.1");

    if (env_iface != nullptr)
        setenv("MSCHARGED_NET_IFACE", saved_iface.c_str(), 1);
    else
        unsetenv("MSCHARGED_NET_IFACE");
    check_eq(SOCleanup(), SO_SUCCESS, "hostid: SOCleanup before restoring the environment");
    check_eq(SOStartup(), SO_SUCCESS, "hostid: SOStartup with the original environment");
    check_eq((u32)SOGetHostID(), host, "hostid: with the original environment the first detected address comes back");

    check_eq(SOCleanup(), SO_SUCCESS, "post tests: SOCleanup while ACTIVE (ACTIVE -> READY)");
    check_eq(SOGetHostID(), 0l, "post tests: SOGetHostID after SOCleanup (READY)");
    check_eq(SOPoll(pfd, 1, 0), SO_EINVAL, "post tests: SOPoll after SOCleanup (READY)");
    check_eq(SOGetSockName(rx, &name), SO_EINVAL, "post tests: SOGetSockName after SOCleanup (READY)");
    check_eq(SOFinish(), SO_SUCCESS, "post tests: SOFinish after SOCleanup (READY -> TERMINATED)");
    check_eq(SOGetHostID(), 0l, "post tests: SOGetHostID after SOFinish (TERMINATED)");
    
    return failures == 0 ? 0 : 1;
}
