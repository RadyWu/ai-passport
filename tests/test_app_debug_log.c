#include "app_debug_log.h"
#include "app_prompt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char buf[2048];
    size_t n;
} capture_t;

static void capture_emit(void *ctx, const char *bytes, size_t len)
{
    capture_t *c = ctx;
    assert(c->n + len < sizeof(c->buf));
    memcpy(c->buf + c->n, bytes, len);
    c->n += len;
    c->buf[c->n] = '\0';
}

static capture_t emit_pair(const char *newest, const char *older)
{
    capture_t c;
    memset(&c, 0, sizeof(c));
    app_debug_emit_pair(newest, older, capture_emit, &c);
    return c;
}

static void test_ring(void)
{
    app_debug_ring_t ring;
    const char *slot[2] = { NULL, NULL };
    const char *seq[] = { "a", "b", "c", "d" };
    int i;
    uint8_t packed;

    memset(&ring, 0, sizeof(ring));
    assert(app_debug_ring_at(&ring, 0) < 0);
    assert(app_debug_ring_at(&ring, 1) < 0);

    for (i = 0; i < 4; i++) {
        int dest = app_debug_ring_push(&ring);
        assert(dest == 0 || dest == 1);
        slot[dest] = seq[i];
    }
    /* Four pushes into two slots: newest is d, older is c. a and b are gone. */
    assert(strcmp(slot[app_debug_ring_at(&ring, 0)], "d") == 0);
    assert(strcmp(slot[app_debug_ring_at(&ring, 1)], "c") == 0);
    assert(app_debug_ring_at(&ring, 2) < 0);

    packed = app_debug_ring_pack(&ring);
    memset(&ring, 0x5a, sizeof(ring));
    app_debug_ring_unpack(packed, &ring);
    assert(strcmp(slot[app_debug_ring_at(&ring, 0)], "d") == 0);
    assert(strcmp(slot[app_debug_ring_at(&ring, 1)], "c") == 0);

    app_debug_ring_unpack(0xff, &ring);
    assert(ring.count == 0);
    assert(app_debug_ring_at(&ring, 0) < 0);
}

static void test_capture(void)
{
    char dst[64];
    size_t n;

    n = app_debug_capture(dst, sizeof(dst), "hello", false, false);
    assert(n == 5);
    assert(strcmp(dst, "hello") == 0);

    n = app_debug_capture(dst, sizeof(dst), "abc", false, true);
    assert(strcmp(dst, "abc" APP_DEBUG_TRUNC_MARK) == 0);

    n = app_debug_capture(dst, sizeof(dst), "abc", true, true);
    assert(strcmp(dst, APP_DEBUG_TRUNC_MARK "abc") == 0);

    /* 8-byte dst cannot hold the 11-byte marker; keep a UTF-8 prefix. */
    n = app_debug_capture(dst, 8, "你你你你", false, false);
    assert(n == 6);
    assert(memcmp(dst, "你你", 6) == 0);
    assert(dst[6] == '\0');

    n = app_debug_capture(dst, 16, "你你你你你你", false, false);
    assert(strcmp(dst, "你" APP_DEBUG_TRUNC_MARK) == 0);

    n = app_debug_capture(dst, 16, "你你你你你你", true, false);
    assert(strcmp(dst, APP_DEBUG_TRUNC_MARK "你") == 0);

    /* Tail keeps the topic marker when the cap drops the head. */
    {
        char big[64];
        memset(big, 'A', 40);
        memcpy(big + 40, "##题目##", 10);
        big[50] = '\0';
        n = app_debug_capture(dst, 24, big, true, false);
        assert(strstr(dst, "##题目##") != NULL);
        assert(strncmp(dst, APP_DEBUG_TRUNC_MARK, strlen(APP_DEBUG_TRUNC_MARK)) == 0);
    }
}

