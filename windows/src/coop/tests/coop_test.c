// coop unit tests (MIT): hosts and stations in one process over loopback, through the same API the HLE hooks call, plus hostile peers made of raw
// sockets. Sections (all by default, or name some): config, addr, txt, loopback (the step 1 flow twice: game datagrams over UDP, then over the
// control links only), lan (an auto host: listening only while hosting, the default port, build and mods checks), hostile (malformed, truncated,
// oversized and flooding input against a host, and a fake host against a station), mdns (DNS-SD on this machine's LAN: only with COOP_TEST_MDNS=1);
// step 1d's crypto, inet and relay (coop_test_inet.c); probe (only when named: one node from DM_COOP*, for latency over real paths).
// COOP_TEST_VERBOSE=1 prints every node's log.
#include "coop_int.h"   // the module's platform layer: the hostile peers' raw sockets
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
static void msleep(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void msleep(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }
#endif
static int64_t now_ms(void) { return cplat_now_ms(); }
static uint64_t fnv64(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }   // the passphrase hash JOIN carries

static int g_fail, g_checks, g_verbose;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); g_checks++; \
    if (a_ != b_) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld, %#llx vs %#llx)\n", __FILE__, __LINE__, #a, #b, a_, b_, a_, b_); } } while (0)
#define WAIT_UNTIL(c, ms) do { int64_t u_ = now_ms() + (ms); while (!(c) && now_ms() < u_) msleep(2); } while (0)

typedef struct { const char *name; atomic_int calls, last; const char *build; uint64_t mods; } tnode;
static void t_log(void *u, const char *l) { if (g_verbose) fprintf(stderr, "  [%s] %s\n", ((tnode *)u)->name, l); }
static void t_state(void *u, int st) { tnode *t = u; atomic_store(&t->last, st); atomic_fetch_add(&t->calls, 1); }
static const char *t_build(void *u) { return ((tnode *)u)->build; }
static uint64_t t_mods(void *u, char *what, size_t n) { tnode *t = u; if (t->mods) snprintf(what, n, "test cheats"); return t->mods; }
static coop_hooks hooks(tnode *t) { coop_hooks h = {t, t_log, NULL, t_state, t_build, t_mods}; return h; }
#define BUILD_201 "76a489f3c0ffee00112233445566778899aabbcc"
#define BUILD_105 "816c5358c0ffee00112233445566778899aabbcc"

// The host game's frame loop: it reads GetNetworkInfo every frame, which releases the ACCEPTs held for it
static coop_node *_Atomic g_poll_node; static atomic_int g_poll_run;
static cthread g_poll_th;
static void poller(void *p) { (void)p; uint8_t ni[0x480]; while (atomic_load(&g_poll_run)) { coop_ldn_network_info(atomic_load(&g_poll_node), ni); msleep(5); } }
static void poll_start(coop_node *n) { atomic_store(&g_poll_node, n); atomic_store(&g_poll_run, 1); cplat_thread_start(&g_poll_th, poller, NULL); }
static void poll_stop(void) { atomic_store(&g_poll_run, 0); cplat_thread_join(g_poll_th); }

// The game's structs, as step 0 measured them (research/06 §3.1): LocalCommunicationId -1, SceneId 0, NodeCountMax, version 301
static void mk_netcfg(uint8_t c[0x20], int maxn, int lcv) { memset(c, 0, 0x20); put64(c, ~0ull); c[0x12] = (uint8_t)maxn; put16(c + 0x14, (uint32_t)lcv); }
static void mk_sec(uint8_t s[0x44], uint8_t salt) { memset(s, 0, 0x44); put16(s, 1); put16(s + 2, 0x40); for (int i = 0; i < 0x40; i++) s[4 + i] = (uint8_t)(i * 7 + salt); }   // a test passphrase
static void mk_user(uint8_t u[0x30], const char *name) { memset(u, 0, 0x30); strncpy((char *)u, name, 32); }
static void mk_filter(uint8_t f[0x60], uint32_t flags, uint16_t scene) { memset(f, 0, 0x60); put64(f, ~0ull); put16(f + 0xA, scene); put32(f + 0x20, 2); put32(f + 0x5C, flags); }
static void mk_sa(uint8_t sa[16], uint32_t ip, uint16_t port) {
    memset(sa, 0, 16); sa[1] = 2; sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16); sa[6] = (uint8_t)(ip >> 8); sa[7] = (uint8_t)ip;
}
static uint32_t sa_ip(const uint8_t sa[16]) { return (uint32_t)sa[4] << 24 | (uint32_t)sa[5] << 16 | (uint32_t)sa[6] << 8 | sa[7]; }
static uint16_t sa_port(const uint8_t sa[16]) { return (uint16_t)(sa[2] << 8 | sa[3]); }
#define PORT 49530
#define NI_COUNT(i) ((i)[0x67])
#define NI_MAX(i) ((i)[0x66])
#define NI_NODE_IP(i, k) get32((i) + 0x68 + 0x40 * (k))
#define NI_ADV_N(i) get16((i) + 0x26A)
#define NI_ADV(i) ((i) + 0x26C)

static int game_socket(coop_node *n) {   // KTGL's socket: Socket(AF_INET, SOCK_DGRAM | NonBlock) bound to INADDR_ANY:49530
    int fd = coop_vnet_socket(n, 2, 0x20000002, 0);
    uint8_t sa[16]; mk_sa(sa, 0, PORT);
    CHECK(fd >= COOP_FD_BASE);
    CHECK_EQ(coop_vnet_bind(n, fd, sa, 16), 0);
    return fd;
}
static int64_t send_to(coop_node *n, int fd, uint32_t ip, const char *msg) { uint8_t sa[16]; mk_sa(sa, ip, PORT); return coop_vnet_sendto(n, fd, msg, strlen(msg) + 1, 0, sa, 16); }
static int64_t recv_wait(coop_node *n, int fd, char *buf, size_t cap, uint8_t from[16], int ms) {   // MSG_DONTWAIT polling, as the game reads
    int64_t u = now_ms() + ms, k;
    uint32_t fl = 16;
    while ((k = coop_vnet_recvfrom(n, fd, buf, cap, 0x80, from, &fl)) == -COOP_EAGAIN && now_ms() < u) msleep(2);
    return k;
}
static coop_node *mk_node(const char *coop, const char *uid, tnode *t, int tcp_only) {
    coop_cfg c; char err[200];
    CHECK_EQ(coop_cfg_parse(&c, coop, uid, err, sizeof err), 1);
    c.tcp_only = tcp_only; c.hooks = hooks(t); c.bind_loopback = 1; c.discovery = 0;
    return coop_create(&c);
}
static int scan_until(coop_node *S, uint8_t *scan, const uint8_t *f, int want, int ms) {   // the station redials a host that came back: scan until it answers
    int cnt = 0; int64_t u = now_ms() + ms;
    do { cnt = 0; coop_ldn_scan(S, scan, &cnt, 24, f, 0); if (cnt == want) break; msleep(50); } while (now_ms() < u);
    return cnt;
}

// ---------- raw sockets: a hostile peer that speaks frames by hand ----------
static csock raw_connect(uint16_t port) {
    csock s = cplat_tcp_connect(0x7F000001u, port);
    if (s == CSOCK_BAD) return s;
    cpollfd p = {s, CP_OUT, 0};
    cplat_poll(&p, 1, 2000);
    if (!(p.got & CP_OUT) || cplat_tcp_connected(s) != 1) { cplat_close(s); return CSOCK_BAD; }
    return s;
}
static void raw_send(csock s, const void *b, size_t n) {
    const uint8_t *p = b; int64_t u = now_ms() + 2000;
    while (n && now_ms() < u) { int64_t k = cplat_send(s, p, n); if (k > 0) { p += k; n -= (size_t)k; } else if (k == -1) msleep(1); else break; }
}
static void raw_frame(csock s, int type, const void *body, size_t len) {
    uint8_t h[5]; put32(h, (uint32_t)len + 1); h[4] = (uint8_t)type;
    raw_send(s, h, 5); if (len) raw_send(s, body, len);
}
// Reads one frame: its type and body length, -1 when the link closed, -2 on a timeout
static int raw_read(csock s, uint8_t *body, size_t cap, size_t *blen, int ms) {
    uint8_t h[5]; size_t got = 0, need = 5; int64_t u = now_ms() + ms; uint32_t fl = 0;
    while (now_ms() < u) {
        cpollfd p = {s, CP_IN, 0};
        cplat_poll(&p, 1, 10);
        if (!p.got) continue;
        uint8_t *dst = got < 5 ? h + got : body + got - 5;
        int64_t k = cplat_recv(s, dst, need - got);
        if (k == 0 || k < -1) return -1;
        if (k < 0) continue;
        got += (size_t)k;
        if (got == 5) { fl = get32(h); if (fl < 1 || fl - 1 > cap) return -1; need = 4 + fl; }
        if (got == need && got >= 5) { *blen = fl - 1; return h[4]; }
    }
    return -2;
}
static int raw_closed(csock s, int ms) {   // 1 when the other end closed the link within ms (whatever it sent first is read away)
    uint8_t b[2048]; int64_t u = now_ms() + ms;
    while (now_ms() < u) {
        cpollfd p = {s, CP_IN, 0};
        cplat_poll(&p, 1, 10);
        if (!p.got) continue;
        int64_t k = cplat_recv(s, b, sizeof b);
        if (k == 0 || k < -1) return 1;
    }
    return 0;
}
static void raw_hello(csock s, uint16_t proto) {
    uint8_t h[MP_HELLO_LEN]; put32(h, MP_MAGIC); put16(h + 4, proto); h[6] = 1; memset(h + 7, 0x5A, 16);
    raw_frame(s, M_HELLO, h, sizeof h);
}
// A NetworkInfo as a host would send it: nodes connected of maxn, on 169.254.sub.0/24
static void mk_info(uint8_t *p, int nodes, int maxn, uint8_t sub, const char *adv) {
    memset(p, 0, 0x480);
    put64(p, ~0ull); for (int i = 0; i < 16; i++) p[0x10 + i] = (uint8_t)(0xA0 + i);
    p[0x26] = 32; memset(p + 0x27, 'a', 32); p[0x48] = 1; p[0x4A] = 3; p[0x4B] = 2;
    uint8_t *l = p + 0x50;
    l[0x16] = (uint8_t)maxn; l[0x17] = (uint8_t)nodes;
    for (int k = 0; k < nodes; k++) { uint8_t *nd = l + 0x18 + 0x40 * k; put32(nd, 0xA9FE0000u | (uint32_t)sub << 8 | (uint32_t)(k + 1)); nd[0xA] = (uint8_t)k; nd[0xB] = 1; nd[0xC] = 'N'; put16(nd + 0x2E, 301); }
    size_t al = strlen(adv); put16(l + 0x21A, (uint32_t)al); memcpy(l + 0x21C, adv, al);
}

