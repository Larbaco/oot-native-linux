/* Co-op step 1b (docs/coop-step1b.md): nxrt's hooks route to the coop module. Every call goes through the generated adapter of the forced entry, as the game's
   calls do. Run once per mode, each in its own process (DM_COOP is read once):
     off   no DM_COOP: ldn stays cut, sockets answer -1 / ENETDOWN, pctl answers "not allowed", the account Uid is the constant;
     host  DM_COOP=host:0 (set here): the hooks host a network; a station made directly with the module scans, connects and exchanges datagrams with it;
     join  a host made directly with the module; DM_COOP=join:127.0.0.1:<its port> and DM_COOP_TCP=1 (set here): the hooks scan, connect and exchange
           datagrams, over the control link only. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "hle_registry.h"
#include "nxrt/runtime.h"
#include "coop.h"

NXRT_EARLY_RESERVE()

coop_node *dm_coop(void);   /* hle_offline.c */
static int g_fail, g_checks;
#define T(c) do { g_checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
#define T_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); g_checks++; \
    if (a_ != b_) { printf("FAIL %s:%d: %s == %s (%lld vs %lld, %#llx vs %#llx)\n", __FILE__, __LINE__, #a, #b, a_, b_, a_, b_); g_fail = 1; } } while (0)

/* A forced entry's adapter with x0.. = the arguments; returns x0 */
static uint64_t hc(const char *name, int n, ...) {
    const nxrt_hle_entry *e = nxrt_hle_find(name, NXRT_HLE_FORCED);
    if (!e || e->ported != 1) { printf("FAIL: %s is not a ported forced entry\n", name); g_fail = 1; return ~0ull; }
    NxwGuestCpu c; memset(&c, 0, sizeof c);
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) c.x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    e->adapter(&c);
    return c.x[0];
}
#define P(p) ((uint64_t)(uintptr_t)(p))
#define I(v) ((uint64_t)(int64_t)(v))
static int r32(uint64_t x) { return (int)(int32_t)x; }

#define LDN_INIT "_ZN2nn3ldn10InitializeEv"
#define LDN_FINI "_ZN2nn3ldn8FinalizeEv"
#define LDN_STATE "_ZN2nn3ldn8GetStateEv"
#define LDN_OPEN_AP "_ZN2nn3ldn15OpenAccessPointEv"
#define LDN_CLOSE_AP "_ZN2nn3ldn16CloseAccessPointEv"
#define LDN_CREATE "_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE"
#define LDN_DESTROY "_ZN2nn3ldn14DestroyNetworkEv"
#define LDN_ADV "_ZN2nn3ldn16SetAdvertiseDataEPKvm"
#define LDN_OPEN_STA "_ZN2nn3ldn11OpenStationEv"
#define LDN_CLOSE_STA "_ZN2nn3ldn12CloseStationEv"
#define LDN_SCAN "_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi"
#define LDN_CONNECT "_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE"
#define LDN_DISC "_ZN2nn3ldn10DisconnectEv"
#define LDN_DISC_REASON "_ZN2nn3ldn19GetDisconnectReasonEv"
#define LDN_INFO "_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE"
#define LDN_IP "_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE"
#define SK_SOCKET "_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE"
#define SK_SOCKET_I "_ZN2nn6socket6SocketEiii"
#define SK_SOCKET_C "nnsocketSocket"
#define SK_BIND "_ZN2nn6socket4BindEiPKNS0_8SockAddrEj"
#define SK_BIND_P "_ZN2nn6socket4BindEiPK8sockaddrj"
#define SK_CLOSE "_ZN2nn6socket5CloseEi"
#define SK_FCNTL "_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz"
#define SK_FCNTL_I "_ZN2nn6socket5FcntlEiiz"
#define SK_FCNTL_C "nnsocketFcntl"
#define SK_GETOPT "_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj"
#define SK_SETOPT_I "_ZN2nn6socket10SetSockOptEiiiPKvj"
#define SK_NAME_P "_ZN2nn6socket11GetSockNameEiP8sockaddrPj"
#define SK_RECVFROM "_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj"
#define SK_RECVFROM_P "_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj"
#define SK_SENDTO "_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj"
#define SK_SENDTO_P "_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj"
#define SK_POLL "_ZN2nn6socket4PollEPNS0_6PollFdEmi"
#define SK_POLL_P "_ZN2nn6socket4PollEP6pollfdmi"
#define SK_SELECT "_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE"
#define SK_LASTERROR "_ZN2nn6socket12GetLastErrorEv"
#define SK_LASTERRNO_I "_ZN2nn6socket12GetLastErrnoEv"
#define SK_LASTERRNO_C "nnsocketGetLastErrno"
#define PCTL_CHECK "_ZN2nn4pctl32CheckFreeCommunicationPermissionEb"
#define PCTL_TRY "_ZN2nn4pctl25TryBeginFreeCommunicationEb"
#define PCTL_IS "_ZN2nn4pctl28IsFreeCommunicationAvailableEv"
#define ACC_LIST "_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi"
enum { ENETDOWN_ = 50, EAGAIN_ = 35, MSG_DONTWAIT_ = 0x80, PORT = 49530, DGRAM_NB = 0x20000002 };

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
/* The game's structs as step 0 measured them (research/06 §3.1), as coop_test.c builds them */
static void mk_netcfg(uint8_t c[0x20]) { memset(c, 0, 0x20); put64(c, ~0ull); c[0x12] = 4; put16(c + 0x14, 301); }
static void mk_sec(uint8_t s[0x44]) { memset(s, 0, 0x44); put16(s, 1); put16(s + 2, 0x40); for (int i = 0; i < 0x40; i++) s[4 + i] = (uint8_t)(i * 5 + 1); }   /* a test passphrase */
static void mk_user(uint8_t u[0x30], const char *name) { memset(u, 0, 0x30); strncpy((char *)u, name, 32); }
static void mk_filter(uint8_t f[0x60]) { memset(f, 0, 0x60); put64(f, ~0ull); put32(f + 0x20, 2); put32(f + 0x5C, 0x21); }
static void mk_sa(uint8_t sa[16], uint32_t ip, uint16_t port) {   /* sin_len 0, as the game binds */
    memset(sa, 0, 16); sa[1] = 2; sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16); sa[6] = (uint8_t)(ip >> 8); sa[7] = (uint8_t)ip;
}
static uint32_t sa_ip(const uint8_t sa[16]) { return (uint32_t)sa[4] << 24 | (uint32_t)sa[5] << 16 | (uint32_t)sa[6] << 8 | sa[7]; }
static uint16_t sa_port(const uint8_t sa[16]) { return (uint16_t)(sa[2] << 8 | sa[3]); }
#define NI_COUNT(i) ((i)[0x67])
typedef struct { int32_t fd; int16_t events, revents; } pollfd_;
static const uint8_t k_uid_host[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

/* The host game's frame loop: GetNetworkInfo every 5 ms (through the hook in host mode, the module in join mode) */
static volatile LONG g_poll_run; static coop_node *g_poll_node;
static DWORD WINAPI poller(LPVOID p) {
    (void)p; uint8_t ni[0x480];
    while (g_poll_run) { if (g_poll_node) coop_ldn_network_info(g_poll_node, ni); else hc(LDN_INFO, 1, P(ni)); Sleep(5); }
    return 0;
}
static HANDLE poll_start(coop_node *n) { g_poll_node = n; g_poll_run = 1; return CreateThread(NULL, 0, poller, NULL, 0, NULL); }
static void poll_stop(HANDLE h) { g_poll_run = 0; WaitForSingleObject(h, INFINITE); CloseHandle(h); }

static void check_uid(int from_env) {
    int n = -1; uint8_t u[16] = {0};
    T_EQ(r32(hc(ACC_LIST, 3, P(&n), P(u), I(1))), 0);
    T_EQ(n, 1);
    if (from_env) T(!memcmp(u, k_uid_host, 16));
    else { T_EQ(get64(u), 0x78626c0000000001ll); T_EQ(get64(u + 8), 0x53574e4154495645ll); }
}

static void mode_off(void) {
    _putenv("DM_COOP="); _putenv("DM_COOP_TCP=");
    T(dm_coop() == NULL);
    T_EQ(hc(PCTL_CHECK, 1, I(1)) & 0xff, 0); T_EQ(hc(PCTL_TRY, 1, I(1)) & 0xff, 0); T_EQ(hc(PCTL_IS, 0) & 0xff, 0);
    check_uid(0);
    T_EQ((uint32_t)hc(LDN_INIT, 0), 0x2A2);
    T_EQ(r32(hc(LDN_STATE, 0)), 0);
    uint8_t info[0x480], f[0x60]; int cnt = 7; mk_filter(f);
    T_EQ((uint32_t)hc(LDN_SCAN, 5, P(info), P(&cnt), I(24), P(f), I(0)), 0x2A2); T_EQ(cnt, 0);
    uint32_t ip = 1, mask = 1;
    T_EQ((uint32_t)hc(LDN_IP, 2, P(&ip), P(&mask)), 0x2A2); T_EQ(ip, 0); T_EQ(mask, 0);
    T_EQ((uint32_t)hc(LDN_INFO, 1, P(info)), 0x2A2);
    T_EQ((uint32_t)hc(LDN_OPEN_AP, 0), 0x2A2);
    /* every socket form: -1, ENETDOWN */
    T_EQ(r32(hc(SK_SOCKET, 3, I(2), I(DGRAM_NB), I(0))), -1); T_EQ(r32(hc(SK_LASTERROR, 0)), ENETDOWN_);
    T_EQ(r32(hc(SK_SOCKET_I, 3, I(2), I(DGRAM_NB), I(0))), -1); T_EQ(r32(hc(SK_LASTERRNO_I, 0)), ENETDOWN_);
    T_EQ(r32(hc(SK_SOCKET_C, 3, I(2), I(DGRAM_NB), I(0))), -1); T_EQ(r32(hc(SK_LASTERRNO_C, 0)), ENETDOWN_);
    uint8_t sa[16]; mk_sa(sa, 0, PORT); char buf[64]; unsigned sl = 16;
    T_EQ(r32(hc(SK_BIND, 3, I(COOP_FD_BASE), P(sa), I(16))), -1);
    T_EQ(r32(hc(SK_BIND_P, 3, I(COOP_FD_BASE), P(sa), I(16))), -1);
    T_EQ((int64_t)hc(SK_RECVFROM, 6, I(COOP_FD_BASE), P(buf), I(64), I(MSG_DONTWAIT_), P(sa), P(&sl)), -1);
    T_EQ((int64_t)hc(SK_RECVFROM_P, 6, I(COOP_FD_BASE), P(buf), I(64), I(MSG_DONTWAIT_), P(sa), P(&sl)), -1);
    T_EQ((int64_t)hc(SK_SENDTO, 6, I(COOP_FD_BASE), P("x"), I(2), I(0), P(sa), I(16)), -1);
    T_EQ((int64_t)hc(SK_SENDTO_P, 6, I(COOP_FD_BASE), P("x"), I(2), I(0), P(sa), I(16)), -1);
    T_EQ(r32(hc(SK_FCNTL, 3, I(COOP_FD_BASE), I(3), I(0))), -1); T_EQ(r32(hc(SK_FCNTL_I, 3, I(COOP_FD_BASE), I(3), I(0))), -1);
    T_EQ(r32(hc(SK_CLOSE, 1, I(COOP_FD_BASE))), -1); T_EQ(r32(hc(SK_LASTERROR, 0)), ENETDOWN_);
    /* Poll / Select: a descriptor fails at once; nothing to wait on waits out the timeout and returns 0 */
    pollfd_ pf = {COOP_FD_BASE, 1, 0};
    T_EQ(r32(hc(SK_POLL, 3, P(&pf), I(1), I(0))), -1);
    pf.fd = -1; T_EQ(r32(hc(SK_POLL_P, 3, P(&pf), I(1), I(0))), 0);
    uint64_t rd[16] = {0}; int64_t tv[2] = {0, 0};
    T_EQ(r32(hc(SK_SELECT, 5, I(1024), P(rd), I(0), I(0), P(tv))), 0);
    rd[COOP_FD_BASE >> 6] = 1ull << (COOP_FD_BASE & 63);
    T_EQ(r32(hc(SK_SELECT, 5, I(1024), P(rd), I(0), I(0), P(tv))), -1);
}

static void mode_host(void) {
    char uid[33]; for (int i = 0; i < 16; i++) sprintf(uid + 2 * i, "%02x", k_uid_host[i < 8 ? 7 - i : 23 - i]);   /* word 0 first, as DM_UID reads */
    _putenv("DM_COOP=host:0"); _putenv_s("DM_UID", uid); _putenv("DM_COOP_TCP=");
    coop_node *node = dm_coop();
    T(node != NULL);
    if (!node) return;
    T_EQ(hc(PCTL_CHECK, 1, I(1)) & 0xff, 1); T_EQ(hc(PCTL_TRY, 1, I(1)) & 0xff, 1); T_EQ(hc(PCTL_IS, 0) & 0xff, 1);
    check_uid(1);
    /* before ldn::Initialize sockets stay offline */
    T_EQ(r32(hc(SK_SOCKET, 3, I(2), I(DGRAM_NB), I(0))), -1); T_EQ(r32(hc(SK_LASTERROR, 0)), ENETDOWN_);
    T_EQ((uint32_t)hc(LDN_INIT, 0), 0);
    T_EQ(r32(hc(LDN_STATE, 0)), COOP_LS_INITIALIZED);
    T_EQ(coop_port(node), 0);   /* step 1c: nothing listens until the game hosts */
    uint16_t port = 0;
    /* the game's order: Socket + Bind, a first RecvFrom, OpenAccessPoint, CreateNetwork, SetAdvertiseData */
    int fd = r32(hc(SK_SOCKET, 3, I(2), I(DGRAM_NB), I(0)));
    T_EQ(fd, COOP_FD_BASE);
    T_EQ(r32(hc(SK_SOCKET, 3, I(2), I(1), I(0))), -1); T_EQ(r32(hc(SK_LASTERROR, 0)), ENETDOWN_);   /* a stream socket stays offline */
    uint8_t sa[16], from[16]; char buf[64]; unsigned sl;
    mk_sa(sa, 0, PORT);
    T_EQ(r32(hc(SK_BIND, 3, I(fd), P(sa), I(16))), 0);
    sl = 16; T_EQ((int64_t)hc(SK_RECVFROM, 6, I(fd), P(buf), I(64), I(MSG_DONTWAIT_), P(from), P(&sl)), -1);
    T_EQ(r32(hc(SK_LASTERROR, 0)), EAGAIN_); T_EQ(r32(hc(SK_LASTERRNO_I, 0)), EAGAIN_); T_EQ(r32(hc(SK_LASTERRNO_C, 0)), EAGAIN_);
    /* Fcntl's third argument (x2) reaches the module, in all three forms */
    T_EQ(r32(hc(SK_FCNTL, 3, I(fd), I(3), I(0))), 6);                 /* F_GETFL: O_RDWR | O_NONBLOCK */
    T_EQ(r32(hc(SK_FCNTL_I, 3, I(fd), I(4), I(0))), 0);               /* F_SETFL 0 */
    T_EQ(r32(hc(SK_FCNTL_C, 3, I(fd), I(3), I(0))), 2);
    T_EQ(r32(hc(SK_FCNTL, 3, I(fd), I(4), I(4))), 0);
    T_EQ(r32(hc(SK_FCNTL_I, 3, I(fd), I(3), I(0))), 6);
    int v = 1, ty = 0; unsigned vl = 4;
    T_EQ(r32(hc(SK_SETOPT_I, 5, I(fd), I(0xffff), I(0x20), P(&v), I(4))), 0);   /* SO_BROADCAST */
    T_EQ(r32(hc(SK_GETOPT, 5, I(fd), I(0xffff), I(0x1008), P(&ty), P(&vl))), 0); T_EQ(ty, 2);   /* SO_TYPE: SOCK_DGRAM */
    sl = 16; T_EQ(r32(hc(SK_NAME_P, 3, I(fd), P(from), P(&sl))), 0); T_EQ(sa_port(from), PORT);
    /* a descriptor that is not virtual keeps the offline answer */
    T_EQ(r32(hc(SK_BIND, 3, I(7), P(sa), I(16))), -1); T_EQ(r32(hc(SK_LASTERROR, 0)), ENETDOWN_);
    pollfd_ pf2[2] = {{fd, 1, 0}, {7, 1, 0}};
    T_EQ(r32(hc(SK_POLL, 3, P(pf2), I(2), I(0))), -1); T_EQ(r32(hc(SK_LASTERROR, 0)), ENETDOWN_);
    uint8_t cfg[0x20], sec[0x44], uh[0x30], us[0x30], adv[100], info[0x480], f[0x60];
    mk_netcfg(cfg); mk_sec(sec); mk_user(uh, "Host"); mk_user(us, "Sta"); mk_filter(f);
    for (int i = 0; i < 100; i++) adv[i] = (uint8_t)(i ^ 0x5a);
    T_EQ((uint32_t)hc(LDN_OPEN_AP, 0), 0);
    T_EQ((uint32_t)hc(LDN_CREATE, 3, P(cfg), P(sec), P(uh)), 0);
    port = coop_port(node); T(port != 0);
    T_EQ((uint32_t)hc(LDN_ADV, 2, P(adv), I(sizeof adv)), 0);
    T_EQ(r32(hc(LDN_STATE, 0)), COOP_LS_AP_CREATED);
    uint32_t hip = 0, mask = 0;
    T_EQ((uint32_t)hc(LDN_IP, 2, P(&hip), P(&mask)), 0);
    T_EQ(hip >> 16, 0xA9FE); T_EQ(hip & 0xFF, 1); T_EQ(mask, 0xFFFFFF00u);
    T_EQ((uint32_t)hc(LDN_INFO, 1, P(info)), 0); T_EQ(NI_COUNT(info), 1);
    HANDLE th = poll_start(NULL);
    /* a station made directly with the module */
    coop_cfg sc; char err[160], join[48];
    snprintf(join, sizeof join, "join:127.0.0.1:%u", port);
    T_EQ(coop_cfg_parse(&sc, join, "0000000000000002000000000000000b", err, sizeof err), 1);
    coop_node *S = coop_create(&sc);
    T(S != NULL);
    if (!S) { poll_stop(th); return; }
    T_EQ(coop_ldn_initialize(S), 0);
    int sfd = coop_vnet_socket(S, 2, DGRAM_NB, 0); uint8_t ssa[16]; mk_sa(ssa, 0, PORT);
    T_EQ(coop_vnet_bind(S, sfd, ssa, 16), 0);
    T_EQ(coop_ldn_open_station(S), 0);
    uint8_t *scan = calloc(24, 0x480); int cnt = 0;
    for (int i = 0; i < 50 && cnt < 1; i++) { cnt = 0; coop_ldn_scan(S, scan, &cnt, 24, f, 0); if (cnt < 1) Sleep(100); }
    T_EQ(cnt, 1);
    T(cnt >= 1 && !memcmp(scan + 0x26C, adv, sizeof adv));   /* the advertise data set through the hook */
    T_EQ(coop_ldn_connect(S, scan, sec, us, 301, 0), 0);
    uint32_t sip = 0, smask = 0; T_EQ(coop_ldn_ipv4(S, &sip, &smask), 0); T_EQ(sip, (hip & ~0xFFu) | 2);
    T_EQ((uint32_t)hc(LDN_INFO, 1, P(info)), 0); T_EQ(NI_COUNT(info), 2);
    /* station → host: Poll through the hook sees it, RecvFrom (the plain overload) reads it with the sender's virtual address */
    uint8_t to[16]; mk_sa(to, hip, PORT);
    T_EQ(coop_vnet_sendto(S, sfd, "join-hello", 11, 0, to, 16), 11);
    pollfd_ pf = {fd, 1, 0};
    T_EQ(r32(hc(SK_POLL, 3, P(&pf), I(1), I(2000))), 1); T(pf.revents & 1);
    sl = 16; memset(buf, 0, sizeof buf);
    T_EQ((int64_t)hc(SK_RECVFROM_P, 6, I(fd), P(buf), I(64), I(MSG_DONTWAIT_), P(from), P(&sl)), 11);
    T(!strcmp(buf, "join-hello")); T_EQ(sa_ip(from), sip); T_EQ(sa_port(from), PORT);
    /* host → station through both SendTo overloads */
    mk_sa(to, sip, PORT);
    T_EQ((int64_t)hc(SK_SENDTO, 6, I(fd), P("host-a"), I(7), I(0), P(to), I(16)), 7);
    T_EQ((int64_t)hc(SK_SENDTO_P, 6, I(fd), P("host-b"), I(7), I(0), P(to), I(16)), 7);
    for (int k = 0; k < 2; k++) {
        int64_t got = -COOP_EAGAIN; uint32_t fl = 16;
        for (int i = 0; i < 1000 && got == -COOP_EAGAIN; i++) { got = coop_vnet_recvfrom(S, sfd, buf, sizeof buf, MSG_DONTWAIT_, from, &fl); if (got == -COOP_EAGAIN) Sleep(2); }
        T_EQ(got, 7); T(!strcmp(buf, k ? "host-b" : "host-a")); T_EQ(sa_ip(from), hip);
    }
    /* Select through the hook (Horizon's 1024-bit fd_set) */
    mk_sa(to, hip, PORT);
    T_EQ(coop_vnet_sendto(S, sfd, "sel", 4, 0, to, 16), 4);
    uint64_t rd[16] = {0}; int64_t tv[2] = {2, 0};
    rd[fd >> 6] = 1ull << (fd & 63);
    T_EQ(r32(hc(SK_SELECT, 5, I(fd + 1), P(rd), I(0), I(0), P(tv))), 1); T(rd[fd >> 6] >> (fd & 63) & 1);
    sl = 16; T_EQ((int64_t)hc(SK_RECVFROM, 6, I(fd), P(buf), I(64), I(0), P(from), P(&sl)), 4);
    coop_stats st; coop_get_stats(node, &st); T_EQ(st.links, 1);
    /* the station leaves; the host's network drops back to one node */
    T_EQ(coop_ldn_disconnect(S), 0);
    int n1 = 0;
    for (int i = 0; i < 300 && n1 != 1; i++) { hc(LDN_INFO, 1, P(info)); n1 = NI_COUNT(info); if (n1 != 1) Sleep(10); }
    T_EQ(n1, 1);
    coop_destroy(S); free(scan);
    poll_stop(th);
    T_EQ(r32(hc(SK_CLOSE, 1, I(fd))), 0);
    T_EQ((uint32_t)hc(LDN_DESTROY, 0), 0);
    T_EQ((uint32_t)hc(LDN_CLOSE_AP, 0), 0);
    hc(LDN_FINI, 0);
    T_EQ(r32(hc(LDN_STATE, 0)), COOP_LS_NONE);
}

static void mode_join(void) {
    coop_cfg hcfg; char err[160];
    T_EQ(coop_cfg_parse(&hcfg, "host:0", "0000000000000001000000000000000a", err, sizeof err), 1);
    coop_node *H = coop_create(&hcfg);
    T(H != NULL);
    if (!H) return;
    T_EQ(coop_ldn_initialize(H), 0);
    int hfd = coop_vnet_socket(H, 2, DGRAM_NB, 0); uint8_t sa[16]; mk_sa(sa, 0, PORT);
    T_EQ(coop_vnet_bind(H, hfd, sa, 16), 0);
    uint8_t cfg[0x20], sec[0x44], uh[0x30], us[0x30], f[0x60], info[0x480];
    mk_netcfg(cfg); mk_sec(sec); mk_user(uh, "Host"); mk_user(us, "Visitor"); mk_filter(f);
    T_EQ(coop_ldn_open_ap(H), 0);
    T_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    uint16_t port = coop_port(H);   /* step 1c: the host listens from CreateNetwork on */
    T_EQ(coop_ldn_set_advertise(H, "advert", 7), 0);
    uint32_t hip = 0, mask = 0; T_EQ(coop_ldn_ipv4(H, &hip, &mask), 0);
    HANDLE th = poll_start(H);
    /* the hooks join it, with game datagrams over the control link only */
    char coop[48]; snprintf(coop, sizeof coop, "join:127.0.0.1:%u", port);
    _putenv_s("DM_COOP", coop); _putenv("DM_COOP_TCP=1"); _putenv("DM_UID=");
    coop_node *node = dm_coop();
    T(node != NULL);
    if (!node) { poll_stop(th); coop_destroy(H); return; }
    T_EQ(hc(PCTL_TRY, 1, I(1)) & 0xff, 1);
    check_uid(0);   /* DM_UID unset: the constant */
    T_EQ((uint32_t)hc(LDN_INIT, 0), 0);
    int fd = r32(hc(SK_SOCKET_I, 3, I(2), I(DGRAM_NB), I(0)));
    T_EQ(fd, COOP_FD_BASE);
    T_EQ(r32(hc(SK_BIND_P, 3, I(fd), P(sa), I(16))), 0);
    T_EQ((uint32_t)hc(LDN_OPEN_STA, 0), 0);
    uint8_t *scan = calloc(24, 0x480); int cnt = 0;
    for (int i = 0; i < 50 && cnt < 1; i++) { cnt = 0; T_EQ((uint32_t)hc(LDN_SCAN, 5, P(scan), P(&cnt), I(24), P(f), I(0)), 0); if (cnt < 1) Sleep(100); }
    T_EQ(cnt, 1);
    T(cnt >= 1 && !memcmp(scan + 0x26C, "advert", 7));
    T_EQ((uint32_t)hc(LDN_CLOSE_STA, 0), 0);   /* the visitor scans with one station and connects with a new one */
    T_EQ((uint32_t)hc(LDN_OPEN_STA, 0), 0);
    T_EQ((uint32_t)hc(LDN_CONNECT, 5, P(scan), P(sec), P(us), I(301), I(0)), 0);
    T_EQ(r32(hc(LDN_STATE, 0)), COOP_LS_STATION_CONNECTED);
    uint32_t sip = 0, smask = 0; T_EQ((uint32_t)hc(LDN_IP, 2, P(&sip), P(&smask)), 0); T_EQ(sip, (hip & ~0xFFu) | 2);
    T_EQ(coop_ldn_network_info(H, info), 0); T_EQ(NI_COUNT(info), 2);
    /* visitor → host (the typed SendTo), host → visitor (the typed RecvFrom polled with MSG_DONTWAIT, as the game reads) */
    uint8_t to[16], from[16]; char buf[64]; unsigned sl;
    mk_sa(to, hip, PORT);
    T_EQ((int64_t)hc(SK_SENDTO, 6, I(fd), P("visitor"), I(8), I(0), P(to), I(16)), 8);
    { int64_t got = -COOP_EAGAIN; uint32_t fl = 16;
      for (int i = 0; i < 1000 && got == -COOP_EAGAIN; i++) { got = coop_vnet_recvfrom(H, hfd, buf, sizeof buf, MSG_DONTWAIT_, from, &fl); if (got == -COOP_EAGAIN) Sleep(2); }
      T_EQ(got, 8); T(!strcmp(buf, "visitor")); T_EQ(sa_ip(from), sip); }
    mk_sa(to, sip, PORT);
    T_EQ(coop_vnet_sendto(H, hfd, "welcome", 8, 0, to, 16), 8);
    pollfd_ pf = {fd, 1, 0};
    T_EQ(r32(hc(SK_POLL_P, 3, P(&pf), I(1), I(2000))), 1);
    int64_t got = -1;
    for (int i = 0; i < 1000; i++) {
        sl = 16; got = (int64_t)hc(SK_RECVFROM, 6, I(fd), P(buf), I(64), I(MSG_DONTWAIT_), P(from), P(&sl));
        if (got >= 0 || r32(hc(SK_LASTERROR, 0)) != EAGAIN_) break;
        Sleep(2);
    }
    T_EQ(got, 8); T(!strcmp(buf, "welcome")); T_EQ(sa_ip(from), hip); T_EQ(sa_port(from), PORT);
    /* DM_COOP_TCP reached the module: no UDP path, the datagrams went over the control link */
    coop_stats st; coop_get_stats(node, &st);
    T_EQ(st.links, 1); T_EQ(st.udp_links, 0); T_EQ(st.udp_tx, 0); T(st.tcp_tx >= 1); T(st.tcp_rx >= 1);
    coop_get_stats(H, &st); T_EQ(st.udp_links, 0);
    T_EQ((uint32_t)hc(LDN_DISC, 0), 0);
    T_EQ(r32(hc(LDN_DISC_REASON, 0)), COOP_DR_BY_USER);
    T_EQ(r32(hc(LDN_STATE, 0)), COOP_LS_STATION);
    T_EQ((uint32_t)hc(LDN_CLOSE_STA, 0), 0);
    T_EQ(r32(hc(SK_CLOSE, 1, I(fd))), 0);
    hc(LDN_FINI, 0);
    free(scan);
    poll_stop(th);
    coop_destroy(H);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "off";
    if (nxrt_init(&(nxrt_options){.quiet = 1})) { printf("FAIL nxrt_init\n"); return 1; }
    if (!strcmp(mode, "off")) mode_off();
    else if (!strcmp(mode, "host")) mode_host();
    else if (!strcmp(mode, "join")) mode_join();
    else { printf("usage: nxrt-tests-coop off|host|join\n"); return 2; }
    printf("nxrt.coop_%s: %d checks, %s\n", mode, g_checks, g_fail ? "FAILED" : "ok");
    return g_fail;
}