static void test_emit(void)
{
    capture_t c = emit_pair(NULL, "");
    assert(strcmp(c.buf, "暂无") == 0);

    c = emit_pair("only", NULL);
    assert(strcmp(c.buf, "only") == 0);
    assert(strstr(c.buf, "\n\n") == NULL);

    c = emit_pair("new", "old");
    assert(strcmp(c.buf, "new\n\nold") == 0);

    c = emit_pair("", "old");
    assert(strcmp(c.buf, "old") == 0);

    app_debug_emit_pair("x", "y", NULL, NULL);
}

static void test_prompt(void)
{
    char topics[APP_PROMPT_TOPIC_SLOTS][APP_PROMPT_TOPIC_MAX];
    char out[APP_PROMPT_SYS_MAX];
    char worst_pref[512];
    char tiny[32];
    int n;
    int i;

    memset(topics, 0, sizeof(topics));
    strcpy(topics[0], "苹果");
    strcpy(topics[1], "月亮");
    n = app_prompt_build_system(out, sizeof(out), "冷知识", topics, 42);
    assert(n > 0);
    assert(strstr(out, "偏好：冷知识。") != NULL);
    assert(strstr(out, "以下话题已写过，勿重复：苹果；月亮；种子42。") != NULL);
    assert(strstr(out, "月亮种子") == NULL);

    memset(topics, 0, sizeof(topics));
    strcpy(topics[0], "模型精度");
    n = app_prompt_build_system(out, sizeof(out), "冷知识", topics, 122463);
    assert(strstr(out, "以下话题已写过，勿重复：模型精度；种子122463。") != NULL);
    assert(strstr(out, "模型精度种子") == NULL);

    memset(topics, 0, sizeof(topics));
    strcpy(topics[0], "标题甲");
    strcpy(topics[2], "标题乙");
    strcpy(topics[4], "标题丙");
    n = app_prompt_build_system(out, sizeof(out), "冷知识", topics, 123456);
    assert(strstr(out, "以下话题已写过，勿重复：标题甲；标题乙；标题丙；种子123456。") != NULL);
    assert(strstr(out, "1500") != NULL);
    assert(strstr(out, "2000") != NULL);
    assert(strstr(out, APP_DEBUG_TRUNC_MARK) == NULL);
    assert(strstr(out, "{") == NULL);

    n = app_prompt_build_system(out, sizeof(out), "A\nB\tC", NULL, 7);
    assert(strstr(out, "偏好：ABC。种子7。") != NULL);
    assert(strstr(out, "以下话题") == NULL);
    assert(strstr(out, "；种子") == NULL);

    n = app_prompt_build_system(out, sizeof(out), "", NULL, 1);
    assert(strstr(out, "轻松有趣的冷知识或生活小故事") != NULL);

    n = app_prompt_build_system(out, sizeof(out), "引号\"斜杠\\", NULL, 3);
    assert(strstr(out, "引号\"斜杠\\") != NULL);
    assert(strstr(out, "\\\"") == NULL);

    memset(worst_pref, 'A', sizeof(worst_pref) - 1);
    worst_pref[sizeof(worst_pref) - 1] = '\0';
    memset(topics, 0, sizeof(topics));
    for (i = 0; i < APP_PROMPT_TOPIC_SLOTS; i++) memset(topics[i], 'B', APP_PROMPT_TOPIC_MAX - 1);
    n = app_prompt_build_system(out, sizeof(out), worst_pref, topics, 4294967295u);
    assert(n > 0);
    assert(n < APP_PROMPT_SYS_MAX);
    assert(strstr(out, APP_DEBUG_TRUNC_MARK) == NULL);
    assert(strstr(out, "；种子4294967295。") != NULL);
    assert(strstr(out, worst_pref) != NULL);

    n = app_prompt_build_system(tiny, sizeof(tiny), "冷知识", NULL, 1);
    assert(n > 0);
    assert(n < (int)sizeof(tiny));
    assert(strstr(tiny, APP_DEBUG_TRUNC_MARK) != NULL);
    assert(app_prompt_build_system(NULL, 8, "x", NULL, 1) < 0);
}

int main(void)
{
    test_ring();
    test_capture();
    test_emit();
    test_prompt();
    puts("test_app_debug_log: PASS");
    return 0;
}