static void test_config(void) {
    fprintf(stderr, "== config\n");
    coop_cfg c; char err[200];
    CHECK_EQ(coop_cfg_parse(&c, NULL, NULL, err, sizeof err), 0);
    CHECK_EQ(coop_cfg_parse(&c, "", NULL, err, sizeof err), 0);
    CHECK_EQ(coop_cfg_parse(&c, "off", NULL, err, sizeof err), 0);
    CHECK_EQ(coop_cfg_parse(&c, "0", NULL, err, sizeof err), 0);
    CHECK_EQ(coop_cfg_parse(&c, "host:47600", NULL, err, sizeof err), 1);
    CHECK_EQ(c.role, COOP_ROLE_HOST); CHECK_EQ(c.port, 47600); CHECK_EQ(c.platform, 1); CHECK_EQ(c.scope, COOP_SCOPE_LOOPBACK);
    CHECK_EQ(get64(c.uid), 0x78626c0000000001ll); CHECK_EQ(get64(c.uid + 8), 0x53574e4154495645ll);   // nxnative's constant when DM_UID is unset
    CHECK_EQ(coop_cfg_parse(&c, "auto", NULL, err, sizeof err), 1);
    CHECK_EQ(c.role, COOP_ROLE_AUTO); CHECK_EQ(c.port, COOP_PORT_DEFAULT); CHECK_EQ(c.scope, COOP_SCOPE_LAN); CHECK_EQ(c.discovery, 1);
    CHECK_EQ(coop_cfg_parse(&c, "auto:47612", NULL, err, sizeof err), 1); CHECK_EQ(c.port, 47612);
    CHECK_EQ(coop_cfg_parse(&c, "auto:0", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "auto:x", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:127.0.0.1:47600", "0123456789abcdef0011223344556677", err, sizeof err), 1);
    CHECK_EQ(c.role, COOP_ROLE_JOIN); CHECK_EQ(c.addr, 0x7F000001); CHECK_EQ(c.port, 47600); CHECK_EQ(c.scope, COOP_SCOPE_LOOPBACK);
    CHECK_EQ(get64(c.uid), 0x0123456789abcdefll); CHECK_EQ(get64(c.uid + 8), 0x0011223344556677ll);
    CHECK_EQ(c.uid[0], 0xef);   // Uid word 0 little-endian in memory, as nn::account::Uid lies
    CHECK_EQ(coop_cfg_parse(&c, "join:localhost:5", NULL, err, sizeof err), 1);
    CHECK_EQ(coop_cfg_parse(&c, "join:127.0.0.1", NULL, err, sizeof err), 1); CHECK_EQ(c.port, COOP_PORT_DEFAULT);
    // step 1c: LAN addresses, never public ones
    CHECK_EQ(coop_cfg_parse(&c, "join:10.20.1.20", NULL, err, sizeof err), 1); CHECK_EQ(c.addr, 0x0A140114u); CHECK_EQ(c.scope, COOP_SCOPE_LAN);
    CHECK_EQ(coop_cfg_parse(&c, "join:10.0.0.1:47600", NULL, err, sizeof err), 1);
    CHECK_EQ(coop_cfg_parse(&c, "join:172.16.0.1:1", NULL, err, sizeof err), 1);
    CHECK_EQ(coop_cfg_parse(&c, "join:172.31.255.255:1", NULL, err, sizeof err), 1);
    CHECK_EQ(coop_cfg_parse(&c, "join:169.254.3.4:5", NULL, err, sizeof err), 1);
    CHECK_EQ(coop_cfg_parse(&c, "join:8.8.8.8:47600", NULL, err, sizeof err), -1);
    CHECK(strstr(err, "internet") != NULL);
    CHECK_EQ(coop_cfg_parse(&c, "join:172.32.0.1:1", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:100.64.0.1:1", NULL, err, sizeof err), -1);   // carrier NAT is not a LAN
    CHECK_EQ(coop_cfg_parse(&c, "join:0.0.0.0:1", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:255.255.255.255:1", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:224.0.0.251:1", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:10.20.1.256", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:10.20.1", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:10.20.1.2:", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:10.20.0001.2", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:127.0.0.1:0", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "join:127.0.0.1:65536", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "host:70000", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "bogus", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "host:1", "00000000000000000000000000000000", err, sizeof err), -1);   // the invalid Uid
    CHECK_EQ(coop_cfg_parse(&c, "host:1", "0123", err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse(&c, "host:1", "0123456789abcdef001122334455667g", err, sizeof err), -1);
}

static void test_addr(void) {
    fprintf(stderr, "== addr\n");
    struct { uint32_t ip; int cls; } v4[] = {
        {0x7F000001u, COOP_ADDR_LOOPBACK}, {0x7FFFFFFEu, COOP_ADDR_LOOPBACK}, {0x0A000001u, COOP_ADDR_PRIVATE}, {0xAC100001u, COOP_ADDR_PRIVATE},
        {0xAC1FFFFFu, COOP_ADDR_PRIVATE}, {0xAC200001u, COOP_ADDR_PUBLIC}, {0xAC0FFFFFu, COOP_ADDR_PUBLIC}, {0xC0A80101u, COOP_ADDR_PRIVATE},
        {0xC0A90101u, COOP_ADDR_PUBLIC}, {0xA9FE0102u, COOP_ADDR_LINKLOCAL}, {0x08080808u, COOP_ADDR_PUBLIC}, {0x64400001u, COOP_ADDR_PUBLIC},
        {0x00000000u, COOP_ADDR_INVALID}, {0xFFFFFFFFu, COOP_ADDR_INVALID}, {0xE00000FBu, COOP_ADDR_INVALID}, {0x00000001u, COOP_ADDR_INVALID}};
    for (size_t i = 0; i < sizeof v4 / sizeof v4[0]; i++) CHECK_EQ(coop_addr4_class(v4[i].ip), v4[i].cls);
    uint8_t a[16] = {0}; a[15] = 1; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_LOOPBACK);
    memset(a, 0, 16); CHECK_EQ(coop_addr6_class(a), COOP_ADDR_INVALID);
    a[0] = 0xFD; a[15] = 7; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_PRIVATE);           // fd00::/8, a unique local address
    a[0] = 0xFC; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_PRIVATE);
    a[0] = 0xFE; a[1] = 0x80; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_LINKLOCAL);       // fe80::/10
    a[1] = 0xC0; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_PUBLIC);                       // fec0::, the old site-local, is not accepted
    a[0] = 0x20; a[1] = 0x01; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_PUBLIC);          // 2001::
    a[0] = 0xFF; a[1] = 0x02; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_INVALID);         // multicast
    memset(a, 0, 16); a[10] = a[11] = 0xFF; a[12] = 192; a[13] = 168; a[14] = 1; a[15] = 2; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_PRIVATE);   // ::ffff:10.20.1.2
    a[12] = 8; a[13] = 8; a[14] = 4; a[15] = 4; CHECK_EQ(coop_addr6_class(a), COOP_ADDR_PUBLIC);
    caddr mine[16]; int nm = cplat_local_addrs(mine, 16, 1);   // this machine's own list (Windows: with the profiles the LAN host logs a hint from)
    CHECK(nm >= 0);
    for (int i = 0; i < nm; i++) {
        CHECK((mine[i].ip >> 24) != 127); CHECK(mine[i].is_public == 0 || mine[i].is_public == 1);
        fprintf(stderr, "   address %u.%u.%u.%u/%08x%s\n", mine[i].ip >> 24, mine[i].ip >> 16 & 0xFF, mine[i].ip >> 8 & 0xFF, mine[i].ip & 0xFF, mine[i].mask,
                mine[i].is_public ? " (a Public network)" : "");
    }
    for (int cls = 0; cls <= COOP_ADDR_INVALID; cls++) {
        CHECK_EQ(coop_addr_allowed(COOP_SCOPE_LOOPBACK, cls), cls == COOP_ADDR_LOOPBACK);
        CHECK_EQ(coop_addr_allowed(COOP_SCOPE_LAN, cls), cls == COOP_ADDR_LOOPBACK || cls == COOP_ADDR_PRIVATE || cls == COOP_ADDR_LINKLOCAL);
    }
}

