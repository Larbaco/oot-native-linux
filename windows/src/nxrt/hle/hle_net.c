// from nxnative dm/host/hle_net.c (MIT, © seedds). Windows port: long / unsigned long are int64_t / uint64_t; __thread is _Thread_local.
// Networking (out of scope: online play/friends/store/cloud saves): all hooked up as "network unavailable", the game takes the offline branch.
// Cuts: nifm reports no network; socket only hooks up Initialize (returns success, creates no session), the rest of socket/ssl/friends are hooked up as failures when they come into use.
#include "hle.h"
#include <stdatomic.h>

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "connects to the nifm network management service; offline (cut)", Result, h_nifm_Initialize, void) { return RES_OK; }
// The request is asynchronous on Horizon: SubmitNetworkRequest only hands it to the nifm service, and IsNetworkRequestOnHold stays true until the service has evaluated it, so a query
// right after the submission finds it on hold. Pikmin 4 (Unreal Engine 4.26, main+0x203f3b0) relies on that: it starts its BCAT thread, whose Run (main+0x203f510) sets its status
// to 2, submits, waits while on hold and sets 6 (done, offline), and only then stores status 1 itself; answered at once, a thread that ran first finished before that store, the 1 stayed,
// and the boot state machine (main+0x1f2f764, state 7) waited for the status forever at the black loading screen (about 1 boot in 6, 3 in 4 with four at once). Pikmin 4 sleeps 10 ms
// between its queries, so a request that is on hold at the first query is all it needs.
// Each query is an IPC to the nifm service on Horizon, which evaluates the request as it serves them (offline, a rejection), so the request is on hold for the first query after
// the submission and settled at the next. The hold used to be 50 ms of host time: Hades' NetworkPollThread (main+0x27aaa0) polls with SleepThread(0) and cancels after 2,000
// queries, 2,000 IPCs on the Switch but 0.1 ms here, so it cancelled and submitted again forever (1.17 billion queries a minute, a host core). Its stamp, now_ns() | 1, also read
// as settled at once whenever the next query came within the same 42 ns clock tick (30% of Hades' submissions)
static _Atomic int g_nifm_hold;   // queries the pending request still answers "on hold" (0 = none, or settled)
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "offline: the request is on hold for the first query, then settles as unavailable", void, h_nifm_Submit, void) { atomic_store(&g_nifm_hold, 1); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "offline: drops the pending request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "offline: on hold for the first query after the submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "offline: the network is always unavailable", int, h_nifm_Available, void) { return 0; }
// Returns bool (`and w0, w0, #1`, Metroid Dread's sdk+0x336210): whether the system's network screen resolved the error
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "offline: not resolved, shows no system network screen", unsigned, h_nifm_HandleErr, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "offline: no IP", Result, h_nifm_GetIp, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "offline: no proxy setting", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "connects to the ssl service; the network library's global init needs it to succeed, offline creates no session (cut)", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "pairs with nnsslInitialize (cut)", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- socket: offline, all calls that contact the service return -1 in BSD convention, error code ENETDOWN ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 50                  // Horizon sockets use FreeBSD error codes
static _Thread_local int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
// Link Play (docs/coop-step1b.md): with DM_COOP set, the AF_INET datagram sockets the game makes while nn::ldn is initialized are the coop module's virtual
// sockets (descriptors 0x100 and up on 169.254.S.0/24, src/coop/coop_vnet.c). Any other descriptor, and every socket without DM_COOP, keeps the offline
// answer (-1, ENETDOWN). A module result -errno comes back as -1 with the thread's last error set (FreeBSD values). hle_offline.c's overloads call these too.
#include "coop.h"
coop_node *dm_coop(void);   // hle_offline.c: NULL without DM_COOP
static int mpsk_is(int s) { coop_node *c = dm_coop(); return c && coop_vnet_is(c, s); }
static int64_t mpsk_ret(int64_t r) { if (r >= 0) return r; t_skerr = (int)-r; return -1; }
int mpsk_errno(void) { return t_skerr; }
int mpsk_socket(int fam, int type, int proto) {
    coop_node *c = dm_coop();
    if (!c) return sk_fail();
    int r = coop_vnet_socket(c, fam, type, proto);
    if (r < 0) LOG("[coop] sock: Socket(%d, %#x, %d) stays offline (only AF_INET datagram sockets after ldn::Initialize are virtual)", fam, type, proto);
    return (int)mpsk_ret(r);
}
int mpsk_bind(int s, const void *a, unsigned n) { return mpsk_is(s) ? (int)mpsk_ret(coop_vnet_bind(dm_coop(), s, a, n)) : sk_fail(); }
int mpsk_close(int s) { return mpsk_is(s) ? (int)mpsk_ret(coop_vnet_close(dm_coop(), s)) : sk_fail(); }
int mpsk_setopt(int s, int l, int o, const void *v, unsigned n) { return mpsk_is(s) ? (int)mpsk_ret(coop_vnet_setsockopt(dm_coop(), s, l, o, v, n)) : sk_fail(); }
int mpsk_getopt(int s, int l, int o, void *v, unsigned *n) { return mpsk_is(s) ? (int)mpsk_ret(coop_vnet_getsockopt(dm_coop(), s, l, o, v, n)) : sk_fail(); }
int mpsk_getname(int s, void *a, unsigned *n) { return mpsk_is(s) ? (int)mpsk_ret(coop_vnet_getsockname(dm_coop(), s, a, n)) : sk_fail(); }
int mpsk_fcntl(int s, int cmd, int v) { return mpsk_is(s) ? (int)mpsk_ret(coop_vnet_fcntl(dm_coop(), s, cmd, v)) : sk_fail(); }
int64_t mpsk_sendto(int s, const void *b, uint64_t n, int f, const void *to, unsigned tl) { return mpsk_is(s) ? mpsk_ret(coop_vnet_sendto(dm_coop(), s, b, n, f, to, tl)) : sk_fail(); }
int64_t mpsk_send(int s, const void *b, uint64_t n, int f) { return mpsk_sendto(s, b, n, f, NULL, 0); }   // no connected sockets: DestAddrRequired on a virtual one
int64_t mpsk_recvfrom(int s, void *b, uint64_t n, int f, void *from, unsigned *fl) { return mpsk_is(s) ? mpsk_ret(coop_vnet_recvfrom(dm_coop(), s, b, n, f, from, fl)) : sk_fail(); }
int64_t mpsk_recv(int s, void *b, uint64_t n, int f) { return mpsk_recvfrom(s, b, n, f, NULL, NULL); }
#define SKF(m, cname, ...) HLE_FORCE(m, "bsd socket service (cut): returns -1/ENETDOWN", int, cname, __VA_ARGS__)
// Poll / Select with nothing to wait on: the host creates no socket, so a game's descriptors are all -1, or it passes none. POSIX and Horizon's bsd service ignore negative
// descriptors and wait out the timeout, returning 0. Failing at once made Dark Souls Remastered's network thread ("NexusRevolution Socket", main+0x92c7c4) spin a core at 100%
// on Poll(fds, 0, 33 ms), 11,000 calls a second. An endless wait on nothing never returns on hardware; here it returns 0 after 100 ms: a thread in a host hook pauses for a garbage
// collector (IL2CPP's, hle_thread.c) only once the hook returns, and the caller can still see a shutdown
void dm_guest_sleep_ns(int64_t ns);
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
// Virtual eventfds (nn::socket::EventFd): gRPC (the game's NPLN online library) needs a wakeup descriptor for its poll engine and aborts without one. They live
// entirely in the host (a counter each, no bsd service), have descriptors of their own, and are the only descriptors Poll can wait on besides the coop module's.
// Horizon's EventFdFlags: bit 0 semaphore, bit 1 non-blocking (gRPC passes 6 = non-blocking | close-on-exec)
#include <pthread.h>
#include <time.h>
enum { EVFD_BASE = 0x6000, EVFD_MAX = 256 };
static struct { uint8_t used, nonblock, sem; uint64_t cnt; } g_evfd[EVFD_MAX];
static pthread_mutex_t g_evfd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_evfd_cv = PTHREAD_COND_INITIALIZER;
static int evfd_is(int fd) { return fd >= EVFD_BASE && fd < EVFD_BASE + EVFD_MAX && g_evfd[fd - EVFD_BASE].used; }
#define SK_EAGAIN 35                    // FreeBSD
static int evfd_create(uint64_t v, int flags) {
    int r = -1;
    pthread_mutex_lock(&g_evfd_mu);
    for (int i = 0; i < EVFD_MAX; i++) if (!g_evfd[i].used) { g_evfd[i].used = 1; g_evfd[i].cnt = v; g_evfd[i].sem = flags & 1; g_evfd[i].nonblock = (flags >> 1) & 1; r = EVFD_BASE + i; break; }
    pthread_mutex_unlock(&g_evfd_mu);
    return r < 0 ? sk_fail() : r;
}
static int evfd_read(int fd, uint64_t *out) {
    if (!evfd_is(fd)) return sk_fail();
    int r = 0;
    pthread_mutex_lock(&g_evfd_mu);
    for (;;) {
        if (g_evfd[fd - EVFD_BASE].cnt) { if (g_evfd[fd - EVFD_BASE].sem) { *out = 1; g_evfd[fd - EVFD_BASE].cnt--; } else { *out = g_evfd[fd - EVFD_BASE].cnt; g_evfd[fd - EVFD_BASE].cnt = 0; } break; }
        if (g_evfd[fd - EVFD_BASE].nonblock) { t_skerr = SK_EAGAIN; r = -1; break; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_evfd_cv, &g_evfd_mu, &ts);
    }
    pthread_mutex_unlock(&g_evfd_mu);
    return r;
}
static int evfd_write(int fd, uint64_t v) {
    if (!evfd_is(fd)) return sk_fail();
    pthread_mutex_lock(&g_evfd_mu); g_evfd[fd - EVFD_BASE].cnt += v; pthread_cond_broadcast(&g_evfd_cv); pthread_mutex_unlock(&g_evfd_mu);
    return 0;
}
static int evfd_close(int fd) {
    pthread_mutex_lock(&g_evfd_mu); g_evfd[fd - EVFD_BASE].used = 0; g_evfd[fd - EVFD_BASE].cnt = 0; pthread_mutex_unlock(&g_evfd_mu);
    return 0;
}
// poll over eventfds only (negative descriptors are ignored): returns the number ready, or 0 after min(ms, 100 ms) like sk_idle
static int evfd_poll(sk_pollfd *p, uint64_t n, int ms) {
    int ready = 0;
    pthread_mutex_lock(&g_evfd_mu);
    for (int round = 0; round < 2; round++) {
        ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0 || !evfd_is(p[i].fd)) continue;
            if ((p[i].events & 1) && g_evfd[p[i].fd - EVFD_BASE].cnt) p[i].revents |= 1;   // POLLIN
            if (p[i].events & 4) p[i].revents |= 4;                                          // POLLOUT: always writable
            if (p[i].revents) ready++;
        }
        if (ready || round || ms == 0) break;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); int w = ms < 0 || ms > 100 ? 100 : ms;
        ts.tv_nsec += (long)w * 1000000; while (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_evfd_cv, &g_evfd_mu, &ts);
    }
    pthread_mutex_unlock(&g_evfd_mu);
    return ready;
}
// DM_COOP: a set whose descriptors are all virtual is waited on by the coop module (readable when a datagram waits, always writable)
int dm_sk_poll(void *fds, uint64_t n, int ms) {   // also hle_offline.c's plain pollfd form
    sk_pollfd *p = fds;
    int virt = 0;
    int ev = 0;
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) { if (evfd_is(p[i].fd)) { ev = 1; continue; } if (!mpsk_is(p[i].fd)) return sk_fail(); virt = 1; }
    if (ev && !virt) return evfd_poll(p, n, ms);
    if (virt) return (int)mpsk_ret(coop_vnet_poll(dm_coop(), (coop_pollfd *)fds, n, ms));
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    return sk_idle(ms);
}
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 seconds, i64 microseconds}
    const uint64_t *sets[3] = {r, w, e};
    int virt = 0;
    for (int k = 0; k < 3; k++) for (int fd = 0; sets[k] && fd < nfds && fd < 1024; fd++) if (sets[k][fd >> 6] >> (fd & 63) & 1) { if (!mpsk_is(fd)) return sk_fail(); virt = 1; }
    int64_t ms = tv ? tv[0] * 1000 + tv[1] / 1000 : -1;
    if (virt) return (int)mpsk_ret(coop_vnet_select(dm_coop(), nfds, (uint64_t *)r, (uint64_t *)w, (uint64_t *)e, ms > 0x7fffffff ? 0x7fffffff : (int)ms));
    return sk_idle(ms);
}
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return mpsk_socket(a, b, c); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return mpsk_bind(s, a, n); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { (void)s; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return evfd_is(s) ? evfd_close(s) : mpsk_close(s); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return mpsk_getname(s, a, n); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return mpsk_getopt(s, l, o, v, n); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return mpsk_setopt(s, l, o, v, n); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, int v) { return mpsk_fcntl(s, c, v); }   // variadic: the third argument arrives in x2
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, uint64_t n, int t) { return dm_sk_poll(f, n, t); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
SKF("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", h_sk_EventFd, uint64_t v, int f) { return evfd_create(v, f); }
SKF("_ZN2nn6socket11EventFdReadEiPm", h_sk_EventFdRead, int s, uint64_t *v) { return evfd_read(s, v); }
SKF("_ZN2nn6socket12EventFdWriteEim", h_sk_EventFdWrite, int s, uint64_t v) { return evfd_write(s, v); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "bsd socket service (cut)", int64_t, h_sk_Recv, int s, void *b, uint64_t n, int f) { return mpsk_recv(s, b, n, f); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "bsd socket service (cut)", int64_t, h_sk_RecvFrom, int s, void *b, uint64_t n, int f, void *a, unsigned *al) { return mpsk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "bsd socket service (cut)", int64_t, h_sk_Send, int s, const void *b, uint64_t n, int f) { return mpsk_send(s, b, n, f); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4;   // EAI_FAIL
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAddrInfo, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution (cut): NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, uint64_t c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return mpsk_socket(a, b, c); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return mpsk_bind(s, a, n); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketClose", h_nsk_Close, int s) { return mpsk_close(s); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return mpsk_getname(s, a, n); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return mpsk_getopt(s, l, o, v, n); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return mpsk_setopt(s, l, o, v, n); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, int v) { return mpsk_fcntl(s, c, v); }
SKF("nnsocketPoll", h_nsk_Poll, void *f, uint64_t n, int t) { return dm_sk_poll(f, n, t); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "bsd socket service (cut)", int64_t, h_nsk_Recv, int s, void *b, uint64_t n, int f) { return mpsk_recv(s, b, n, f); }
HLE_FORCE("nnsocketSend", "bsd socket service (cut)", int64_t, h_nsk_Send, int s, const void *b, uint64_t n, int f) { return mpsk_send(s, b, n, f); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution (cut): NULL", void *, h_nsk_GetHost, const char *n, const void *o, uint64_t c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl: offline, Initialize succeeds (the network library's global initialization depends on it), everything else that contacts the service fails ----------
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "same as nnsslInitialize: succeeds, creates no session (cut)", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "pairs with Initialize (cut)", Result, h_ssl_FinCpp, void) { return RES_OK; }
SSF("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context7DestroyEv", h_ssl_CtxDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) { (void)self; (void)s; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl service (cut): read returns -1", int, h_ssl_CnRead, void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl service (cut): write returns -1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
// C interface: all return nnResult
#define SSC(m, cname) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, void) { return RES_FAIL; }
SSC("nnsslConnectionCreate", h_nssl_Create) SSC("nnsslConnectionDestroy", h_nssl_Destroy)
SSC("nnsslConnectionDoHandshakeWithCertBuffer", h_nssl_Handshake) SSC("nnsslConnectionGetConnectionId", h_nssl_GetId)
SSC("nnsslConnectionGetNextAlpnProto", h_nssl_GetAlpn) SSC("nnsslConnectionGetVerifyCertError", h_nssl_VerifyErr)
SSC("nnsslConnectionPeek", h_nssl_Peek) SSC("nnsslConnectionPending", h_nssl_Pending) SSC("nnsslConnectionRead", h_nssl_Read)
SSC("nnsslConnectionSetHostName", h_nssl_SetHost) SSC("nnsslConnectionSetIoMode", h_nssl_SetIo)
SSC("nnsslConnectionSetNextAlpnProto", h_nssl_SetAlpn) SSC("nnsslConnectionSetOption", h_nssl_SetOpt)
SSC("nnsslConnectionSetSessionCacheMode", h_nssl_SetCache) SSC("nnsslConnectionSetSocketDescriptor", h_nssl_SetSock)
SSC("nnsslConnectionSetVerifyOption", h_nssl_SetVerify) SSC("nnsslConnectionWrite", h_nssl_Write)
SSC("nnsslContextDestroy", h_nssl_CtxDestroy) SSC("nnsslContextGetContextId", h_nssl_CtxGetId)

#include "hle_net.adapters.inc"
