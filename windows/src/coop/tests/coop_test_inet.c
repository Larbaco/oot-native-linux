// coop unit tests, step 1d (MIT): internet play (docs/coop-step1d.md). Included by coop_test.c, after its helpers. Sections:
// - crypto: SHA-256 against FIPS 180-4's examples, HMAC-SHA256 against RFC 4231's seven cases, HKDF-SHA256 against RFC 5869 A.1-A.3; room codes, keys,
//   tags and the replay window;
// - inet: configuration; hosts and stations with the room code over UDP and over the control links; a wrong code; LAN and internet nodes refusing
//   each other; a proxy that changes, repeats or drops one frame; replayed and changed datagrams; the failure budget;
// - relay: tools/coop-relay in this process, a host and one then three visitors through it (two and four players), one leaving; a wrong code;
//   the relay's limits (rooms, visitors, new links, lookups, the first message, waits, age, bytes, rate);
// - probe (only when named): one node from DM_COOP* for latency over a real path (an SSH tunnel), as docs/coop-step1d.md measured.
#include "coop_relay.h"

static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; }
static void unhex(const char *h, uint8_t *o) {
    for (size_t i = 0; h[2 * i]; i++) o[i] = (uint8_t)(hexd(h[2 * i]) << 4 | hexd(h[2 * i + 1]));
}
static int hex_is(const uint8_t *b, size_t n, const char *hex) {
    char s[2 * 128 + 1];
    if (n > 128 || strlen(hex) != 2 * n) return 0;
    for (size_t i = 0; i < n; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
    return !memcmp(s, hex, 2 * n);
}

static void test_crypto(void) {
    fprintf(stderr, "== crypto: SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 4231), HKDF-SHA256 (RFC 5869), room codes, tags, the replay window\n");
    int f0 = g_fail, nsha = 0, nhmac = 0, nhkdf = 0, okv = 0;
    uint8_t d[128];
    // SHA-256: FIPS 180-4's examples (NIST's SHA256.pdf, "abc" and the 448-bit message; the empty and 896-bit messages of the same set)
    static const struct { const char *m, *h; } sv[] = {
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    for (size_t i = 0; i < sizeof sv / sizeof sv[0]; i++) {
        csha256(sv[i].m, strlen(sv[i].m), d); int ok = hex_is(d, 32, sv[i].h); CHECK(ok); nsha++; okv += ok;
        csha s; csha_init(&s); for (size_t j = 0; j < strlen(sv[i].m); j++) csha_update(&s, sv[i].m + j, 1); csha_final(&s, d);   // a byte at a time
        CHECK(hex_is(d, 32, sv[i].h));
    }
    { csha s; static char a[1000]; memset(a, 'a', sizeof a); csha_init(&s); for (int i = 0; i < 1000; i++) csha_update(&s, a, sizeof a); csha_final(&s, d);
      int ok = hex_is(d, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); CHECK(ok); nsha++; okv += ok; }   // one million 'a'
    // the padding's edges, cross-checked with Python's hashlib (not published vectors): 55, 56, 63, 64, 65, 119, 120 and 128 'a'
    static const struct { int n; const char *h; } pv[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"}, {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"}, {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"}, {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"}, {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"}};
    for (size_t i = 0; i < sizeof pv / sizeof pv[0]; i++) { char a[128]; memset(a, 'a', sizeof a); csha256(a, (size_t)pv[i].n, d); CHECK(hex_is(d, 32, pv[i].h)); }
    // HMAC-SHA256: RFC 4231 test cases 1-7 (case 5 compares the first 128 bits, as the RFC does)
    uint8_t k131[131], d50[50], c50[50], k25[25];
    memset(k131, 0xaa, sizeof k131); memset(d50, 0xdd, sizeof d50); memset(c50, 0xcd, sizeof c50); for (int i = 0; i < 25; i++) k25[i] = (uint8_t)(i + 1);
    uint8_t k0b[20], kaa[20], k0c[20]; memset(k0b, 0x0b, 20); memset(kaa, 0xaa, 20); memset(k0c, 0x0c, 20);
    static const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First",
                      *m7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    const struct { const uint8_t *k; size_t kn; const void *m; size_t mn; const char *h; size_t hn; } hv[] = {
        {k0b, 20, "Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32},
        {(const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32},
        {kaa, 20, d50, 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32},
        {k25, 25, c50, 50, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 32},
        {k0c, 20, "Test With Truncation", 20, "a3b6167473100ee06e0c796c2955552b", 16},
        {k131, 131, m6, strlen(m6), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32},
        {k131, 131, m7, strlen(m7), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", 32},
    };
    for (size_t i = 0; i < sizeof hv / sizeof hv[0]; i++) {
        chmac_once(hv[i].k, hv[i].kn, hv[i].m, hv[i].mn, d); int ok = hex_is(d, hv[i].hn, hv[i].h); CHECK(ok); nhmac++; okv += ok;
        chmac k; csha s; chmac_key(&k, hv[i].k, hv[i].kn);   // the same through a prepared key, the message in two pieces
        chmac_start(&k, &s); csha_update(&s, hv[i].m, hv[i].mn / 2); csha_update(&s, (const uint8_t *)hv[i].m + hv[i].mn / 2, hv[i].mn - hv[i].mn / 2); chmac_end(&k, &s, d);
        CHECK(hex_is(d, hv[i].hn, hv[i].h));
    }
    // HKDF-SHA256: RFC 5869 appendix A.1-A.3 (PRK and OKM)
    { uint8_t ikm[80], salt[80], info[80], prk[32], okm[82];
      memset(ikm, 0x0b, 22); unhex("000102030405060708090a0b0c", salt); unhex("f0f1f2f3f4f5f6f7f8f9", info);
      chkdf_extract(salt, 13, ikm, 22, prk); CHECK(hex_is(prk, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
      CHECK_EQ(chkdf_expand(prk, info, 10, okm, 42), 0);
      int ok = hex_is(okm, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"); CHECK(ok); nhkdf++; okv += ok;
      for (int i = 0; i < 80; i++) { ikm[i] = (uint8_t)i; salt[i] = (uint8_t)(0x60 + i); info[i] = (uint8_t)(0xb0 + i); }
      chkdf_extract(salt, 80, ikm, 80, prk); CHECK(hex_is(prk, 32, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"));
      CHECK_EQ(chkdf_expand(prk, info, 80, okm, 82), 0);
      ok = hex_is(okm, 82, "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87");
      CHECK(ok); nhkdf++; okv += ok;
      memset(ikm, 0x0b, 22);
      chkdf_extract(NULL, 0, ikm, 22, prk); CHECK(hex_is(prk, 32, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"));
      CHECK_EQ(chkdf_expand(prk, NULL, 0, okm, 42), 0);
      ok = hex_is(okm, 42, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"); CHECK(ok); nhkdf++; okv += ok;
      static uint8_t big[255 * 32 + 1];
      CHECK_EQ(chkdf_expand(prk, NULL, 0, big, 255 * 32), 0); CHECK_EQ(chkdf_expand(prk, NULL, 0, big, 255 * 32 + 1), -1); }
    CHECK_EQ(cct_eq("abcdef", "abcdef", 6), 1); CHECK_EQ(cct_eq("abcdef", "abcdeg", 6), 0); CHECK_EQ(cct_eq("xbcdef", "abcdef", 6), 0);
    fprintf(stderr, "   published vectors: SHA-256 %d, HMAC-SHA256 %d, HKDF-SHA256 %d; %d of %d match\n", nsha, nhmac, nhkdf, okv, nsha + nhmac + nhkdf);

    // room codes: Crockford base32, 16 symbols (80 bits), shown XXXX-XXXX-XXXX-XXXX
    char r[COOP_ROOM_LEN + 1], shown[COOP_ROOM_SHOWN];
    CHECK_EQ(coop_room_normalize("7K3M-9QXA-2B4C-HJ6N", r), 1); CHECK(!strcmp(r, "7K3M9QXA2B4CHJ6N"));
    CHECK_EQ(coop_room_normalize("7k3m 9qxa 2b4c hj6n", r), 1); CHECK(!strcmp(r, "7K3M9QXA2B4CHJ6N"));
    CHECK_EQ(coop_room_normalize("oOiI-lL00-1111-ZZZZ", r), 1); CHECK(!strcmp(r, "001111001111ZZZZ"));   // O as 0, I and L as 1
    CHECK_EQ(coop_room_normalize("7K3M-9QXA-2B4C-HJ6", r), 0);    // 15 symbols
    CHECK_EQ(coop_room_normalize("7K3M-9QXA-2B4C-HJ6NN", r), 0);  // 17
    CHECK_EQ(coop_room_normalize("7K3M-9QXA-2B4C-HJ6U", r), 0);   // U is not a symbol
    CHECK_EQ(coop_room_normalize("7K3M-9QXA-2B4C-HJ6*", r), 0);
    CHECK_EQ(coop_room_normalize("", r), 0); CHECK_EQ(coop_room_normalize(NULL, r), 0);
    coop_room_format("7K3M9QXA2B4CHJ6N", shown); CHECK(!strcmp(shown, "7K3M-9QXA-2B4C-HJ6N"));
    { int seen[32] = {0}, dup = 0, bad = 0; static char many[1000][COOP_ROOM_LEN + 1];
      for (int i = 0; i < 1000; i++) {
          coop_room_new(many[i]); char back[COOP_ROOM_LEN + 1]; bad += !coop_room_normalize(many[i], back) || strcmp(back, many[i]);
          for (int j = 0; j < COOP_ROOM_LEN; j++) { const char *a = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; seen[strchr(a, many[i][j]) - a]++; }
          for (int j = 0; j < i; j++) dup += !strcmp(many[i], many[j]);
      }
      int lo = 16000, hi = 0; for (int i = 0; i < 32; i++) { if (seen[i] < lo) lo = seen[i]; if (seen[i] > hi) hi = seen[i]; }
      CHECK_EQ(bad, 0); CHECK_EQ(dup, 0); CHECK(lo > 350 && hi < 650);   // 500 expected per symbol
      fprintf(stderr, "   1000 new room codes: all valid, all different; each symbol drawn %d-%d times of 16000 (500 expected)\n", lo, hi); }
    // keys: the same code the same keys; another code others; the relay's room id is not the auth key
    { uint8_t ka[32], ia[16], kb[32], ib[16]; croom_keys("7K3M9QXA2B4CHJ6N", ka, ia); croom_keys("7K3M9QXA2B4CHJ6N", kb, ib);
      CHECK(!memcmp(ka, kb, 32) && !memcmp(ia, ib, 16));
      croom_keys("7K3M9QXA2B4CHJ6P", kb, ib); CHECK(memcmp(ka, kb, 32) && memcmp(ia, ib, 16)); CHECK(memcmp(ka, ia, 16));
      // proofs: each side's differs, and binds both nonces and the HELLO
      uint8_t ns[16] = {1}, nh[16] = {2}, hl[17] = {1, 0x5A}, p1[32], p2[32];
      cauth_proof(ka, 0, ns, nh, hl, 17, p1); cauth_proof(ka, 1, ns, nh, NULL, 0, p2); CHECK(memcmp(p1, p2, 32));
      cauth_proof(ka, 0, nh, ns, hl, 17, p2); CHECK(memcmp(p1, p2, 32));
      hl[3] ^= 1; cauth_proof(ka, 0, ns, nh, hl, 17, p2); CHECK(memcmp(p1, p2, 32));
      // link keys: the station's sending keys are the host's receiving keys, and each direction and path differs
      static conn hc, sc; memset(&hc, 0, sizeof hc); memset(&sc, 0, sizeof sc);
      cauth_link_keys(&hc, ka, ns, nh, 1); cauth_link_keys(&sc, ka, ns, nh, 0);
      uint8_t hdr[5] = {9, 0, 0, 0, M_PING}, t1[CAUTH_TAG], t2[CAUTH_TAG];
      cauth_frame_tag(&sc.ktx, 0, hdr, NULL, 0, t1); cauth_frame_tag(&hc.krx, 0, hdr, NULL, 0, t2); CHECK(!memcmp(t1, t2, CAUTH_TAG));
      cauth_frame_tag(&hc.ktx, 0, hdr, NULL, 0, t2); CHECK(memcmp(t1, t2, CAUTH_TAG));         // the other direction
      cauth_frame_tag(&sc.ktx, 1, hdr, NULL, 0, t2); CHECK(memcmp(t1, t2, CAUTH_TAG));         // the next frame
      cauth_udp_tag(&sc.kutx, hdr, 5, t2); CHECK(memcmp(t1, t2, CAUTH_TAG));                   // the datagram key
      uint8_t body[40]; memset(body, 7, sizeof body); hdr[0] = 41 + CAUTH_TAG;
      cauth_frame_tag(&sc.ktx, 3, hdr, body, 40, t1);
      for (int i = 0; i < 40; i++) { body[i] ^= 0x10; cauth_frame_tag(&hc.krx, 3, hdr, body, 40, t2); CHECK(memcmp(t1, t2, CAUTH_TAG)); body[i] ^= 0x10; }
      cauth_frame_tag(&hc.krx, 3, hdr, body, 40, t2); CHECK(!memcmp(t1, t2, CAUTH_TAG)); }
    // the replay window: 64 below the highest number seen
    { uint64_t mx = 0, w = 0;
      CHECK_EQ(cauth_replay_ok(&mx, &w, 0), 0);
      CHECK_EQ(cauth_replay_ok(&mx, &w, 1), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 1), 0);
      CHECK_EQ(cauth_replay_ok(&mx, &w, 5), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 3), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 3), 0); CHECK_EQ(cauth_replay_ok(&mx, &w, 4), 1);
      CHECK_EQ(cauth_replay_ok(&mx, &w, 2), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 5), 0);
      CHECK_EQ(cauth_replay_ok(&mx, &w, 100), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 36), 0); CHECK_EQ(cauth_replay_ok(&mx, &w, 37), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 37), 0);
      CHECK_EQ(cauth_replay_ok(&mx, &w, 99), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 1000), 1); CHECK_EQ(cauth_replay_ok(&mx, &w, 100), 0); CHECK_EQ(cauth_replay_ok(&mx, &w, 999), 1); }
    if (g_fail != f0) fprintf(stderr, "   crypto: %d checks failed\n", g_fail - f0);
}

// ---------- internet play over loopback ----------
#define ROOM "7K3M-9QXA-2B4C-HJ6N"
#define ROOM_WRONG "7K3M-9QXA-2B4C-HJ6P"
static coop_node *mk_inet(const char *coop, const char *uid, const char *room, const char *relay, tnode *t, int tcp_only) {
    coop_cfg c; char err[240] = "";
    int r = coop_cfg_parse_ex(&c, coop, uid, "1", room, relay, err, sizeof err);
    CHECK_EQ(r, 1);
    if (r != 1) fprintf(stderr, "   %s\n", err);
    c.tcp_only = tcp_only; c.hooks = hooks(t); c.bind_loopback = 1; c.discovery = 0;
    return coop_create(&c);
}
static void test_inet_config(void) {
    coop_cfg c; char err[240];
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8:47600", NULL, NULL, ROOM, NULL, err, sizeof err), -1);   // internet play off: as step 1c
    CHECK(strstr(err, "DM_COOP_INTERNET") != NULL);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8:47600", NULL, "0", ROOM, NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8:47600", NULL, "1", NULL, NULL, err, sizeof err), -1);    // a visitor needs the code
    CHECK(strstr(err, "room code") != NULL);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8:47600", NULL, "1", ROOM, NULL, err, sizeof err), 1);
    CHECK_EQ(c.auth, 1); CHECK_EQ(c.scope, COOP_SCOPE_INTERNET); CHECK(!strcmp(c.room, "7K3M9QXA2B4CHJ6N")); CHECK_EQ(c.room_new, 0); CHECK_EQ(c.addr, 0x08080808u);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:10.20.1.20", NULL, "1", ROOM, NULL, err, sizeof err), 1); CHECK_EQ(c.scope, COOP_SCOPE_INTERNET); CHECK_EQ(c.auth, 1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:127.0.0.1:5", NULL, "on", ROOM, NULL, err, sizeof err), 1); CHECK_EQ(c.scope, COOP_SCOPE_LOOPBACK); CHECK_EQ(c.auth, 1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:100.64.0.1:1", NULL, "1", ROOM, NULL, err, sizeof err), 1);   // carrier NAT: an address like any other here
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:0.0.0.0:1", NULL, "1", ROOM, NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:224.0.0.251:1", NULL, "1", ROOM, NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:255.255.255.255:1", NULL, "1", ROOM, NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8", NULL, "1", "7K3M-9QXA-2B4C-HJ6", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8", NULL, "1", "7K3M-9QXA-2B4C-HJ6U", NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", NULL, NULL, err, sizeof err), 1);   // a host without a code gets a new one
    CHECK_EQ(c.room_new, 1); CHECK_EQ(c.scope, COOP_SCOPE_INTERNET); CHECK_EQ(c.discovery, 1);
    { char back[COOP_ROOM_LEN + 1]; CHECK_EQ(coop_room_normalize(c.room, back), 1); }
    CHECK_EQ(coop_cfg_parse_ex(&c, "host:0", NULL, "1", NULL, NULL, err, sizeof err), 1); CHECK_EQ(c.auth, 1); CHECK_EQ(c.scope, COOP_SCOPE_LOOPBACK); CHECK_EQ(c.room_new, 1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", ROOM, "127.0.0.1", err, sizeof err), 1);
    CHECK_EQ(c.relay_ip, 0x7F000001u); CHECK_EQ(c.relay_port, COOP_RELAY_PORT_DEFAULT); CHECK_EQ(c.discovery, 0);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", ROOM, "203.0.113.7:5000", err, sizeof err), 1); CHECK_EQ(c.relay_ip, 0xCB007107u); CHECK_EQ(c.relay_port, 5000);
    CHECK_EQ(coop_cfg_parse_ex(&c, "join:8.8.8.8", NULL, "1", ROOM, "127.0.0.1", err, sizeof err), -1);   // a relay needs auto
    CHECK_EQ(coop_cfg_parse_ex(&c, "host:0", NULL, "1", ROOM, "127.0.0.1", err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", ROOM, "300.1.1.1", err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", ROOM, "0.0.0.0", err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", ROOM, "1.2.3.4:0", err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "1", ROOM, "relay.example", err, sizeof err), -1);   // IPv4 only
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "yes", ROOM, NULL, err, sizeof err), -1);
    CHECK_EQ(coop_cfg_parse_ex(&c, "auto", NULL, "0", ROOM, "127.0.0.1", err, sizeof err), 1);   // off: the code and the relay are ignored
    CHECK_EQ(c.auth, 0); CHECK_EQ(c.relay_ip, 0); CHECK_EQ(c.room[0], 0); CHECK_EQ(c.scope, COOP_SCOPE_LAN);
    CHECK_EQ(coop_cfg_parse_ex(&c, "off", NULL, "1", ROOM, NULL, err, sizeof err), 0);
    CHECK_EQ(coop_cfg_parse(&c, "auto", NULL, err, sizeof err), 1); CHECK_EQ(c.auth, 0); CHECK_EQ(c.room[0], 0); CHECK_EQ(c.relay_ip, 0);
    for (int cls = 0; cls <= COOP_ADDR_INVALID; cls++) CHECK_EQ(coop_addr_allowed(COOP_SCOPE_INTERNET, cls), cls != COOP_ADDR_INVALID);
}

// A man in the middle for one link: it copies the station's bytes to the host unchanged, and the host's frames back, changing, repeating or
// dropping frame number k (counted from the host's first frame: 0 AUTH_CHAL, 1 AUTH_OK, 2 WELCOME, then whatever follows)
enum { PX_PASS, PX_FLIP, PX_DUP, PX_DROP };
typedef struct { csock l; uint16_t port, to; int mode, k; atomic_int run, done; cthread th; } proxy;
static void proxy_main(void *arg) {
    proxy *p = arg; csock a = CSOCK_BAD, b = CSOCK_BAD;
    int64_t u = now_ms() + 8000;
    while (a == CSOCK_BAD && now_ms() < u && atomic_load(&p->run)) { uint32_t ip; uint16_t pt; a = cplat_tcp_accept(p->l, &ip, &pt); if (a == CSOCK_BAD) msleep(2); }
    cplat_close(p->l); p->l = CSOCK_BAD;   // one link only
    if (a != CSOCK_BAD) b = raw_connect(p->to);
    static uint8_t hb[0x10000]; size_t hn = 0; int frames = 0;
    while (a != CSOCK_BAD && b != CSOCK_BAD && atomic_load(&p->run)) {
        cpollfd f[2] = {{a, CP_IN, 0}, {b, CP_IN, 0}};
        cplat_poll(f, 2, 10);
        uint8_t buf[4096]; int64_t k;
        if (f[0].got) { k = cplat_recv(a, buf, sizeof buf); if (k == 0 || k < -1) break; if (k > 0) raw_send(b, buf, (size_t)k); }
        if (f[1].got) {
            k = cplat_recv(b, hb + hn, sizeof hb - hn); if (k == 0 || k < -1) break;
            if (k > 0) hn += (size_t)k;
            while (hn >= 4 && hn >= 4 + (size_t)get32(hb)) {
                size_t fl = 4 + get32(hb);
                if (frames++ == p->k && p->mode != PX_PASS) {
                    if (p->mode == PX_FLIP) { hb[5 + (fl - 5) / 2] ^= 0x40; raw_send(a, hb, fl); }
                    else if (p->mode == PX_DUP) { raw_send(a, hb, fl); raw_send(a, hb, fl); }
                } else raw_send(a, hb, fl);
                memmove(hb, hb + fl, hn - fl); hn -= fl;
            }
        }
    }
    cplat_close(a); cplat_close(b);
    atomic_store(&p->done, 1);
}
static void proxy_open(proxy *p, uint16_t to, int mode, int k) {
    memset(p, 0, sizeof *p); p->to = to; p->mode = mode; p->k = k;
    p->l = cplat_tcp_listen(0x7F000001u, 0, &p->port); CHECK(p->l != CSOCK_BAD);
    atomic_store(&p->run, 1); cplat_thread_start(&p->th, proxy_main, p);
}
static void proxy_close(proxy *p) { atomic_store(&p->run, 0); cplat_thread_join(p->th); if (p->l != CSOCK_BAD) cplat_close(p->l); }

static coop_node *inet_host(const char *coop, const char *relay, tnode *t, int maxn, int *hs) {
    coop_node *H = mk_inet(coop, "000000000000000100000000000000aa", ROOM, relay, t, 0);
    uint8_t cfg[0x20], sec[0x44], uh[0x30]; mk_netcfg(cfg, maxn, 301); mk_sec(sec, 3); mk_user(uh, "Host");
    CHECK_EQ(coop_ldn_initialize(H), 0); *hs = game_socket(H);
    CHECK_EQ(coop_ldn_open_ap(H), 0); CHECK_EQ(coop_ldn_create_network(H, cfg, sec, uh), 0);
    return H;
}
static int inet_join(coop_node *S, uint8_t *scan, const char *name, int *fd, uint32_t *ip) {   // scan, connect; 0 joined
    uint8_t sec[0x44], u[0x30], f[0x60]; mk_sec(sec, 3); mk_user(u, name); mk_filter(f, 0x21, 0);
    CHECK_EQ(coop_ldn_initialize(S), 0); *fd = game_socket(S); CHECK_EQ(coop_ldn_open_station(S), 0);
    int cnt = scan_until(S, scan, f, 1, 6000);
    CHECK_EQ(cnt, 1);
    if (cnt != 1) return -1;
    uint32_t r = coop_ldn_connect(S, scan, sec, u, 301, 0), mask;
    CHECK_EQ(r, 0);
    coop_ldn_ipv4(S, ip, &mask);
    return r ? -1 : 0;
}
static int dg(coop_node *from, int ffd, coop_node *to, int tfd, uint32_t dst, uint32_t src, const char *msg) {   // one datagram arrives intact from src
    char b[256]; uint8_t fr[16];
    if (send_to(from, ffd, dst, msg) != (int64_t)strlen(msg) + 1) return 0;
    int64_t k = recv_wait(to, tfd, b, sizeof b, fr, 3000);
    return k == (int64_t)strlen(msg) + 1 && !strcmp(b, msg) && sa_ip(fr) == src;
}

static void inet_session(int tcp_only) {
    fprintf(stderr, "   a host and two stations with the room code, game datagrams over %s\n", tcp_only ? "the control links" : "UDP");
    tnode th = {.name = "ihost"}, t1 = {.name = "ista1"}, t2 = {.name = "ista2"};
    int hs, s1, s2; uint32_t hip = 0, ip1 = 0, ip2 = 0, mask;
    coop_node *H = inet_host("host:0", NULL, &th, 4, &hs);
    uint16_t port = coop_port(H); CHECK(port != 0);
    poll_start(H);
    coop_ldn_ipv4(H, &hip, &mask);
    char join[64]; snprintf(join, sizeof join, "join:127.0.0.1:%u", port);
    uint8_t *scan = malloc(0x480 * 24);
    coop_node *S1 = mk_inet(join, "0000000000000001000000000000000b", ROOM, NULL, &t1, tcp_only), *S2 = mk_inet(join, "0000000000000001000000000000000c", ROOM, NULL, &t2, tcp_only);
    CHECK_EQ(inet_join(S1, scan, "Sta1", &s1, &ip1), 0);
    CHECK_EQ(inet_join(S2, scan, "Sta2", &s2, &ip2), 0);
    coop_stats st;
    if (!tcp_only) { WAIT_UNTIL((coop_get_stats(H, &st), st.udp_links == 2), 3000); CHECK_EQ(st.udp_links, 2); }
    CHECK(dg(S1, s1, H, hs, hip, ip1, "s1->host")); CHECK(dg(H, hs, S1, s1, ip1, hip, "host->s1"));
    CHECK(dg(S1, s1, S2, s2, ip2, ip1, "s1->s2")); CHECK(dg(S2, s2, S1, s1, ip1, ip2, "s2->s1"));
    CHECK(dg(H, hs, S2, s2, hip | 0xFF, hip, "host bcast"));
    coop_get_stats(H, &st); CHECK_EQ(st.auth_ok, 2); CHECK_EQ(st.auth_fail, 0); CHECK_EQ(st.bad_tag, 0); CHECK_EQ(st.replays, 0);
    if (tcp_only) { CHECK_EQ(st.udp_rx, 0); CHECK(st.tcp_rx >= 3); } else CHECK(st.udp_rx >= 3);
    coop_get_stats(S1, &st); CHECK_EQ(st.auth_ok, 1); CHECK_EQ(st.bad_tag, 0);
    if (!tcp_only) {   // replays and changes, made white-box: a datagram with S1's own key and next number, sent from S1's own socket
        coop_stats h0, h1; uint8_t pkt[128]; size_t n; csock us; uint32_t uip; uint16_t upt;
        cnode_lock(S1);
        peer *up = cmp_up(S1); conn *c = &up->c;
        put32(pkt, MP_UMAGIC3); pkt[4] = U_DGRAM; pkt[5] = pkt[6] = pkt[7] = 0; put64(pkt + 8, S1->token); put64(pkt + 16, ++c->useq_tx);
        uint8_t *b = pkt + MP_UDP_HDR3; put32(b, ip1); put32(b + 4, hip); put16(b + 8, PORT); put16(b + 10, PORT); memcpy(b + MP_DG_HDR, "replay?", 8);
        n = MP_UDP_HDR3 + MP_DG_HDR + 8; cauth_udp_tag(&c->kutx, pkt, n, pkt + n); n += CAUTH_TAG;
        us = S1->susock; uip = up->ip; upt = up->udp_port;
        cnode_unlock(S1);
        char rb[64]; uint8_t fr[16];
        cplat_sendto(us, pkt, n, uip, upt);
        CHECK_EQ(recv_wait(H, hs, rb, sizeof rb, fr, 2000), 8); CHECK(!strcmp(rb, "replay?"));
        coop_get_stats(H, &h0);
        cplat_sendto(us, pkt, n, uip, upt);                       // the same bytes again
        msleep(150); CHECK_EQ(recv_wait(H, hs, rb, sizeof rb, fr, 0), -COOP_EAGAIN);
        coop_get_stats(H, &h1); CHECK_EQ(h1.replays - h0.replays, 1);
        pkt[MP_UDP_HDR3 + MP_DG_HDR + 2] ^= 1;                    // a changed byte
        cplat_sendto(us, pkt, n, uip, upt);
        msleep(150); CHECK_EQ(recv_wait(H, hs, rb, sizeof rb, fr, 0), -COOP_EAGAIN);
        coop_get_stats(H, &h0); CHECK_EQ(h0.bad_tag - h1.bad_tag, 1);
        pkt[MP_UDP_HDR3 + MP_DG_HDR + 2] ^= 1; put64(pkt + 16, 1);  // an old number (the UHELLO's) with a tag that no longer fits
        cplat_sendto(us, pkt, n, uip, upt);
        msleep(150); coop_get_stats(H, &h1); CHECK_EQ(h1.bad_tag - h0.bad_tag, 1);
        CHECK(dg(S1, s1, H, hs, hip, ip1, "still fine"));
        fprintf(stderr, "   datagrams: a repeat refused (replays %llu), a changed one and a renumbered one refused (bad tags %llu); the link goes on\n",
                (unsigned long long)h1.replays, (unsigned long long)h1.bad_tag);
    }
    poll_stop();
    coop_destroy(S2); coop_destroy(S1); coop_destroy(H); free(scan);
}

static void test_inet(void) {
    fprintf(stderr, "== inet: internet play's room-code handshake, tags and limits (loopback)\n");
    test_inet_config();
    inet_session(0);
    inet_session(1);
    uint8_t *scan = malloc(0x480 * 24); uint8_t f[0x60]; mk_filter(f, 0x21, 0);
    tnode th = {.name = "ihost"}, tw = {.name = "iwrong"}, tl = {.name = "ilan"};
    int hs; coop_stats st;
    coop_node *H = inet_host("host:0", NULL, &th, 4, &hs);
    uint16_t port = coop_port(H);
    poll_start(H);
    char join[64]; snprintf(join, sizeof join, "join:127.0.0.1:%u", port);
    // a wrong code: refused; the station says so once and waits 30 s before trying again (one failure, not one a second)
    { coop_node *W = mk_inet(join, "0000000000000001000000000000000e", ROOM_WRONG, NULL, &tw, 0);
      CHECK_EQ(coop_ldn_initialize(W), 0); CHECK_EQ(coop_ldn_open_station(W), 0);
      int cnt = scan_until(W, scan, f, 1, 2500); CHECK_EQ(cnt, 0);
      msleep(1500);
      coop_get_stats(H, &st); CHECK_EQ(st.auth_fail, 1); CHECK_EQ(st.auth_ok, 0); CHECK_EQ(st.links, 0);
      coop_get_stats(W, &st); CHECK_EQ(st.auth_fail, 1); CHECK_EQ(st.links, 0);
      coop_destroy(W); }
    // step 1c's LAN node against this internet host, and an internet node against a LAN host: each refuses the other's protocol
    { coop_node *L = mk_node(join, "0000000000000001000000000000000f", &tl, 0);
      CHECK_EQ(coop_ldn_initialize(L), 0); CHECK_EQ(coop_ldn_open_station(L), 0);
      int cnt = scan_until(L, scan, f, 1, 1500); CHECK_EQ(cnt, 0);
      cnode_lock(L); CHECK_EQ(L->pr[0].compat, -2); cnode_unlock(L);
      coop_destroy(L);
      tnode tlh = {.name = "lanhost"}, ti = {.name = "inet"};
      coop_node *LH = mk_node("host:0", "000000000000000100000000000000ab", &tlh, 0);
      uint8_t cfg[0x20], sec[0x44], uh[0x30]; mk_netcfg(cfg, 4, 301); mk_sec(sec, 3); mk_user(uh, "Host");
      CHECK_EQ(coop_ldn_initialize(LH), 0); CHECK_EQ(coop_ldn_open_ap(LH), 0); CHECK_EQ(coop_ldn_create_network(LH, cfg, sec, uh), 0);
      char j2[64]; snprintf(j2, sizeof j2, "join:127.0.0.1:%u", coop_port(LH));
      coop_node *I = mk_inet(j2, "0000000000000001000000000000001f", ROOM, NULL, &ti, 0);
      CHECK_EQ(coop_ldn_initialize(I), 0); CHECK_EQ(coop_ldn_open_station(I), 0);
      cnt = scan_until(I, scan, f, 1, 2000); CHECK_EQ(cnt, 0);
      cnode_lock(I); CHECK_EQ(I->pr[0].compat, -2); cnode_unlock(I);
      coop_destroy(I); coop_destroy(LH); }
    // a man in the middle changes, repeats or drops one frame from the host: the station closes the link, nothing reaches the game
    { static const struct { int mode, k; const char *what; } px[] = {
          {PX_FLIP, 1, "AUTH_OK changed (the host's proof)"}, {PX_FLIP, 2, "WELCOME changed"}, {PX_DROP, 2, "WELCOME dropped"}, {PX_DUP, 3, "the first answer repeated"},
          {PX_PASS, 0, "nothing changed"}};
      for (size_t i = 0; i < sizeof px / sizeof px[0]; i++) {
          proxy p; proxy_open(&p, port, px[i].mode, px[i].k);
          tnode tp = {.name = "ivia"}; char jp[64]; snprintf(jp, sizeof jp, "join:127.0.0.1:%u", p.port);
          coop_node *P = mk_inet(jp, "0000000000000001000000000000002f", ROOM, NULL, &tp, 0);
          CHECK_EQ(coop_ldn_initialize(P), 0); CHECK_EQ(coop_ldn_open_station(P), 0);
          int cnt = 0;
          for (int r = 0; r < 4; r++) { int c1 = 0; coop_ldn_scan(P, scan, &c1, 24, f, 0); cnt += c1; if (px[i].mode == PX_PASS && cnt) break; msleep(300); }
          coop_stats ps; WAIT_UNTIL((coop_get_stats(P, &ps), atomic_load(&p.done) || ps.links), 3000);
          msleep(1300);   // a dropped frame shows when the next one (a PING) arrives
          coop_get_stats(P, &ps);
          if (px[i].mode == PX_PASS) { CHECK(cnt >= 1); CHECK_EQ(ps.links, 1); CHECK_EQ(ps.bad_tag, 0); }
          else if (px[i].k == 1) { CHECK_EQ(ps.auth_fail, 1); CHECK_EQ(ps.links, 0); CHECK_EQ(cnt, 0); }
          else { CHECK(ps.bad_tag >= 1); CHECK_EQ(ps.links, 0); }
          if (px[i].mode == PX_DUP) CHECK(cnt <= 1); else if (px[i].mode != PX_PASS) CHECK_EQ(cnt, 0);
          fprintf(stderr, "   proxy, %s: station links %d, bad tags %llu, failed proofs %llu, networks listed %d\n", px[i].what, ps.links, (unsigned long long)ps.bad_tag,
                  (unsigned long long)ps.auth_fail, cnt);
          coop_destroy(P); proxy_close(&p);
      } }
    poll_stop(); coop_destroy(H);
    // the failure budget: six wrong proofs a minute from one address; then links from it close before the challenge; one more after 10 s
    { tnode tb = {.name = "ibudget"}; int hb;
      coop_node *B = inet_host("host:0", NULL, &tb, 4, &hb);
      uint16_t bp = coop_port(B);
      int chal = 0, fail = 0, shut = 0;
      for (int i = 0; i < 9; i++) {
          csock r = raw_connect(bp); uint8_t h[MP_HELLO3_LEN] = {0}, body[256]; size_t bl;
          put32(h, MP_MAGIC); put16(h + 4, MP_PROTO_INET); h[6] = 1; memset(h + 7, 0x5A, 16); memset(h + MP_HELLO_LEN, i, CAUTH_NONCE);
          raw_frame(r, M_HELLO, h, sizeof h);
          int ty = raw_read(r, body, sizeof body, &bl, 2000);
          if (ty == M_AUTH_CHAL) { chal++; uint8_t pr[32] = {0}; raw_frame(r, M_AUTH_PROOF, pr, 32); if (raw_read(r, body, sizeof body, &bl, 2000) == M_AUTH_FAIL) fail++; }
          else if (ty == -1) shut++;
          cplat_close(r);
      }
      CHECK_EQ(chal, 6); CHECK_EQ(fail, 6); CHECK_EQ(shut, 3);
      coop_get_stats(B, &st); CHECK_EQ(st.auth_fail, 9); CHECK_EQ(st.links, 0);
      fprintf(stderr, "   failure budget: 9 wrong proofs from one address, %d challenged and refused, %d closed before the challenge\n", chal, shut);
      msleep(10500);   // a minute's budget refills at one a 10 s
      poll_start(B);
      tnode tok = {.name = "iafter"}; int so; uint32_t ipo;
      char jb[64]; snprintf(jb, sizeof jb, "join:127.0.0.1:%u", bp);
      coop_node *O = mk_inet(jb, "0000000000000001000000000000003f", ROOM, NULL, &tok, 0);
      CHECK_EQ(inet_join(O, scan, "After", &so, &ipo), 0);
      coop_get_stats(B, &st); CHECK_EQ(st.auth_ok, 1);
      poll_stop(); coop_destroy(O); coop_destroy(B); }
    free(scan);
}

// ---------- the relay, in this process ----------
static cmutex g_rlk = CMUTEX_INIT;
static coop_relay *g_rly; static atomic_int g_rly_run; static cthread g_rly_th;
static void rly_log(void *u, const char *l) { (void)u; if (g_verbose) fprintf(stderr, "  [relay] %s\n", l); }
static void rly_loop(void *a) { (void)a; while (atomic_load(&g_rly_run)) { cplat_lock(&g_rlk); coop_relay_step(g_rly, 5); cplat_unlock(&g_rlk); msleep(1); } }
static uint16_t rly_start(coop_relay_cfg *c) {
    char err[200]; c->ip = 0x7F000001u; c->port = 0; c->log = rly_log;   // loopback only, a port the system picks
    g_rly = coop_relay_create(c, err, sizeof err); CHECK(g_rly != NULL);
    if (!g_rly) { fprintf(stderr, "   relay: %s\n", err); return 0; }
    atomic_store(&g_rly_run, 1); cplat_thread_start(&g_rly_th, rly_loop, NULL);
    return coop_relay_port(g_rly);
}
static void rly_stop(void) { atomic_store(&g_rly_run, 0); cplat_thread_join(g_rly_th); coop_relay_destroy(g_rly); g_rly = NULL; }
static coop_relay_stats rly_stats(void) { coop_relay_stats s; cplat_lock(&g_rlk); coop_relay_get_stats(g_rly, &s); cplat_unlock(&g_rlk); return s; }
static csock rly_raw(uint16_t port, int kind, const uint8_t room[16], uint32_t id, uint32_t cookie) {
    csock s = raw_connect(port);
    if (s == CSOCK_BAD) return s;
    uint8_t h[RLY_HELLO_LEN] = {0}; put32(h, RLY_MAGIC); h[4] = (uint8_t)kind; h[5] = 1; memcpy(h + 8, room, 16); put32(h + 24, id); put32(h + 28, cookie);
    raw_send(s, h, sizeof h);
    return s;
}
static int rly_msg(csock s, uint8_t m[RLY_MSG_LEN], int ms) {   // the next 16-byte message's type; -1 closed, -2 nothing in time
    size_t got = 0; int64_t u = now_ms() + ms;
    while (got < RLY_MSG_LEN && now_ms() < u) {
        cpollfd p = {s, CP_IN, 0}; cplat_poll(&p, 1, 10);
        if (!p.got) continue;
        int64_t k = cplat_recv(s, m + got, RLY_MSG_LEN - got);
        if (k == 0 || k < -1) return -1;
        if (k > 0) got += (size_t)k;
    }
    return got == RLY_MSG_LEN ? m[4] : -2;
}
static int raw_recv_n(csock s, uint8_t *b, size_t n, int ms) {   // n bytes, or how many arrived before the end or the time
    size_t got = 0; int64_t u = now_ms() + ms;
    while (got < n && now_ms() < u) {
        cpollfd p = {s, CP_IN, 0}; cplat_poll(&p, 1, 10);
        if (!p.got) continue;
        int64_t k = cplat_recv(s, b + got, n - got);
        if (k == 0 || k < -1) break;
        if (k > 0) got += (size_t)k;
    }
    return (int)got;
}

static void relay_session(void) {
    fprintf(stderr, "   a host and visitors through the relay: two players, then four, one leaving\n");
    coop_relay_cfg rc; coop_relay_defaults(&rc);
    uint16_t rp = rly_start(&rc);
    char rel[32]; snprintf(rel, sizeof rel, "127.0.0.1:%u", rp);
    tnode th = {.name = "rhost"}, tv[3] = {{.name = "rvis1"}, {.name = "rvis2"}, {.name = "rvis3"}}, tw = {.name = "rwrong"};
    int hs, fd[3]; uint32_t hip = 0, ip[3] = {0}, mask; coop_stats st;
    coop_node *H = inet_host("auto", rel, &th, 4, &hs);
    CHECK_EQ(coop_port(H), 0);   // nothing listens on the host's device
    poll_start(H);
    coop_ldn_ipv4(H, &hip, &mask);
    WAIT_UNTIL((coop_get_stats(H, &st), st.relay_up), 3000); CHECK_EQ(st.relay_up, 1);
    coop_relay_stats rs = rly_stats(); CHECK_EQ(rs.rooms, 1);
    // a node that only initialized (the Link Play menu open, nothing chosen yet) does not ask the relay for a room
    { tnode ti = {.name = "ridle"}; coop_node *I = mk_inet("auto", "0000000000000001000000000000006e", ROOM, rel, &ti, 0);
      coop_relay_stats r0 = rly_stats(); CHECK_EQ(coop_ldn_initialize(I), 0); msleep(700);
      coop_relay_stats r1 = rly_stats(); CHECK_EQ(r1.links, r0.links); CHECK_EQ(r1.waiting + r1.paired, r0.waiting + r0.paired);
      coop_destroy(I); }
    uint8_t *scan = malloc(0x480 * 24);
    static const char *uids[3] = {"0000000000000001000000000000004b", "0000000000000001000000000000004c", "0000000000000001000000000000004d"};
    coop_node *V[3];
    V[0] = mk_inet("auto", uids[0], ROOM, rel, &tv[0], 0);
    int64_t t0 = now_ms();
    CHECK_EQ(inet_join(V[0], scan, "Vis1", &fd[0], &ip[0]), 0);
    fprintf(stderr, "   visitor 1 joined through the relay %lld ms after it started\n", (long long)(now_ms() - t0));
    CHECK(dg(V[0], fd[0], H, hs, hip, ip[0], "v1->host")); CHECK(dg(H, hs, V[0], fd[0], ip[0], hip, "host->v1"));
    rs = rly_stats(); CHECK_EQ(rs.paired, 2); CHECK_EQ(rs.visitors_paired, 1);
    for (int i = 1; i < 3; i++) { V[i] = mk_inet("auto", uids[i], ROOM, rel, &tv[i], 0); char nm[8]; snprintf(nm, sizeof nm, "Vis%d", i + 1); CHECK_EQ(inet_join(V[i], scan, nm, &fd[i], &ip[i]), 0); }
    uint8_t info[0x480];
    WAIT_UNTIL(coop_ldn_network_info(H, info) == 0 && NI_COUNT(info) == 4, 3000); CHECK_EQ(NI_COUNT(info), 4);
    WAIT_UNTIL(coop_ldn_network_info(V[0], info) == 0 && NI_COUNT(info) == 4, 3000); CHECK_EQ(NI_COUNT(info), 4);
    // visitors see each other through the host (the game's own station-to-station traffic)
    CHECK(dg(V[0], fd[0], V[1], fd[1], ip[1], ip[0], "v1->v2")); CHECK(dg(V[2], fd[2], V[0], fd[0], ip[0], ip[2], "v3->v1"));
    CHECK(dg(V[1], fd[1], V[2], fd[2], ip[2], ip[1], "v2->v3")); CHECK(dg(H, hs, V[2], fd[2], ip[2], hip, "host->v3"));
    rs = rly_stats(); CHECK_EQ(rs.paired, 6); CHECK_EQ(rs.rooms, 1);
    coop_get_stats(H, &st); CHECK_EQ(st.auth_ok, 3); CHECK_EQ(st.udp_links, 0); CHECK_EQ(st.bad_tag, 0);   // through a relay datagrams use the links
    // one visitor leaves; the others play on
    CHECK_EQ(coop_ldn_disconnect(V[1]), 0); coop_destroy(V[1]); V[1] = NULL;
    WAIT_UNTIL(coop_ldn_network_info(H, info) == 0 && NI_COUNT(info) == 3, 3000); CHECK_EQ(NI_COUNT(info), 3);
    CHECK(dg(V[0], fd[0], V[2], fd[2], ip[2], ip[0], "v1->v3 after")); CHECK(dg(V[2], fd[2], H, hs, hip, ip[2], "v3->host after"));
    WAIT_UNTIL((rs = rly_stats(), rs.paired == 4), 3000); CHECK_EQ(rs.paired, 4);
    // a wrong code asks the relay for a room that does not exist: the host never hears of it
    { coop_node *W = mk_inet("auto", "0000000000000001000000000000005e", ROOM_WRONG, rel, &tw, 0);
      uint8_t f[0x60]; mk_filter(f, 0x21, 0);
      CHECK_EQ(coop_ldn_initialize(W), 0); CHECK_EQ(coop_ldn_open_station(W), 0);
      CHECK_EQ(scan_until(W, scan, f, 1, 2500), 0);
      rs = rly_stats(); CHECK(rs.not_found >= 1);
      coop_get_stats(H, &st); CHECK_EQ(st.auth_fail, 0);
      coop_destroy(W); }
    // the right room through the relay but no code: the host refuses the proof (counted, for every link through the relay together)
    { uint8_t ka[32], id[16]; char nrm[COOP_ROOM_LEN + 1]; coop_room_normalize(ROOM, nrm); croom_keys(nrm, ka, id);
      csock r = rly_raw(rp, RLY_JOIN, id, 0, 0); uint8_t m[RLY_MSG_LEN];
      CHECK_EQ(rly_msg(r, m, 3000), RLY_OK);
      uint8_t h[MP_HELLO3_LEN] = {0}, body[256]; size_t bl; put32(h, MP_MAGIC); put16(h + 4, MP_PROTO_INET); h[6] = 1;
      raw_frame(r, M_HELLO, h, sizeof h);
      CHECK_EQ(raw_read(r, body, sizeof body, &bl, 3000), M_AUTH_CHAL);
      uint8_t pr[32] = {0}; raw_frame(r, M_AUTH_PROOF, pr, 32);
      CHECK_EQ(raw_read(r, body, sizeof body, &bl, 3000), M_AUTH_FAIL);
      cplat_close(r);
      coop_get_stats(H, &st); CHECK_EQ(st.auth_fail, 1);
      cwipe(ka, sizeof ka); }
    poll_stop();
    coop_destroy(V[2]); coop_destroy(V[0]);
    coop_destroy(H);   // the room closes with its host
    WAIT_UNTIL((rs = rly_stats(), rs.rooms == 0 && rs.links == 0), 3000); CHECK_EQ(rs.rooms, 0); CHECK_EQ(rs.links, 0);
    fprintf(stderr, "   relay: %llu rooms, %llu visitors paired, %llu bytes copied, %llu lookups not found\n", (unsigned long long)rs.rooms_opened,
            (unsigned long long)rs.visitors_paired, (unsigned long long)rs.bytes, (unsigned long long)rs.not_found);
    rly_stop(); free(scan);
}

static void relay_limits(void) {
    fprintf(stderr, "   the relay's limits\n");
    coop_relay_cfg rc; coop_relay_defaults(&rc);
    rc.max_rooms = 1; rc.max_peers = 2; rc.hello_ms = 300; rc.wait_ms = 400; rc.nofind_per_min = 3; rc.new_per_ip = 10;
    uint16_t rp = rly_start(&rc);
    uint8_t X[16], Y[16], m[RLY_MSG_LEN]; memset(X, 0x11, 16); memset(Y, 0x22, 16);
    csock A = rly_raw(rp, RLY_HOST, X, 0, 0); CHECK_EQ(rly_msg(A, m, 2000), RLY_OK);
    csock B = rly_raw(rp, RLY_HOST, Y, 0, 0); CHECK_EQ(rly_msg(B, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_LIMIT); cplat_close(B);    // rooms
    csock C = rly_raw(rp, RLY_HOST, X, 0, 0); CHECK_EQ(rly_msg(C, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_TAKEN); cplat_close(C);    // a held room
    csock V1 = rly_raw(rp, RLY_JOIN, X, 0, 0), V2 = rly_raw(rp, RLY_JOIN, X, 0, 0);
    uint32_t id1, ck1;
    CHECK_EQ(rly_msg(A, m, 2000), RLY_VISITOR); id1 = get32(m + 8); ck1 = get32(m + 12);
    CHECK_EQ(rly_msg(A, m, 2000), RLY_VISITOR);
    csock V3 = rly_raw(rp, RLY_JOIN, X, 0, 0); CHECK_EQ(rly_msg(V3, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_FULL); cplat_close(V3);  // visitors
    csock T = rly_raw(rp, RLY_ATTACH, X, id1, ck1 ^ 1); CHECK_EQ(rly_msg(T, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_NOROOM); cplat_close(T);   // a wrong cookie
    // the host never dials in: both visitors give up after wait_ms, and the host hears they left
    CHECK_EQ(rly_msg(V1, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_TIMEOUT);
    CHECK_EQ(rly_msg(V2, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_TIMEOUT);
    cplat_close(V1); cplat_close(V2);
    CHECK_EQ(rly_msg(A, m, 2000), RLY_GONE);
    // lookups for rooms that are not there: three a minute, then "limit" (one went on the cookie above)
    int noroom = 0, limit = 0;
    for (int i = 0; i < 4; i++) { csock s = rly_raw(rp, RLY_JOIN, Y, 0, 0); if (rly_msg(s, m, 2000) == RLY_ERR) { noroom += m[5] == RLY_E_NOROOM; limit += m[5] == RLY_E_LIMIT; } cplat_close(s); }
    CHECK_EQ(noroom, 2); CHECK_EQ(limit, 2);
    // the first message: nothing in time, or not ours
    coop_relay_stats rs0 = rly_stats();
    csock Q = raw_connect(rp); CHECK_EQ(rly_msg(Q, m, 1500), -1); cplat_close(Q);
    uint8_t junk[RLY_HELLO_LEN]; memset(junk, 0x41, sizeof junk);
    Q = raw_connect(rp); raw_send(Q, junk, sizeof junk); CHECK_EQ(rly_msg(Q, m, 1500), RLY_ERR); CHECK_EQ(m[5], RLY_E_BAD); cplat_close(Q);
    coop_relay_stats rs = rly_stats(); CHECK_EQ(rs.bad - rs0.bad, 2);
    // new links a second from one address: ten, bursts of twenty
    { csock s[40]; uint64_t r0 = rly_stats().refused;
      for (int i = 0; i < 40; i++) s[i] = raw_connect(rp);
      msleep(100); rs = rly_stats();
      for (int i = 0; i < 40; i++) cplat_close(s[i]);
      CHECK(rs.refused - r0 >= 15);
      fprintf(stderr, "   40 links at once from one address: %llu refused at once\n", (unsigned long long)(rs.refused - r0)); }
    cplat_close(A);
    rly_stop();
    // a room's age and bytes, a link's rate: a raw host and visitor paired through the relay
    for (int part = 0; part < 3; part++) {
        coop_relay_defaults(&rc);
        if (part == 0) rc.room_ms = 500;
        if (part == 1) rc.room_bytes = 64 << 10;
        if (part == 2) rc.link_bps = 32 << 10;
        rp = rly_start(&rc);
        A = rly_raw(rp, RLY_HOST, X, 0, 0); CHECK_EQ(rly_msg(A, m, 2000), RLY_OK);
        if (part == 0) {
            CHECK_EQ(rly_msg(A, m, 3000), RLY_ERR); CHECK_EQ(m[5], RLY_E_EXPIRED);
            rs = rly_stats(); CHECK_EQ(rs.expired, 1); CHECK_EQ(rs.rooms, 0);
            cplat_close(A); rly_stop(); continue;
        }
        csock V = rly_raw(rp, RLY_JOIN, X, 0, 0);
        CHECK_EQ(rly_msg(A, m, 2000), RLY_VISITOR);
        csock D = rly_raw(rp, RLY_ATTACH, X, get32(m + 8), get32(m + 12));
        CHECK_EQ(rly_msg(V, m, 2000), RLY_OK);
        static uint8_t big[96 << 10], back[96 << 10];
        size_t n = part == 1 ? sizeof big : 64 << 10;
        for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + 3);
        int64_t t0 = now_ms();
        raw_send(V, big, n);
        int got = raw_recv_n(D, back, n, 4000);
        int64_t ms = now_ms() - t0;
        if (part == 1) {   // 64 KiB a room: closed partway
            CHECK(got < (int)sizeof big);
            rs = rly_stats(); CHECK_EQ(rs.expired, 1); CHECK_EQ(rs.rooms, 0);
            CHECK_EQ(rly_msg(A, m, 2000), RLY_ERR); CHECK_EQ(m[5], RLY_E_EXPIRED);
            fprintf(stderr, "   a 64 KiB room: closed after %d of %d bytes\n", got, (int)sizeof big);
        } else {           // 32 KiB a second, starting with one second's worth: 64 KiB take about a second, intact
            CHECK_EQ(got, (int)n); CHECK(!memcmp(big, back, n)); CHECK(ms >= 700);
            fprintf(stderr, "   a link at 32 KiB/s: 64 KiB arrived intact in %lld ms\n", (long long)ms);
        }
        cplat_close(V); cplat_close(D); cplat_close(A);
        rly_stop();
    }
}
static void test_relay(void) {
    fprintf(stderr, "== relay: tools/coop-relay in this process (loopback only)\n");
    relay_session();
    relay_limits();
}

// ---------- probe (only when named): one node from the same environment the games read (DM_COOP, DM_COOP_INTERNET, DM_COOP_ROOM, DM_COOP_RELAY,
// DM_COOP_TCP, DM_UID), for latency over real paths (docs/coop-step1d.md). COOP_PROBE=host echoes every game datagram back for COOP_PROBE_SECS
// (120); COOP_PROBE=join scans until it finds the network (60 s at most), joins, sends COOP_PROBE_N (500) datagrams 20 ms apart and reports the
// round trips. Each side's log goes to stderr with a time stamp ----------
static int64_t g_probe_t0;
static void probe_log(void *u, const char *l) { (void)u; fprintf(stderr, "  [%7.3f] %s\n", (now_ms() - g_probe_t0) / 1000.0, l); }
static int cmp_i64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static void test_probe(void) {
    const char *role = getenv("COOP_PROBE"), *secs = getenv("COOP_PROBE_SECS"), *cnt = getenv("COOP_PROBE_N");
    g_probe_t0 = now_ms();
    if (!role || (strcmp(role, "host") && strcmp(role, "join"))) { fprintf(stderr, "probe: COOP_PROBE=host or join\n"); g_fail++; return; }
    fprintf(stderr, "== probe: %s\n", role);
    static const coop_hooks h = {NULL, probe_log, NULL, NULL, NULL, NULL};
    coop_node *n = coop_env_node(&h);
    if (!n) { fprintf(stderr, "probe: DM_COOP gives no node\n"); g_fail++; return; }
    uint8_t cfg[0x20], sec[0x44], u[0x30], f[0x60], info[0x480]; mk_netcfg(cfg, 4, 301); mk_sec(sec, 3); mk_filter(f, 0x21, 0);
    char b[256]; uint8_t fr[16]; uint32_t fl = 16;
    if (!strcmp(role, "host")) {
        mk_user(u, "ProbeHost");
        CHECK_EQ(coop_ldn_initialize(n), 0); int fd = game_socket(n);
        CHECK_EQ(coop_ldn_open_ap(n), 0); CHECK_EQ(coop_ldn_create_network(n, cfg, sec, u), 0);
        int64_t until = now_ms() + 1000ll * (secs ? atoi(secs) : 120); uint64_t echoed = 0; int last_nodes = 1;
        while (now_ms() < until) {
            coop_ldn_network_info(n, info);   // the game reads it every frame: it releases ACCEPTs
            if (NI_COUNT(info) != last_nodes) { last_nodes = NI_COUNT(info); fprintf(stderr, "  [%7.3f] probe: %d node(s)\n", (now_ms() - g_probe_t0) / 1000.0, last_nodes); }
            int64_t k; fl = 16;
            while ((k = coop_vnet_recvfrom(n, fd, b, sizeof b, 0x80, fr, &fl)) > 0) { coop_vnet_sendto(n, fd, b, (uint64_t)k, 0, fr, 16); echoed++; fl = 16; }
            msleep(2);
        }
        coop_stats st; coop_get_stats(n, &st);
        fprintf(stderr, "probe host: echoed %llu datagrams; udp %llu/%llu tcp %llu/%llu (tx/rx); room code proven by %llu, failed %llu; bad tags %llu, replays %llu\n",
                (unsigned long long)echoed, (unsigned long long)st.udp_tx, (unsigned long long)st.udp_rx, (unsigned long long)st.tcp_tx, (unsigned long long)st.tcp_rx,
                (unsigned long long)st.auth_ok, (unsigned long long)st.auth_fail, (unsigned long long)st.bad_tag, (unsigned long long)st.replays);
    } else {
        mk_user(u, "ProbeVisitor");
        uint8_t *scan = malloc(0x480 * 24); int c = 0;
        int64_t t0 = now_ms();
        CHECK_EQ(coop_ldn_initialize(n), 0); int fd = game_socket(n); CHECK_EQ(coop_ldn_open_station(n), 0);
        while (now_ms() - t0 < 60000) { c = 0; coop_ldn_scan(n, scan, &c, 24, f, 0); if (c) break; msleep(200); }
        int64_t t_scan = now_ms() - t0;
        CHECK_EQ(c, 1);
        if (c != 1) { free(scan); return; }
        uint32_t r = coop_ldn_connect(n, scan, sec, u, 301, 0), ip, mask;
        int64_t t_join = now_ms() - t0;
        CHECK_EQ(r, 0);
        coop_ldn_ipv4(n, &ip, &mask);
        uint32_t hip = (ip & 0xFFFFFF00u) | 1;
        int N = cnt ? atoi(cnt) : 500, got = 0; int64_t *rtt = calloc((size_t)N, sizeof *rtt);
        msleep(500);   // the UDP endpoint is proven right after ACCEPT
        for (int i = 0; i < N; i++) {
            char m[64]; snprintf(m, sizeof m, "probe %d %lld", i, (long long)now_ms());
            send_to(n, fd, hip, m);
            int64_t k; fl = 16;
            while ((k = coop_vnet_recvfrom(n, fd, b, sizeof b, 0x80, fr, &fl)) > 0) {
                int seq; long long ts; if (sscanf(b, "probe %d %lld", &seq, &ts) == 2 && got < N) rtt[got++] = now_ms() - ts;
                fl = 16;
            }
            msleep(20);
        }
        int64_t u2 = now_ms() + 2000;
        while (got < N && now_ms() < u2) { int64_t k; fl = 16; if ((k = coop_vnet_recvfrom(n, fd, b, sizeof b, 0x80, fr, &fl)) > 0) { int seq; long long ts; if (sscanf(b, "probe %d %lld", &seq, &ts) == 2) rtt[got++] = now_ms() - ts; } else msleep(2); }
        qsort(rtt, (size_t)got, sizeof *rtt, cmp_i64);
        coop_stats st; coop_get_stats(n, &st);
        fprintf(stderr, "probe join: found the network after %lld ms, joined after %lld ms; %d of %d echoes back; round trip min %lld, median %lld, p95 %lld, max %lld ms; "
                        "path %s; room code proven %llu, failed %llu; bad tags %llu, replays %llu\n", (long long)t_scan, (long long)t_join, got, N,
                (long long)(got ? rtt[0] : -1), (long long)(got ? rtt[got / 2] : -1), (long long)(got ? rtt[got * 95 / 100] : -1), (long long)(got ? rtt[got - 1] : -1),
                st.udp_links ? "UDP" : "the control link (TCP)", (unsigned long long)st.auth_ok, (unsigned long long)st.auth_fail, (unsigned long long)st.bad_tag,
                (unsigned long long)st.replays);
        CHECK(got >= N * 9 / 10);
        coop_ldn_disconnect(n);
        free(rtt); free(scan);
    }
    coop_ldn_finalize(n);
}