static void test_txt(void) {
    fprintf(stderr, "== txt\n");
    uint8_t t[200]; cdisc_entry e;
    uint32_t one = 0xC0A80114u;
    size_t n = cdisc_txt_make(t, sizeof t, 2, "76a489f3c0ffee00", 0x1122334455667788ull, &one, 1);
    CHECK_EQ(cdisc_txt_parse(t, n, &e), 1);
    CHECK_EQ(e.proto, 2); CHECK(!strcmp(e.build, "76a489f3c0ffee00")); CHECK_EQ(e.mods, 0x1122334455667788ll); CHECK_EQ(e.txt_ip, 0xC0A80114u); CHECK_EQ(e.txt_nx, 0);
    uint32_t four[4] = {0xC0A8014Fu, 0xAC140A02u, 0x0A000005u, 0xA9FE0102u};   // 10.20.1.79, 172.20.10.2, 10.0.0.5, 169.254.1.2
    n = cdisc_txt_make(t, sizeof t, 2, "-", 0, four, 4);
    CHECK_EQ(cdisc_txt_parse(t, n, &e), 1); CHECK_EQ(e.txt_ip, four[0]); CHECK_EQ(e.txt_nx, 3);
    CHECK_EQ(e.txt_x[0], four[1]); CHECK_EQ(e.txt_x[1], four[2]); CHECK_EQ(e.txt_x[2], four[3]);
    n = cdisc_txt_make(t, sizeof t, 2, "", 0, NULL, 0);
    CHECK_EQ(cdisc_txt_parse(t, n, &e), 1); CHECK_EQ(e.build[0], 0); CHECK_EQ(e.txt_ip, 0); CHECK_EQ(e.txt_nx, 0);
    CHECK_EQ(cdisc_txt_parse(t, n - 1, &e), 0);                      // the last string cut short: it runs past the record
    t[0] = 200; CHECK_EQ(cdisc_txt_parse(t, n, &e), 0);              // a length past the end
    static const uint8_t junk[][48] = {
        "\x03p=2\x03" "b=-\x05m=abc",                                  // m= not 16 digits
        "\x03p=0\x03" "b=-\x12m=0000000000000000",                     // protocol 0
        "\x07p=99999\x03" "b=-\x12m=0000000000000000",                 // protocol out of range
        "\x03p=2\x12" "b=76a489f3c0ffee0g\x12m=0000000000000000",     // a non-hex digit
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) CHECK_EQ(cdisc_txt_parse(junk[i], strlen((const char *)junk[i]), &e), 0);
    static const uint8_t noa[] = "\x03p=2\x03" "b=-\x12m=0000000000000000\x0b" "a=300.1.1.1";
    CHECK_EQ(cdisc_txt_parse(noa, sizeof noa - 1, &e), 0);           // a bad a= spoils the record
    static const uint8_t badx[][80] = {
        "\x03p=2\x03" "b=-\x12m=0000000000000000\x0b" "x=10.0.0.1,",                     // a trailing comma
        "\x03p=2\x03" "b=-\x12m=0000000000000000\x0d" "x=10.0.0.1,,1",                   // an empty address
        "\x03p=2\x03" "b=-\x12m=0000000000000000\x02" "x=",                              // no address
        "\x03p=2\x03" "b=-\x12m=0000000000000000\x25" "x=10.0.0.1,10.0.0.2,10.0.0.3,10.0.0.4", // more than three
    };
    for (size_t i = 0; i < sizeof badx / sizeof badx[0]; i++) CHECK_EQ(cdisc_txt_parse(badx[i], strlen((const char *)badx[i]), &e), 0);
    // the host's order: reachable networks first (Windows: not Public), private before link-local, the route's own address first among equals
    caddr pc[] = {{0xAC140A02u, 0xFFFFFFF0u, 1},    // 172.20.10.2/28: an iPhone's USB link, a Public network, and the route's address
                  {0xA9FE0102u, 0xFFFF0000u, 0},    // 169.254.1.2/16: link-local
                  {0x08080808u, 0xFFFFFF00u, 0},    // a public address: never advertised
                  {0xC0A8014Fu, 0xFFFFFF00u, 0},    // 10.20.1.79/24: a private test network
                  {0x0A000005u, 0xFF000000u, 0},    // 10.0.0.5/8
                  {0xC0A8014Fu, 0xFFFFFF00u, 0}};   // the same address again
    uint32_t got[DISC_ADDRS];
    CHECK_EQ(cdisc_host_addrs(pc, 6, 0xAC140A02u, got, DISC_ADDRS), 4);
    CHECK_EQ(got[0], 0xC0A8014Fu); CHECK_EQ(got[1], 0x0A000005u); CHECK_EQ(got[2], 0xA9FE0102u); CHECK_EQ(got[3], 0xAC140A02u);
    CHECK_EQ(cdisc_host_addrs(pc, 6, 0x0A000005u, got, 2), 2);
    CHECK_EQ(got[0], 0x0A000005u); CHECK_EQ(got[1], 0xC0A8014Fu);
    CHECK_EQ(cdisc_host_addrs(pc + 2, 1, 0, got, DISC_ADDRS), 0);
    // the visitor's choice: the first of the host's addresses on one of its own subnets, else the first
    uint32_t host[3] = {0xAC140A02u, 0xC0A8014Fu, 0x0A000005u};
    caddr phone[] = {{0xC0A80122u, 0xFFFFFF00u, 0}, {0x0A400001u, 0xFF000000u, 0}};   // Wi-Fi fixture /24, cellular 10.64.0.1/8
    CHECK_EQ(cdisc_pick(host, 3, phone, 2), 1);
    caddr usb[] = {{0xAC140A01u, 0xFFFFFFF0u, 0}};   // only the USB link 172.20.10.1/28
    CHECK_EQ(cdisc_pick(host, 3, usb, 1), 0);
    caddr away[] = {{0xC0A80222u, 0xFFFFFF00u, 0}, {0xC0A8014Fu, 0, 0}};   // another subnet; a mask of 0 matches nothing
    CHECK_EQ(cdisc_pick(host, 3, away, 2), 0);
    uint32_t two[2] = {0xC0A8014Fu, 0xC0A80150u};
    caddr self[] = {{0xC0A8014Fu, 0xFFFFFF00u, 0}};   // its own address is not "a neighbour"; 10.20.1.80 is
    CHECK_EQ(cdisc_pick(two, 2, self, 1), 1);
    CHECK_EQ(cdisc_pick(host, 3, NULL, 0), 0);
    static const uint8_t extra[] = "\x03p=2\x03" "b=-\x12m=0000000000000000\x05z=abc\x01q";
    CHECK_EQ(cdisc_txt_parse(extra, sizeof extra - 1, &e), 1);       // unknown keys and bare strings are skipped
    static const uint8_t missing[] = "\x03p=2\x12m=0000000000000000";
    CHECK_EQ(cdisc_txt_parse(missing, sizeof missing - 1, &e), 0);
    CHECK_EQ(cdisc_txt_parse(NULL, 0, &e), 0);
    CHECK_EQ(cdisc_name_ok("Pokopia-1a2b3c4d", 64), 1);
    CHECK_EQ(cdisc_name_ok("", 64), 0);
    CHECK_EQ(cdisc_name_ok("bad\x01name", 64), 0);
    char longname[80]; memset(longname, 'x', 79); longname[79] = 0; CHECK_EQ(cdisc_name_ok(longname, 64), 0);
}

static void run(int tcp_only) {
    fprintf(stderr, "== loopback: game datagrams over %s\n", tcp_only ? "the control links (tcp_only)" : "UDP");
    tnode th = {.name = "host"}, t1 = {.name = "sta1"}, t2 = {.name = "sta2"}, t3 = {.name = "sta3"};
    coop_node *H = mk_node("host:0", "000000000000000100000000000000aa", &th, tcp_only);
    CHECK(H != NULL);
    uint8_t cfg[0x20], sec[0x44], badsec[0x44], uh[0x30], u1[0x30], u2[0x30], u3[0x30], f[0x60], info[0x480], sinfo[0x480];
    mk_netcfg(cfg, 3, 301); mk_sec(sec, 3); mk_sec(badsec, 4); mk_user(uh, "Host"); mk_user(u1, "Sta1"); mk_user(u2, "Sta2"); mk_user(u3, "Sta3");
    uint8_t *scan = malloc(0x480 * 24); int cnt = -1;

    // ---- the host, in the game's order (step 0): Initialize, Socket + Bind, a first RecvFrom, OpenAccessPoint, CreateNetwork, SetAdvertiseData
    CHECK_EQ(coop_vnet_socket(H, 2, 0x20000002, 0), -COOP_ENETDOWN);   // no virtual sockets before Initialize
    CHECK_EQ(coop_ldn_state(H), COOP_LS_NONE);
    CHECK_EQ(coop_ldn_initialize(H), 0);
    CHECK_EQ(coop_ldn_state(H), COOP_LS_INITIALIZED);
    CHECK_EQ(coop_port(H), 0);                                  // step 1c: nothing listens until the game hosts
    CHECK_EQ(coop_vnet_socket(H, 2, 1, 0), -COOP_ENETDOWN);   // stream sockets stay offline
    int hs = game_socket(H);
    CHECK_EQ(hs, COOP_FD_BASE);
    { char b[16]; uint8_t fr[16]; uint32_t fl = 16; CHECK_EQ(coop_vnet_recvfrom(H, hs, b, sizeof b, 0x80, fr, &fl), -COOP_EAGAIN); }
    CHECK_EQ(coop_ldn_network_info(H, info), COOP_LDN_INVALID_STATE);
    CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), COOP_LDN_INVALID_STATE);   // not an access point yet
    CHECK_EQ(coop_ldn_open_ap(H), 0);
    CHECK_EQ(coop_port(H), 0);
    CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    uint16_t port = coop_port(H);
    CHECK(port != 0);
    poll_start(H);
    CHECK_EQ(coop_ldn_state(H), COOP_LS_AP_CREATED);
    uint8_t adv[378]; for (int i = 0; i < 378; i++) adv[i] = (uint8_t)(i * 13 + 1);
    CHECK_EQ(coop_ldn_set_advertise(H, adv, 378), 0);
    CHECK_EQ(coop_ldn_set_advertise(H, adv, 0x181), COOP_LDN_INVALID_ARGUMENT);
    uint32_t hip = 0, mask = 0;
    CHECK_EQ(coop_ldn_ipv4(H, &hip, &mask), 0);
    CHECK_EQ(hip & 0xFFFF0000u, 0xA9FE0000u); CHECK_EQ(hip & 0xFF, 1); CHECK_EQ(mask, 0xFFFFFF00u);
    uint32_t net = hip & 0xFFFFFF00u, bcast = net | 0xFF;
    CHECK_EQ(coop_ldn_network_info(H, info), 0);
    CHECK_EQ(NI_COUNT(info), 1); CHECK_EQ(NI_MAX(info), 3); CHECK_EQ(NI_NODE_IP(info, 0), hip);
    CHECK_EQ(get64(info), ~0ull);                     // IntentId passes through
    CHECK_EQ(info[0x4B], 2);                          // NetworkType Ldn
    CHECK_EQ(get16(info + 0x68 + 0x2E), 301);         // node 0's LocalCommunicationVersion
    CHECK(!memcmp(info + 0x68 + 0xC, "Host", 5));     // node 0's UserName
    CHECK_EQ(NI_ADV_N(info), 378); CHECK(!memcmp(NI_ADV(info), adv, 378));
    { uint8_t c2[0x20]; CHECK_EQ(coop_ldn_network_config(H, c2), 0); CHECK(!memcmp(c2, cfg, 0x20)); }
    { uint8_t copy[0x480]; char why[96]; memcpy(copy, info, 0x480); CHECK_EQ(cldn_info_sane(copy, why, sizeof why), 1); CHECK(!memcmp(copy, info, 0x480)); }   // our own passes untouched
    CHECK(atomic_load(&th.calls) >= 4);
    CHECK_EQ(atomic_load(&th.last), COOP_LS_AP_CREATED);

    // ---- station 1: Initialize, the socket on the same port (no host port is used), OpenStation, Scan
    char join[64]; snprintf(join, sizeof join, "join:127.0.0.1:%u", port);
    coop_node *S1 = mk_node(join, "0000000000000001000000000000000b", &t1, tcp_only);
    CHECK_EQ(coop_ldn_initialize(S1), 0);
    int s1 = game_socket(S1);
    CHECK_EQ(coop_ldn_scan(S1, scan, &cnt, 24, NULL, 0), COOP_LDN_INVALID_STATE);   // not a station yet
    CHECK_EQ(coop_ldn_open_station(S1), 0);
    mk_filter(f, 0x21, 0);   // the game's filter: LocalCommunicationId | SceneId
    CHECK_EQ(coop_ldn_scan(S1, scan, &cnt, 24, f, 0), 0);
    CHECK_EQ(cnt, 1);
    CHECK(!memcmp(scan, info, 0x480));                // the host's NetworkInfo, advertise data and all
    mk_filter(f, 0x21, 5);
    CHECK_EQ(coop_ldn_scan(S1, scan + 0x480, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 0);   // another scene
    mk_filter(f, 0x02, 0);
    CHECK_EQ(coop_ldn_scan(S1, scan + 0x480, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 0);   // another session
    mk_filter(f, 0x21, 0);

    // ---- Connect: version mismatch, wrong passphrase, then the real one
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, u1, 300, 0), COOP_LDN_VERSION_TOO_LOW);
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, u1, 302, 0), COOP_LDN_VERSION_TOO_HIGH);
    CHECK_EQ(coop_ldn_connect(S1, scan, badsec, u1, 301, 0), COOP_LDN_CONNECT_REJECTED);
    CHECK_EQ(coop_ldn_state(S1), COOP_LS_STATION);
    { uint8_t stale[0x480]; memcpy(stale, scan, 0x480); stale[0x10] ^= 1; CHECK_EQ(coop_ldn_connect(S1, stale, sec, u1, 301, 0), COOP_LDN_CONNECT_NOT_FOUND); }   // no host has it
    int h_calls = atomic_load(&th.calls), s1_calls = atomic_load(&t1.calls);
    int64_t t0 = now_ms();
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, u1, 301, 0), 0);
    CHECK(now_ms() - t0 < 250);   // ACCEPT went out as soon as the host game read the NetworkInfo
    CHECK_EQ(coop_ldn_state(S1), COOP_LS_STATION_CONNECTED);
    CHECK(atomic_load(&t1.calls) > s1_calls); CHECK_EQ(atomic_load(&t1.last), COOP_LS_STATION_CONNECTED);
    uint32_t ip1 = 0;
    CHECK_EQ(coop_ldn_ipv4(S1, &ip1, &mask), 0); CHECK_EQ(ip1, net | 2);
    WAIT_UNTIL(coop_ldn_network_info(H, info) == 0 && NI_COUNT(info) == 2, 2000);
    CHECK_EQ(NI_COUNT(info), 2); CHECK_EQ(NI_NODE_IP(info, 1), ip1); CHECK(!memcmp(info + 0x68 + 0x40 + 0xC, "Sta1", 5));
    CHECK(atomic_load(&th.calls) > h_calls);
    CHECK_EQ(coop_ldn_network_info(S1, sinfo), 0); CHECK_EQ(NI_COUNT(sinfo), 2); CHECK(!memcmp(sinfo, info, 0x480));
    { uint8_t c2[0x20]; CHECK_EQ(coop_ldn_network_config(S1, c2), 0); CHECK_EQ(c2[0x12], 3); CHECK_EQ(get16(c2 + 0x14), 301); }

    // ---- station 2
    coop_node *S2 = mk_node(join, "0000000000000001000000000000000c", &t2, tcp_only);
    CHECK_EQ(coop_ldn_initialize(S2), 0);
    int s2 = game_socket(S2);
    CHECK_EQ(coop_ldn_open_station(S2), 0);
    CHECK_EQ(coop_ldn_scan(S2, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);
    CHECK_EQ(coop_ldn_connect(S2, scan, sec, u2, 301, 0), 0);
    uint32_t ip2 = 0; CHECK_EQ(coop_ldn_ipv4(S2, &ip2, &mask), 0); CHECK_EQ(ip2, net | 3);
    WAIT_UNTIL(coop_ldn_network_info(S1, sinfo) == 0 && NI_COUNT(sinfo) == 3, 2000);   // S1 hears of S2 through SYNC
    CHECK_EQ(NI_COUNT(sinfo), 3); CHECK_EQ(NI_NODE_IP(sinfo, 2), ip2);

    // ---- station 3: the network is full (Number of Players: Three)
    coop_node *S3 = mk_node(join, "0000000000000001000000000000000d", &t3, tcp_only);
    CHECK_EQ(coop_ldn_initialize(S3), 0);
    int s3 = game_socket(S3);
    CHECK_EQ(coop_ldn_open_station(S3), 0);
    CHECK_EQ(coop_ldn_scan(S3, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);
    CHECK_EQ(NI_COUNT(scan), 3);
    CHECK_EQ(coop_ldn_connect(S3, scan, sec, u3, 301, 0), COOP_LDN_TOO_MANY_PLAYERS);
    CHECK_EQ(coop_ldn_state(S3), COOP_LS_STATION);

    if (!tcp_only) {   // the UDP endpoints are proven right after ACCEPT
        coop_stats st;
        WAIT_UNTIL((coop_get_stats(H, &st), st.udp_links == 2), 2000); CHECK_EQ(st.udp_links, 2); CHECK_EQ(st.links, 3);
        WAIT_UNTIL((coop_get_stats(S1, &st), st.udp_links == 1), 2000); CHECK_EQ(st.udp_links, 1);
        WAIT_UNTIL((coop_get_stats(S2, &st), st.udp_links == 1), 2000); CHECK_EQ(st.udp_links, 1);
    }

    // ---- datagrams: unicast both ways; the receiver sees the sender's virtual address and port
    char b[4200]; uint8_t fr[16];
    CHECK_EQ(send_to(S1, s1, hip, "s1->host"), 9);
    CHECK_EQ(recv_wait(H, hs, b, sizeof b, fr, 2000), 9); CHECK(!strcmp(b, "s1->host")); CHECK_EQ(sa_ip(fr), ip1); CHECK_EQ(sa_port(fr), PORT); CHECK_EQ(fr[1], 2);
    CHECK_EQ(send_to(H, hs, ip1, "host->s1"), 9);
    CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 2000), 9); CHECK(!strcmp(b, "host->s1")); CHECK_EQ(sa_ip(fr), hip);
    // station to station through the host's relay
    CHECK_EQ(send_to(S1, s1, ip2, "s1->s2"), 7);
    CHECK_EQ(recv_wait(S2, s2, b, sizeof b, fr, 2000), 7); CHECK(!strcmp(b, "s1->s2")); CHECK_EQ(sa_ip(fr), ip1); CHECK_EQ(sa_port(fr), PORT);
    CHECK_EQ(send_to(S2, s2, ip1, "s2->s1"), 7);
    CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 2000), 7); CHECK(!strcmp(b, "s2->s1")); CHECK_EQ(sa_ip(fr), ip2);
    // broadcast from the host to the /24: every station, not the host itself
    CHECK_EQ(send_to(H, hs, bcast, "host bcast"), 11);
    CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 2000), 11); CHECK(!strcmp(b, "host bcast")); CHECK_EQ(sa_ip(fr), hip);
    CHECK_EQ(recv_wait(S2, s2, b, sizeof b, fr, 2000), 11); CHECK(!strcmp(b, "host bcast"));
    // broadcast from a station (255.255.255.255): the host and the other station
    CHECK_EQ(send_to(S2, s2, 0xFFFFFFFFu, "s2 bcast"), 9);
    CHECK_EQ(recv_wait(H, hs, b, sizeof b, fr, 2000), 9); CHECK(!strcmp(b, "s2 bcast")); CHECK_EQ(sa_ip(fr), ip2);
    CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 2000), 9); CHECK(!strcmp(b, "s2 bcast")); CHECK_EQ(sa_ip(fr), ip2);
    msleep(50);
    CHECK_EQ(recv_wait(H, hs, b, sizeof b, fr, 0), -COOP_EAGAIN);    // nobody hears their own broadcast, nothing went twice
    CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 0), -COOP_EAGAIN);
    CHECK_EQ(recv_wait(S2, s2, b, sizeof b, fr, 0), -COOP_EAGAIN);
    CHECK_EQ(recv_wait(S3, s3, b, sizeof b, fr, 0), -COOP_EAGAIN);   // not a member
    // a full-size datagram, and the limits
    { uint8_t big[COOP_DGRAM_MAX], sa[16]; for (int i = 0; i < COOP_DGRAM_MAX; i++) big[i] = (uint8_t)(i * 31);
      mk_sa(sa, ip2, PORT);
      CHECK_EQ(coop_vnet_sendto(H, hs, big, COOP_DGRAM_MAX, 0, sa, 16), COOP_DGRAM_MAX);
      CHECK_EQ(recv_wait(S2, s2, b, sizeof b, fr, 2000), COOP_DGRAM_MAX); CHECK(!memcmp(b, big, COOP_DGRAM_MAX));
      CHECK_EQ(coop_vnet_sendto(H, hs, big, COOP_DGRAM_MAX + 1, 0, sa, 16), -COOP_EMSGSIZE);
      mk_sa(sa, 0x0A000001u, PORT);
      CHECK_EQ(coop_vnet_sendto(H, hs, big, 4, 0, sa, 16), -COOP_ENETUNREACH);   // only the virtual /24 is reachable
      mk_sa(sa, 0x7F000001u, PORT);
      CHECK_EQ(coop_vnet_sendto(S1, s1, big, 4, 0, sa, 16), -COOP_ENETUNREACH);
      mk_sa(sa, hip, PORT);
      CHECK_EQ(coop_vnet_sendto(S3, s3, big, 4, 0, sa, 16), -COOP_ENETUNREACH);  // a station not in the network
    }
    // Poll / Select readiness: POLLOUT always, POLLIN when a datagram waits
    { coop_pollfd p[2] = {{s1, COOP_POLLIN | COOP_POLLOUT, 0}, {-1, COOP_POLLIN, 0}};
      CHECK_EQ(coop_vnet_poll(S1, p, 2, 0), 1); CHECK_EQ(p[0].revents, COOP_POLLOUT); CHECK_EQ(p[1].revents, 0);
      p[0].events = COOP_POLLIN;
      CHECK_EQ(coop_vnet_poll(S1, p, 1, 20), 0);
      CHECK_EQ(send_to(H, hs, ip1, "ready"), 6);
      CHECK_EQ(coop_vnet_poll(S1, p, 1, 2000), 1); CHECK_EQ(p[0].revents, COOP_POLLIN);
      uint64_t rd[16] = {0}, wr[16] = {0};
      rd[s1 >> 6] |= 1ull << (s1 & 63); wr[s1 >> 6] |= 1ull << (s1 & 63);
      CHECK_EQ(coop_vnet_select(S1, s1 + 1, rd, wr, NULL, 0), 2);
      CHECK(rd[s1 >> 6] >> (s1 & 63) & 1);
      CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 0), 6);
      rd[s1 >> 6] |= 1ull << (s1 & 63); memset(wr, 0, sizeof wr);
      CHECK_EQ(coop_vnet_select(S1, s1 + 1, rd, wr, NULL, 0), 0); CHECK(!(rd[s1 >> 6] >> (s1 & 63) & 1));
    }
    // a full queue drops the oldest: 300 sent, the last 256 kept
    { coop_stats st0, st; coop_get_stats(S1, &st0);
      for (int i = 0; i < 300; i++) { char m[16]; snprintf(m, sizeof m, "q%d", i); CHECK_EQ(send_to(H, hs, ip1, m), (int64_t)strlen(m) + 1); }
      WAIT_UNTIL((coop_get_stats(S1, &st), st.udp_rx + st.tcp_rx - st0.udp_rx - st0.tcp_rx >= 300), 3000);   // all arrived; the queue kept what it could
      CHECK_EQ(st.udp_rx + st.tcp_rx - st0.udp_rx - st0.tcp_rx, 300);
      if (tcp_only) { CHECK_EQ(st.udp_rx, 0); CHECK_EQ(st.udp_tx, 0); } else CHECK_EQ(st.udp_rx - st0.udp_rx, 300);
      int got = 0, first = -1; int64_t k; uint8_t peek[16]; uint32_t fl = 16;
      CHECK_EQ(coop_vnet_recvfrom(S1, s1, b, sizeof b, 0x80 | 0x2, peek, &fl), 4);   // MSG_PEEK leaves it queued
      while ((k = coop_vnet_recvfrom(S1, s1, b, sizeof b, 0x80, fr, NULL)) > 0) { if (first < 0) first = atoi(b + 1); got++; }
      CHECK_EQ(k, -COOP_EAGAIN); CHECK_EQ(got, COOP_QUEUE_MAX); CHECK_EQ(first, 300 - COOP_QUEUE_MAX);
      CHECK_EQ(st.rate_drops, 0); }   // the game's rates are far under the limits

    // ---- advertise data changes reach the stations (SYNC) and fire their state events
    s1_calls = atomic_load(&t1.calls);
    adv[0] ^= 0xFF; adv[377] = 7;
    CHECK_EQ(coop_ldn_set_advertise(H, adv, 378), 0);
    WAIT_UNTIL(coop_ldn_network_info(S1, sinfo) == 0 && NI_ADV(sinfo)[0] == adv[0], 2000);
    CHECK(!memcmp(NI_ADV(sinfo), adv, 378));
    // The state callback is dispatched asynchronously after the updated table is visible.
    WAIT_UNTIL(atomic_load(&t1.calls) > s1_calls, 2000);
    CHECK(atomic_load(&t1.calls) > s1_calls);

    // ---- Disconnect: the host's table shrinks, the leaver is a station again
    CHECK_EQ(coop_ldn_disconnect(S2), 0);
    CHECK_EQ(coop_ldn_state(S2), COOP_LS_STATION); CHECK_EQ(coop_ldn_disconnect_reason(S2), COOP_DR_BY_USER);
    CHECK_EQ(coop_ldn_network_info(S2, sinfo), COOP_LDN_INVALID_STATE);
    WAIT_UNTIL(coop_ldn_network_info(H, info) == 0 && NI_COUNT(info) == 2, 2000); CHECK_EQ(NI_COUNT(info), 2);
    WAIT_UNTIL(coop_ldn_network_info(S1, sinfo) == 0 && NI_COUNT(sinfo) == 2, 2000); CHECK_EQ(NI_COUNT(sinfo), 2);
    CHECK_EQ(send_to(S2, s2, hip, "gone"), -COOP_ENETUNREACH);
    // now there is room for station 3
    CHECK_EQ(coop_ldn_scan(S3, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);
    CHECK_EQ(coop_ldn_connect(S3, scan, sec, u3, 301, 0), 0);
    uint32_t ip3 = 0; CHECK_EQ(coop_ldn_ipv4(S3, &ip3, &mask), 0); CHECK_EQ(ip3, net | 3);   // node id 2 again
    CHECK_EQ(send_to(S3, s3, ip1, "s3->s1"), 7);
    CHECK_EQ(recv_wait(S1, s1, b, sizeof b, fr, 2000), 7); CHECK_EQ(sa_ip(fr), ip3);

    // ---- Reject: the host removes station 1
    s1_calls = atomic_load(&t1.calls);
    CHECK_EQ(coop_ldn_reject(H, ip1), 0);
    WAIT_UNTIL(coop_ldn_state(S1) == COOP_LS_STATION, 2000);
    CHECK_EQ(coop_ldn_state(S1), COOP_LS_STATION); CHECK_EQ(coop_ldn_disconnect_reason(S1), COOP_DR_REJECTED);
    WAIT_UNTIL(atomic_load(&t1.calls) > s1_calls, 2000);
    CHECK(atomic_load(&t1.calls) > s1_calls);
    CHECK_EQ(coop_ldn_network_info(H, info), 0); CHECK_EQ(NI_COUNT(info), 2);
    // and it can come back
    CHECK_EQ(coop_ldn_scan(S1, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, u1, 301, 0), 0);
    CHECK_EQ(coop_ldn_ipv4(S1, &ip1, &mask), 0); CHECK_EQ(ip1, net | 2);

    // ---- DestroyNetwork: every station is told (DestroyedByUser), the host stops listening, scans find nothing
    CHECK_EQ(coop_ldn_destroy_network(H), 0);
    CHECK_EQ(coop_ldn_state(H), COOP_LS_AP);
    CHECK_EQ(coop_port(H), 0);
    WAIT_UNTIL(coop_ldn_state(S1) == COOP_LS_STATION && coop_ldn_state(S3) == COOP_LS_STATION, 2000);
    CHECK_EQ(coop_ldn_state(S1), COOP_LS_STATION); CHECK_EQ(coop_ldn_disconnect_reason(S1), COOP_DR_DESTROYED_BY_USER);
    CHECK_EQ(coop_ldn_state(S3), COOP_LS_STATION); CHECK_EQ(coop_ldn_disconnect_reason(S3), COOP_DR_DESTROYED_BY_USER);
    { coop_stats st; WAIT_UNTIL((coop_get_stats(H, &st), st.links == 0), 2000); CHECK_EQ(st.links, 0); }   // every link closed with the network
    { csock r = raw_connect(port); CHECK(r == CSOCK_BAD); if (r != CSOCK_BAD) cplat_close(r); }        // and nothing listens
    CHECK_EQ(coop_ldn_scan(S1, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 0);
    CHECK_EQ(send_to(H, hs, bcast, "x"), -COOP_ENETUNREACH);

    // ---- a second network (new session, new /24, the same port); then the host leaves Link Play entirely (Finalize)
    CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    CHECK_EQ(coop_port(H), port);
    CHECK_EQ(scan_until(S1, scan, f, 1, 3000), 1);   // the station redials
    poll_stop();   // a host game that does not read its NetworkInfo: the ACCEPT goes out after ACCEPT_WAIT_MS (300 ms) all the same
    t0 = now_ms();
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, u1, 301, 0), 0);
    CHECK(now_ms() - t0 >= 250);
    CHECK_EQ(coop_ldn_state(S1), COOP_LS_STATION_CONNECTED);
    coop_ldn_finalize(H);
    CHECK_EQ(coop_ldn_state(H), COOP_LS_NONE);
    WAIT_UNTIL(coop_ldn_state(S1) == COOP_LS_STATION, 3000);
    CHECK_EQ(coop_ldn_state(S1), COOP_LS_STATION);
    CHECK(coop_ldn_disconnect_reason(S1) == COOP_DR_DESTROYED_BY_USER || coop_ldn_disconnect_reason(S1) == COOP_DR_SIGNAL_LOST);
    CHECK_EQ(coop_ldn_scan(S1, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 0);   // nobody listens now
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, u1, 301, 0), COOP_LDN_CONNECT_NOT_FOUND);
    CHECK_EQ(coop_vnet_close(H, hs), 0); CHECK_EQ(coop_vnet_close(H, hs), -COOP_EBADF);
    // the host comes back on the same port and is found again (the station dials until it answers)
    CHECK_EQ(coop_ldn_initialize(H), 0);
    CHECK_EQ(coop_ldn_open_ap(H), 0); CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    CHECK_EQ(coop_port(H), port);
    CHECK_EQ(scan_until(S1, scan, f, 1, 3000), 1);

    // ---- Close / Finalize everything
    CHECK_EQ(coop_ldn_close_station(S1), 0); CHECK_EQ(coop_ldn_state(S1), COOP_LS_INITIALIZED);
    CHECK_EQ(coop_ldn_close_ap(H), 0); CHECK_EQ(coop_ldn_state(H), COOP_LS_INITIALIZED);
    coop_stats st; coop_get_stats(H, &st); CHECK_EQ(st.bad_input, 0); CHECK_EQ(st.refused_src, 0);   // honest peers: nothing refused or malformed
    coop_destroy(S1); coop_destroy(S2); coop_destroy(S3); coop_destroy(H);
    free(scan);
}

// An auto host (LAN scope; bound to loopback here, so a test binary gets no firewall prompt) and the step 1c checks around it
static void test_lan(void) {
    fprintf(stderr, "== lan: auto hosts, the listening window, the default port, build and mods\n");
    tnode th = {.name = "auto-host", .build = BUILD_201}, t1 = {.name = "sta-same", .build = BUILD_201}, t2 = {.name = "sta-105", .build = BUILD_105}, t3 = {.name = "sta-mods", .build = BUILD_201, .mods = 0x1234};
    uint16_t base = (uint16_t)(47700 + (now_ms() % 200) * 2);   // a port per run: a previous run's sockets may linger
    char spec[32]; snprintf(spec, sizeof spec, "auto:%u", base);
    coop_node *H = mk_node(spec, "000000000000000100000000000000aa", &th, 0);
    uint8_t cfg[0x20], sec[0x44], uh[0x30], us[0x30], f[0x60], info[0x480];
    mk_netcfg(cfg, 4, 301); mk_sec(sec, 3); mk_user(uh, "Host"); mk_user(us, "Sta"); mk_filter(f, 0x21, 0);
    uint8_t *scan = malloc(0x480 * 24); int cnt = 0;
    CHECK_EQ(coop_ldn_initialize(H), 0);
    { csock r = raw_connect(base); CHECK(r == CSOCK_BAD); if (r != CSOCK_BAD) cplat_close(r); }   // initialized, not hosting: nothing listens
    CHECK_EQ(coop_ldn_open_ap(H), 0);
    { csock r = raw_connect(base); CHECK(r == CSOCK_BAD); if (r != CSOCK_BAD) cplat_close(r); }
    CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    CHECK_EQ(coop_port(H), base);   // the port asked for
    { csock r = raw_connect(base); CHECK(r != CSOCK_BAD); if (r != CSOCK_BAD) cplat_close(r); }   // hosting: it listens
    CHECK_EQ(coop_ldn_set_advertise(H, "lan", 4), 0);
    poll_start(H);
    // a station of the same build joins over a LAN-scope link (127.0.0.1 is loopback, which LAN accepts)
    char join[48]; snprintf(join, sizeof join, "join:127.0.0.1:%u", base);
    coop_node *S1 = mk_node(join, "0000000000000001000000000000000b", &t1, 0);
    CHECK_EQ(coop_ldn_initialize(S1), 0); game_socket(S1); CHECK_EQ(coop_ldn_open_station(S1), 0);
    CHECK_EQ(coop_ldn_scan(S1, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);
    CHECK_EQ(coop_ldn_connect(S1, scan, sec, us, 301, 0), 0);
    // another game build: hidden from its scan, and refused if it tries anyway with a NetworkInfo it got elsewhere
    coop_node *S2 = mk_node(join, "0000000000000001000000000000000c", &t2, 0);
    CHECK_EQ(coop_ldn_initialize(S2), 0); CHECK_EQ(coop_ldn_open_station(S2), 0);
    CHECK_EQ(coop_ldn_scan(S2, scan + 0x480, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 0);
    CHECK_EQ(coop_ldn_connect(S2, scan, sec, us, 301, 0), COOP_LDN_CONNECT_REJECTED);   // its peer was hidden: refused before anything is sent
    // other mods: hidden too; when its mods change after the scan, the host refuses the join itself
    coop_node *S3 = mk_node(join, "0000000000000001000000000000000d", &t3, 0);
    CHECK_EQ(coop_ldn_initialize(S3), 0); CHECK_EQ(coop_ldn_open_station(S3), 0);
    CHECK_EQ(coop_ldn_scan(S3, scan + 0x480, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 0);
    t3.mods = 0;
    CHECK_EQ(coop_ldn_scan(S3, scan + 0x480, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);   // cheats off: listed again
    t3.mods = 0x99;
    coop_stats st0, st; coop_get_stats(H, &st0);
    CHECK_EQ(coop_ldn_connect(S3, scan + 0x480, sec, us, 301, 0), COOP_LDN_CONNECT_REJECTED);
    coop_get_stats(H, &st); CHECK_EQ(st.refused_join, st0.refused_join + 1);
    CHECK_EQ(coop_ldn_network_info(H, info), 0); CHECK_EQ(NI_COUNT(info), 2);   // only S1 joined
    // DestroyNetwork: links closed, nothing listens; a new network listens again on the same port
    CHECK_EQ(coop_ldn_destroy_network(H), 0);
    CHECK_EQ(coop_port(H), 0);
    WAIT_UNTIL(coop_ldn_state(S1) == COOP_LS_STATION, 2000); CHECK_EQ(coop_ldn_disconnect_reason(S1), COOP_DR_DESTROYED_BY_USER);
    WAIT_UNTIL((coop_get_stats(H, &st), st.links == 0), 2000); CHECK_EQ(st.links, 0);
    { csock r = raw_connect(base); CHECK(r == CSOCK_BAD); if (r != CSOCK_BAD) cplat_close(r); }
    CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0); CHECK_EQ(coop_port(H), base);
    CHECK_EQ(coop_ldn_close_ap(H), 0); CHECK_EQ(coop_port(H), 0);
    poll_stop();
    coop_destroy(S1); coop_destroy(S2); coop_destroy(S3); coop_destroy(H);
    // the default port taken by something else: the auto host takes one the system picks
    uint16_t bound = 0; csock blocker = cplat_tcp_listen(0x7F000001u, (uint16_t)(base + 1), &bound);
    CHECK(blocker != CSOCK_BAD);
    snprintf(spec, sizeof spec, "auto:%u", base + 1);
    tnode t4 = {.name = "auto-taken", .build = BUILD_201};
    coop_node *H2 = mk_node(spec, NULL, &t4, 0);
    CHECK_EQ(coop_ldn_initialize(H2), 0); CHECK_EQ(coop_ldn_open_ap(H2), 0); CHECK_EQ(coop_ldn_create_network(H2, cfg, sec, uh), 0);
    CHECK(coop_port(H2) != 0 && coop_port(H2) != base + 1);
    coop_destroy(H2); cplat_close(blocker);
    // a loopback host whose fixed port is taken cannot host: CreateNetwork fails, the state stays AccessPoint
    blocker = cplat_tcp_listen(0x7F000001u, 0, &bound);
    snprintf(spec, sizeof spec, "host:%u", bound);
    tnode t5 = {.name = "host-taken"};
    coop_node *H3 = mk_node(spec, NULL, &t5, 0);
    CHECK_EQ(coop_ldn_initialize(H3), 0); CHECK_EQ(coop_ldn_open_ap(H3), 0);
    CHECK_EQ(coop_ldn_create_network(H3, cfg, sec, uh), COOP_LDN_DEVICE_DISABLED); CHECK_EQ(coop_ldn_state(H3), COOP_LS_AP);
    coop_destroy(H3); cplat_close(blocker);
    free(scan);
}

// Malformed, truncated, oversized and flooding input. A host must close or ignore it and keep serving honest stations; a station must not let a
// hostile host's NetworkInfo, ACCEPT, REFUSE, SYNC or datagram flood reach the game
static void test_hostile_host(void) {
    fprintf(stderr, "== hostile: raw links against a host\n");
    tnode th = {.name = "host"}, ts = {.name = "honest"};
    coop_node *H = mk_node("host:0", "000000000000000100000000000000aa", &th, 0);
    uint8_t cfg[0x20], sec[0x44], uh[0x30], us[0x30], f[0x60], body[0x600], info[0x480];
    mk_netcfg(cfg, 4, 301); mk_sec(sec, 3); mk_user(uh, "Host"); mk_user(us, "Honest"); mk_filter(f, 0x21, 0);
    CHECK_EQ(coop_ldn_initialize(H), 0); int hs = game_socket(H); CHECK_EQ(coop_ldn_open_ap(H), 0); CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    uint16_t port = coop_port(H);
    poll_start(H);
    size_t bl; int ty; csock r;
    // garbage: a frame length of 0xFFFFFFFF, of 0, of 64 KiB; a frame before HELLO; a bad magic; a short HELLO
    static const uint8_t ff[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    r = raw_connect(port); raw_send(r, ff, sizeof ff); CHECK(raw_closed(r, 2000)); cplat_close(r);
    { uint8_t z[8] = {0}; r = raw_connect(port); raw_send(r, z, sizeof z); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    { uint8_t h[5]; put32(h, 0x10000); h[4] = M_HELLO; r = raw_connect(port); raw_send(r, h, 5); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    { uint8_t j[MP_JOIN_LEN] = {0}; r = raw_connect(port); raw_frame(r, M_JOIN, j, sizeof j); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    { uint8_t h[MP_HELLO_LEN] = {0}; put32(h, 0xDEADBEEF); put16(h + 4, MP_PROTO); r = raw_connect(port); raw_frame(r, M_HELLO, h, sizeof h); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    { uint8_t h[10]; put32(h, MP_MAGIC); put16(h + 4, MP_PROTO); r = raw_connect(port); raw_frame(r, M_HELLO, h, sizeof h); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    // another protocol: told this host's (WELCOME, protocol 2), then closed
    r = raw_connect(port); raw_hello(r, 1);
    ty = raw_read(r, body, sizeof body, &bl, 2000); CHECK_EQ(ty, M_WELCOME); CHECK_EQ(bl, MP_WELCOME_LEN); CHECK_EQ(get16(body + 4), MP_PROTO);
    CHECK(raw_closed(r, 2000)); cplat_close(r);
    // a truncated frame that never completes: closed when the HELLO time runs out (3 s), not before
    { uint8_t h[15]; put32(h, 100); h[4] = M_HELLO; memset(h + 5, 0, 10); r = raw_connect(port); raw_send(r, h, sizeof h);
      int64_t t0 = now_ms(); CHECK(raw_closed(r, 5000)); CHECK(now_ms() - t0 >= 2000); cplat_close(r); }
    // a link that said HELLO: a short JOIN closes it; a JOIN with a bad build ID closes it
    r = raw_connect(port); raw_hello(r, MP_PROTO); CHECK_EQ(raw_read(r, body, sizeof body, &bl, 2000), M_WELCOME);
    { uint8_t j[20] = {0}; raw_frame(r, M_JOIN, j, sizeof j); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    r = raw_connect(port); raw_hello(r, MP_PROTO); CHECK_EQ(raw_read(r, body, sizeof body, &bl, 2000), M_WELCOME);
    { uint8_t j[MP_JOIN_LEN] = {0}; memset(j + 0x5A, 'z', 16); raw_frame(r, M_JOIN, j, sizeof j); CHECK(raw_closed(r, 2000)); cplat_close(r); }
    // well-formed but wrong: a stale network (NotFound), then too fast (Rejected unread), then a far newer version (TooHigh); unknown types and early
    // datagrams are ignored; a scan flood is answered twenty times a second at most; the link stays up throughout
    r = raw_connect(port); raw_hello(r, MP_PROTO); CHECK_EQ(raw_read(r, body, sizeof body, &bl, 2000), M_WELCOME);
    { uint8_t j[MP_JOIN_LEN] = {0}; put16(j + 0x50, 301); raw_frame(r, M_JOIN, j, sizeof j);
      ty = raw_read(r, body, sizeof body, &bl, 2000); CHECK_EQ(ty, M_REFUSE); CHECK_EQ(get32(body), COOP_LDN_CONNECT_NOT_FOUND);
      int unread = 0;   // ten more at once: past the burst of five a second, refused unread
      for (int i = 0; i < 10; i++) raw_frame(r, M_JOIN, j, sizeof j);
      for (int i = 0; i < 10; i++) { ty = raw_read(r, body, sizeof body, &bl, 2000); CHECK_EQ(ty, M_REFUSE); unread += ty == M_REFUSE && get32(body) == COOP_LDN_CONNECT_REJECTED; }
      CHECK(unread >= 5);
      msleep(1100);
      CHECK_EQ(coop_ldn_network_info(H, info), 0); memcpy(j, info, 0x20); put16(j + 0x50, 0xFFFF); put64(j + 0x52, fnv64(sec, 0x44));
      raw_frame(r, M_JOIN, j, sizeof j);
      do ty = raw_read(r, body, sizeof body, &bl, 2000); while (ty == M_PING);
      CHECK_EQ(ty, M_REFUSE); CHECK_EQ(get32(body), COOP_LDN_VERSION_TOO_HIGH); }
    { uint8_t x[3] = {1, 2, 3}; raw_frame(r, 200, x, sizeof x); raw_frame(r, M_DGRAM, x, sizeof x); uint8_t d[MP_DG_HDR + 8] = {0}; raw_frame(r, M_DGRAM, d, sizeof d); }
    { for (int i = 0; i < 200; i++) raw_frame(r, M_SCAN_REQ, NULL, 0);
      int answers = 0; int64_t u = now_ms() + 300;
      while (now_ms() < u) { ty = raw_read(r, body, sizeof body, &bl, 50); if (ty == M_SCAN_RESP) answers++; else if (ty == -1) break; }
      CHECK(answers >= 1 && answers <= 30);   // a burst of twenty, then twenty a second
      raw_frame(r, M_PING, NULL, 0);
      do ty = raw_read(r, body, sizeof body, &bl, 1000); while (ty == M_SCAN_RESP || ty == M_PING);
      CHECK_EQ(ty, M_PONG); }   // still up
    cplat_close(r);
    // a flood of half-open links from one address: at most four wait for their HELLO, the rest are closed at once
    { csock fl[10]; int open = 0;
      for (int i = 0; i < 10; i++) fl[i] = raw_connect(port);
      msleep(200);
      for (int i = 0; i < 10; i++) if (fl[i] != CSOCK_BAD) { open += !raw_closed(fl[i], 50); }
      CHECK(open <= 4 && open >= 1);
      for (int i = 0; i < 10; i++) cplat_close(fl[i]);
      msleep(100); }
    // UDP junk at the host's datagram port: short, not ours, an unknown token, over-long
    { coop_stats st0; coop_get_stats(H, &st0);
      csock u = cplat_udp_open(0x7F000001u, 0, NULL);
      uint8_t p[9000]; memset(p, 0x5A, sizeof p);
      cplat_sendto(u, p, 3, 0x7F000001u, port);
      cplat_sendto(u, p, 64, 0x7F000001u, port);
      put32(p, MP_UMAGIC); p[4] = U_DGRAM; cplat_sendto(u, p, 64, 0x7F000001u, port);
      p[4] = U_HELLO; cplat_sendto(u, p, MP_UDP_HDR, 0x7F000001u, port);
      cplat_sendto(u, p, sizeof p, 0x7F000001u, port);
      coop_stats st; WAIT_UNTIL((coop_get_stats(H, &st), st.bad_input >= st0.bad_input + 4), 2000);
      CHECK(st.bad_input >= st0.bad_input + 4);
      cplat_close(u); }
    // after all that, an honest station still scans, joins and talks
    char join[48]; snprintf(join, sizeof join, "join:127.0.0.1:%u", port);
    coop_node *S = mk_node(join, "0000000000000001000000000000000b", &ts, 0);
    uint8_t *scan = malloc(0x480 * 24); int cnt = 0;
    CHECK_EQ(coop_ldn_initialize(S), 0); int ss = game_socket(S); CHECK_EQ(coop_ldn_open_station(S), 0);
    CHECK_EQ(coop_ldn_scan(S, scan, &cnt, 24, f, 0), 0); CHECK_EQ(cnt, 1);
    CHECK_EQ(coop_ldn_connect(S, scan, sec, us, 301, 0), 0);
    uint32_t hip, mask; CHECK_EQ(coop_ldn_ipv4(H, &hip, &mask), 0);
    char b[64]; uint8_t fr[16];
    CHECK_EQ(send_to(S, ss, hip, "still here"), 11); CHECK_EQ(recv_wait(H, hs, b, sizeof b, fr, 2000), 11);
    coop_stats st; coop_get_stats(H, &st);
    CHECK(st.bad_input >= 10); CHECK(st.rate_drops >= 1);
    fprintf(stderr, "   host after the hostile links: %llu malformed inputs, %llu rate drops, %llu joins refused\n", (unsigned long long)st.bad_input,
            (unsigned long long)st.rate_drops, (unsigned long long)st.refused_join);
    poll_stop();
    coop_destroy(S); coop_destroy(H); free(scan);
}

// A fake host: a raw listener that answers a real station's HELLO, SCAN_REQ and JOIN as the case says
typedef struct { csock l, s; uint16_t port; } fake;
static int fake_open(fake *fk) { fk->s = CSOCK_BAD; fk->l = cplat_tcp_listen(0x7F000001u, 0, &fk->port); return fk->l != CSOCK_BAD; }
static int fake_accept(fake *fk, int ms) {   // the station's link, and its HELLO read
    int64_t u = now_ms() + ms; uint32_t ip; uint16_t pt;
    while (now_ms() < u && fk->s == CSOCK_BAD) { cpollfd p = {fk->l, CP_IN, 0}; cplat_poll(&p, 1, 10); if (p.got) fk->s = cplat_tcp_accept(fk->l, &ip, &pt); }
    if (fk->s == CSOCK_BAD) return 0;
    uint8_t b[64]; size_t bl;
    return raw_read(fk->s, b, sizeof b, &bl, 2000) == M_HELLO;
}
static void fake_welcome(fake *fk, uint16_t proto) { uint8_t w[MP_WELCOME_LEN] = {0}; put32(w, MP_MAGIC); put16(w + 4, proto); put16(w + 6, 0); raw_frame(fk->s, M_WELCOME, w, sizeof w); }
static int fake_wait(fake *fk, int type, uint8_t *body, size_t cap, size_t *bl, int ms) {   // the next frame of that type (PINGs answered)
    int64_t u = now_ms() + ms;
    while (now_ms() < u) {
        int t = raw_read(fk->s, body, cap, bl, 50);
        if (t == type) return 1;
        if (t == M_PING) raw_frame(fk->s, M_PONG, NULL, 0);
        if (t == -1) return 0;
    }
    return 0;
}
static void fake_close(fake *fk) { cplat_close(fk->s); cplat_close(fk->l); fk->s = fk->l = CSOCK_BAD; }
// The station's scan is a call that waits for its answer: the fake host answers from a thread while the test's thread scans
typedef struct { fake *fk; const uint8_t *info; int has; } scan_answer;
static void fake_scan_thread(void *arg) {
    scan_answer *a = arg; uint8_t body[64]; size_t bl;
    if (!fake_wait(a->fk, M_SCAN_REQ, body, sizeof body, &bl, 3000)) return;
    uint8_t r[1 + MP_IDENT_LEN + 0x480] = {0}; r[0] = (uint8_t)a->has;
    if (a->has) memcpy(r + 1 + MP_IDENT_LEN, a->info, 0x480);
    raw_frame(a->fk->s, M_SCAN_RESP, r, a->has ? sizeof r : 1 + MP_IDENT_LEN);
}
static int fake_scan(coop_node *S, fake *fk, const uint8_t *info, uint8_t *scan) {
    scan_answer a = {fk, info, 1}; cthread th; int cnt = 0; uint8_t f[0x60]; mk_filter(f, 0x21, 0);
    cplat_thread_start(&th, fake_scan_thread, &a);
    coop_ldn_scan(S, scan, &cnt, 24, f, 0);
    cplat_thread_join(th);
    return cnt;
}
typedef struct { fake *fk; int type; const uint8_t *body; size_t len; size_t join_len; } join_answer;
static void fake_join_thread(void *arg) {
    join_answer *a = arg; uint8_t body[0x200]; size_t bl = 0;
    if (!fake_wait(a->fk, M_JOIN, body, sizeof body, &bl, 3000)) return;
    a->join_len = bl;
    raw_frame(a->fk->s, a->type, a->body, a->len);
}
static uint32_t fake_join(coop_node *S, join_answer *a, const uint8_t *scan) {
    cthread th; uint8_t sec[0x44], us[0x30]; mk_sec(sec, 3); mk_user(us, "Victim");
    cplat_thread_start(&th, fake_join_thread, a);
    uint32_t r = coop_ldn_connect(S, scan, sec, us, 301, 0);
    cplat_thread_join(th);
    return r;
}
static coop_node *fake_station(fake *fk, tnode *t) {
    char join[48]; snprintf(join, sizeof join, "join:127.0.0.1:%u", fk->port);
    coop_node *S = mk_node(join, "0000000000000001000000000000000e", t, 1);
    coop_ldn_initialize(S); coop_ldn_open_station(S);
    return S;
}
static void test_hostile_station(void) {
    fprintf(stderr, "== hostile: a fake host against a station\n");
    uint8_t good[0x480], bad[0x480], *scan = malloc(0x480 * 24);
    mk_info(good, 1, 4, 77, "fake advert");
    { char why[96]; uint8_t c[0x480]; memcpy(c, good, 0x480); CHECK_EQ(cldn_info_sane(c, why, sizeof why), 1); }
    // NetworkInfo checks on their own: every field the game trusts
    struct { int off; uint8_t v; } breaks[] = {{0x26, 33}, {0x4B, 1}, {0x50 + 0x16, 0}, {0x50 + 0x16, 9}, {0x50 + 0x17, 0}, {0x50 + 0x17, 5}, {0x50 + 0x18 + 2, 0}, {0x50 + 0x18 + 2, 255},
                                       {0x50 + 0x18, 2}, {0x50 + 0x18 + 0xA, 3}, {0x50 + 0x18 + 0xB, 2}};
    for (size_t i = 0; i < sizeof breaks / sizeof breaks[0]; i++) {
        char why[96]; memcpy(bad, good, 0x480); bad[breaks[i].off] = breaks[i].v;
        int r = cldn_info_sane(bad, why, sizeof why);
        CHECK_EQ(r, 0);
        if (r) fprintf(stderr, "   not refused: byte %#x = %u\n", breaks[i].off, breaks[i].v);
    }
    { char why[96]; memcpy(bad, good, 0x480); put16(bad + 0x50 + 0x21A, 0x181); CHECK_EQ(cldn_info_sane(bad, why, sizeof why), 0); }   // AdvertiseDataSize
    { char why[96]; memcpy(bad, good, 0x480); memset(bad + 0x50 + 0x18 + 0xC, 'x', 0x34); CHECK_EQ(cldn_info_sane(bad, why, sizeof why), 0); }   // an unterminated name
    { char why[96]; memcpy(bad, good, 0x480); bad[0x50 + 0x18 + 0x40 * 5 + 3] = 0x77; bad[0x50 + 0x21C + 100] = 0x55;   // junk in an unused slot and past the advertise data
      CHECK_EQ(cldn_info_sane(bad, why, sizeof why), 1); CHECK_EQ(bad[0x50 + 0x18 + 0x40 * 5 + 3], 0); CHECK_EQ(bad[0x50 + 0x21C + 100], 0); }

    fake fk; tnode t = {.name = "victim"};
    // another protocol in WELCOME: the host is not listed
    CHECK(fake_open(&fk)); coop_node *S = fake_station(&fk, &t);
    CHECK(fake_accept(&fk, 2000)); fake_welcome(&fk, 9);
    { int cnt = 0; uint8_t f[0x60]; mk_filter(f, 0x21, 0); coop_ldn_scan(S, scan, &cnt, 24, f, 0); CHECK_EQ(cnt, 0); }
    coop_destroy(S); fake_close(&fk);
    // a NetworkInfo with AdvertiseDataSize 0xFFFF: not listed, counted as malformed
    CHECK(fake_open(&fk)); S = fake_station(&fk, &t);
    CHECK(fake_accept(&fk, 2000)); fake_welcome(&fk, MP_PROTO);
    memcpy(bad, good, 0x480); put16(bad + 0x50 + 0x21A, 0xFFFF);
    CHECK_EQ(fake_scan(S, &fk, bad, scan), 0);
    { coop_stats st; coop_get_stats(S, &st); CHECK(st.bad_input >= 1); }
    // a sane NetworkInfo is listed; then an ACCEPT naming another node, a REFUSE with a result nn::ldn never gives, and a real ACCEPT
    CHECK_EQ(fake_scan(S, &fk, good, scan), 1);
    CHECK(!memcmp(scan, good, 0x480));
    uint8_t acc[9 + 0x480];
    { mk_info(acc + 9, 2, 4, 77, "fake advert"); acc[0] = 9; put64(acc + 1, 1);   // node 9 does not exist
      join_answer a = {&fk, M_ACCEPT, acc, sizeof acc, 0};
      CHECK_EQ(fake_join(S, &a, scan), COOP_LDN_CONNECT_REJECTED); CHECK_EQ(a.join_len, MP_JOIN_LEN); CHECK_EQ(coop_ldn_state(S), COOP_LS_STATION); }
    msleep(50);
    { uint8_t rf[4]; put32(rf, 0xDEADBEEF);
      join_answer a = {&fk, M_REFUSE, rf, 4, 0};
      CHECK_EQ(fake_join(S, &a, scan), COOP_LDN_CONNECT_REJECTED); }
    msleep(50);
    { acc[0] = 1; join_answer a = {&fk, M_ACCEPT, acc, sizeof acc, 0};
      CHECK_EQ(fake_join(S, &a, scan), 0); CHECK_EQ(coop_ldn_state(S), COOP_LS_STATION_CONNECTED); }
    uint8_t ni[0x480]; CHECK_EQ(coop_ldn_network_info(S, ni), 0); CHECK_EQ(NI_COUNT(ni), 2);
    // a SYNC the game must not read (NodeCount 9) is ignored
    { memcpy(bad, acc + 9, 0x480); bad[0x50 + 0x17] = 9; raw_frame(fk.s, M_SYNC, bad, 0x480); msleep(100);
      CHECK_EQ(coop_ldn_network_info(S, ni), 0); CHECK_EQ(NI_COUNT(ni), 2); }
    // a datagram flood: 5,000 frames at once. The station's rate limit drops most, its socket queue keeps at most 256
    int ss = game_socket(S);
    { uint8_t d[MP_DG_HDR + 8] = {0}; put32(d, 0xA9FE4D01u); put32(d + 4, 0xA9FE4D02u); put16(d + 8, PORT); put16(d + 10, PORT);
      uint8_t *burst = malloc(5000 * (5 + sizeof d)); size_t at = 0;
      for (int i = 0; i < 5000; i++) { put32(burst + at, (uint32_t)sizeof d + 1); burst[at + 4] = M_DGRAM; memcpy(burst + at + 5, d, sizeof d); at += 5 + sizeof d; }
      raw_send(fk.s, burst, at); free(burst);
      coop_stats st; WAIT_UNTIL((coop_get_stats(S, &st), st.tcp_rx + st.rate_drops >= 5000), 3000);
      CHECK(st.rate_drops >= 1000); CHECK(st.tcp_rx <= 4000);
      int queued = 0; char b[64]; uint8_t fr[16]; uint32_t fl = 16;
      while (coop_vnet_recvfrom(S, ss, b, sizeof b, 0x80, fr, &fl) >= 0) queued++;
      CHECK(queued >= 1 && queued <= COOP_QUEUE_MAX); }
    // a frame length of 0xFFFFFFFF: the link is closed and the station is out of the network (SignalLost)
    raw_send(fk.s, "\xff\xff\xff\xff\x01", 5);
    WAIT_UNTIL(coop_ldn_state(S) == COOP_LS_STATION, 2000);
    CHECK_EQ(coop_ldn_state(S), COOP_LS_STATION); CHECK_EQ(coop_ldn_disconnect_reason(S), COOP_DR_SIGNAL_LOST);
    coop_destroy(S); fake_close(&fk);
    free(scan);
}

// DNS-SD on this machine (COOP_TEST_MDNS=1): an auto host advertises, an auto station browses and resolves it. Both listen on loopback only, so the
// station cannot dial the LAN address it resolves; the discovery table is what is checked
static void test_mdns(void) {
    fprintf(stderr, "== mdns: DNS-SD advertise and browse on this machine\n");
    tnode th = {.name = "mdns-host", .build = BUILD_201}, ts = {.name = "mdns-sta", .build = BUILD_201};
    coop_cfg c; char err[200];
    uint16_t pt = (uint16_t)(47900 + now_ms() % 90);
    char spec[32]; snprintf(spec, sizeof spec, "auto:%u", pt);
    coop_cfg_parse(&c, spec, NULL, err, sizeof err); c.hooks = hooks(&th); c.bind_loopback = 1;
    coop_node *H = coop_create(&c);
    coop_cfg_parse(&c, "auto", NULL, err, sizeof err); c.hooks = hooks(&ts); c.bind_loopback = 1;
    coop_node *S = coop_create(&c);
    uint8_t cfg[0x20], sec[0x44], uh[0x30]; mk_netcfg(cfg, 4, 301); mk_sec(sec, 3); mk_user(uh, "Host");
    CHECK_EQ(coop_ldn_initialize(H), 0); CHECK_EQ(coop_ldn_open_ap(H), 0); CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    coop_stats st; WAIT_UNTIL((coop_get_stats(H, &st), st.advertising), 5000); CHECK_EQ(st.advertising, 1);
    CHECK_EQ(coop_ldn_initialize(S), 0); CHECK_EQ(coop_ldn_open_station(S), 0);
    int64_t t0 = now_ms();
    int found = 0; cdisc_entry e; memset(&e, 0, sizeof e);
    while (!found && now_ms() - t0 < 8000) {
        cplat_lock(&S->dlk);
        for (int i = 0; i < DISC_MAX; i++) if (S->disc.e[i].used && S->disc.e[i].port == pt) { e = S->disc.e[i]; found = 1; }
        cplat_unlock(&S->dlk);
        if (!found) msleep(20);
    }
    CHECK(found);
    uint32_t rip = 0; cplat_route_ip(&rip);
    if (found) {
        fprintf(stderr, "   found \"%s\" at %u.%u.%u.%u:%u after %lld ms (build %s, protocol %d)\n", e.name, e.ip >> 24, e.ip >> 16 & 0xFF, e.ip >> 8 & 0xFF, e.ip & 0xFF, e.port,
                (long long)(now_ms() - t0), e.build, e.proto);
        CHECK_EQ(e.proto, MP_PROTO); CHECK(!strncmp(e.build, BUILD_201, 16)); CHECK_EQ(e.mods, 0);
        CHECK(coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(e.ip)));
    }
    CHECK_EQ(coop_ldn_destroy_network(H), 0);   // the service goes away with the network
    WAIT_UNTIL((coop_get_stats(H, &st), !st.advertising), 3000); CHECK_EQ(st.advertising, 0);
    int gone = 0; t0 = now_ms();
    while (!gone && now_ms() - t0 < 8000) {
        gone = 1;
        cplat_lock(&S->dlk); for (int i = 0; i < DISC_MAX; i++) if (S->disc.e[i].used && S->disc.e[i].port == pt) gone = 0; cplat_unlock(&S->dlk);
        if (!gone) msleep(50);
    }
    fprintf(stderr, "   the station %s the host's goodbye\n", gone ? "saw" : "did not see (within 8 s)");
    coop_destroy(S); coop_destroy(H);
}

#include "coop_test_inet.c"   // step 1d: internet play and the relay

int main(int argc, char **argv) {
    const char *v = getenv("COOP_TEST_VERBOSE"), *m = getenv("COOP_TEST_MDNS");
    g_verbose = v && atoi(v);
    cplat_init();   // the raw sockets need Winsock even while no node exists
    int all = argc < 2;
    int want_config = all, want_addr = all, want_txt = all, want_loop = all, want_lan = all, want_hostile = all, want_mdns = m && atoi(m);
    int want_crypto = all, want_inet = all, want_relay = all, want_probe = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "config")) want_config = 1;
        else if (!strcmp(argv[i], "addr")) want_addr = 1;
        else if (!strcmp(argv[i], "txt")) want_txt = 1;
        else if (!strcmp(argv[i], "loopback")) want_loop = 1;
        else if (!strcmp(argv[i], "lan")) want_lan = 1;
        else if (!strcmp(argv[i], "hostile")) want_hostile = 1;
        else if (!strcmp(argv[i], "mdns")) want_mdns = 1;
        else if (!strcmp(argv[i], "crypto")) want_crypto = 1;
        else if (!strcmp(argv[i], "inet")) want_inet = 1;
        else if (!strcmp(argv[i], "relay")) want_relay = 1;
        else if (!strcmp(argv[i], "probe")) want_probe = 1;
        else { fprintf(stderr, "sections: config addr txt loopback lan hostile mdns crypto inet relay probe\n"); return 2; }
    }
    if (want_config) test_config();
    if (want_addr) test_addr();
    if (want_txt) test_txt();
    if (want_loop) { run(0); run(1); }
    if (want_lan) test_lan();
    if (want_hostile) { test_hostile_host(); test_hostile_station(); }
    if (want_mdns) test_mdns();
    if (want_crypto) test_crypto();
    if (want_inet) test_inet();
    if (want_relay) test_relay();
    if (want_probe) test_probe();
    fprintf(stderr, "%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
