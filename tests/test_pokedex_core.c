// tests/test_pokedex_core.c —— 宿主机单测:pokedex_core 状态机/序列化/格式化。
// 由 tools/validate.sh 用系统 cc 编译运行,不依赖 ESP-IDF。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pokedex_core.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_init_and_bitmap(void)
{
    pokedex_state_t st;
    pokedex_state_init(&st);

    CHECK(st.magic == POKEDEX_STATE_MAGIC);
    CHECK(st.last_id == POKEDEX_DEX_FIRST);
    CHECK(st.save_seq == 0);
    CHECK(pokedex_count_caught(&st) == 0);
    CHECK(pokedex_count_seen(&st) == 0);
    CHECK(!pokedex_is_caught(&st, 1));
    CHECK(!pokedex_is_caught(&st, 151));

    pokedex_set_caught(&st, 1, true);
    pokedex_set_caught(&st, 151, true);
    pokedex_set_caught(&st, 25, true);
    CHECK(pokedex_is_caught(&st, 1));
    CHECK(pokedex_is_caught(&st, 151));
    CHECK(pokedex_is_caught(&st, 25));
    CHECK(pokedex_count_caught(&st) == 3);

    pokedex_set_caught(&st, 25, false);
    CHECK(!pokedex_is_caught(&st, 25));
    CHECK(pokedex_count_caught(&st) == 2);

    pokedex_mark_seen(&st, 1);
    pokedex_mark_seen(&st, 2);
    CHECK(pokedex_is_seen(&st, 1));
    CHECK(pokedex_is_seen(&st, 2));
    CHECK(pokedex_count_seen(&st) == 2);

    /* 越界 id 必须被忽略,不得写坏内存。 */
    pokedex_set_caught(&st, 0, true);
    pokedex_set_caught(&st, 152, true);
    pokedex_set_caught(&st, 0xFFFFFFFFu, true);
    CHECK(pokedex_count_caught(&st) == 2);
}

static void test_step(void)
{
    CHECK(pokedex_step(1, -1) == POKEDEX_DEX_LAST);
    CHECK(pokedex_step(POKEDEX_DEX_LAST, 1) == POKEDEX_DEX_FIRST);
    CHECK(pokedex_step(1, 1) == 2);
    CHECK(pokedex_step(150, 1) == 151);
    CHECK(pokedex_step(50, -1) == 49);
    CHECK(pokedex_step(0, 1) == 2);   /* 越界输入按 FIRST 处理后步进 */
    CHECK(pokedex_step(999, -1) == POKEDEX_DEX_LAST);
}

static void test_serialize_roundtrip(void)
{
    pokedex_state_t st;
    pokedex_state_init(&st);
    st.last_id = 42;
    st.save_seq = 7;
    pokedex_set_caught(&st, 1, true);
    pokedex_set_caught(&st, 150, true);

    uint8_t blob[POKEDEX_STATE_BLOB_SIZE + 8];
    memset(blob, 0xAA, sizeof(blob));
    size_t n = pokedex_state_serialize(&st, blob, sizeof(blob));
    CHECK(n == POKEDEX_STATE_BLOB_SIZE);

    /* 黄金向量:固定布局,偏移即契约。magic=0x50445831 在小端内存中是 "1XDP"。 */
    CHECK(memcmp(blob, "1XDP", 4) == 0);
    CHECK(blob[4] == 42 && blob[5] == 0 && blob[6] == 0 && blob[7] == 0);
    CHECK(blob[8] == 0x01);         /* caught bit0 -> 字节偏移 8 */
    CHECK(blob[26] == 0x20);        /* caught bit149(id=150) -> 字节 8+18,bit5 */
    CHECK(memcmp(blob + 48, "\x07\x00\x00\x00", 4) == 0);
    CHECK(blob[52] == 0xAA);        /* 不越界写 */

    pokedex_state_t back;
    memset(&back, 0, sizeof(back));
    CHECK(pokedex_state_deserialize(&back, blob, n));
    CHECK(back.magic == POKEDEX_STATE_MAGIC);
    CHECK(back.last_id == 42);
    CHECK(back.save_seq == 7);
    CHECK(pokedex_is_caught(&back, 1));
    CHECK(pokedex_is_caught(&back, 150));
    CHECK(!pokedex_is_caught(&back, 2));
}

static void test_serialize_invalid(void)
{
    pokedex_state_t st;

    uint8_t blob[POKEDEX_STATE_BLOB_SIZE];
    pokedex_state_init(&st);
    st.last_id = 3;
    pokedex_state_serialize(&st, blob, sizeof(blob));

    /* 长度不足。 */
    CHECK(!pokedex_state_deserialize(&st, blob, POKEDEX_STATE_BLOB_SIZE - 1));
    /* 坏 magic。 */
    blob[0] = 'X';
    CHECK(!pokedex_state_deserialize(&st, blob, sizeof(blob)));
    blob[0] = 'P';
    blob[1] = 'D';
    blob[2] = 'X';
    blob[3] = 1;
    /* 越界 last_id。 */
    blob[4] = 0;
    blob[5] = 0;
    blob[6] = 0;
    blob[7] = 0;
    CHECK(!pokedex_state_deserialize(&st, blob, sizeof(blob)));
    blob[4] = 152;
    CHECK(!pokedex_state_deserialize(&st, blob, sizeof(blob)));
    /* 失败调用不得改动目标。 */
    CHECK(st.last_id == 3);
}

static void test_urls(void)
{
    char buf[192];

    size_t n = pokedex_api_url(25, buf, sizeof(buf));
    CHECK(strcmp(buf, "https://pokeapi.co/api/v2/pokemon/25") == 0);
    CHECK(n == strlen("https://pokeapi.co/api/v2/pokemon/25"));

    n = pokedex_api_url(151, buf, sizeof(buf));
    CHECK(strstr(buf, "/pokemon/151") != NULL);

    n = pokedex_sprite_url(25, buf, sizeof(buf));
    CHECK(strstr(buf, "/sprites/pokemon/25.png") != NULL);

    /* 过小缓冲:返回 0,不写溢出。 */
    CHECK(pokedex_api_url(25, buf, 0) == 0);
    CHECK(pokedex_api_url(25, buf, 2) == 0);
    CHECK(pokedex_sprite_url(25, buf, 8) == 0);
    CHECK(pokedex_api_url(25, NULL, 10) == 0);

    /* 恰好容纳完整字符串的缓冲(含 NUL)。 */
    size_t need = strlen("https://pokeapi.co/api/v2/pokemon/25") + 1;
    CHECK(pokedex_api_url(25, buf, need) > 0);
    CHECK(pokedex_api_url(25, buf, need - 1) == 0);
}

static void test_formatting(void)
{
    char buf[64];

    CHECK(pokedex_format_number(25, buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "#025") == 0);
    CHECK(strcmp(buf, "#001") != 0);
    pokedex_format_number(1, buf, sizeof(buf));
    CHECK(strcmp(buf, "#001") == 0);

    pokedex_format_height(4, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.4 m") == 0);
    pokedex_format_height(10, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.0 m") == 0);
    pokedex_format_height(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.0 m") == 0);
    pokedex_format_height(-7, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.0 m") == 0);

    pokedex_format_weight(60, buf, sizeof(buf));
    CHECK(strcmp(buf, "6.0 kg") == 0);
    pokedex_format_weight(25, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.5 kg") == 0);
    pokedex_format_weight(1000, buf, sizeof(buf));
    CHECK(strcmp(buf, "100.0 kg") == 0);

    CHECK(pokedex_pretty_name("pikachu", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "Pikachu") == 0);
    CHECK(pokedex_pretty_name("mr-mime", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "Mr Mime") == 0);
    CHECK(pokedex_pretty_name("nidoran-f", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "Nidoran F") == 0);
    CHECK(pokedex_pretty_name("MEOWTH", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "Meowth") == 0);
    CHECK(pokedex_pretty_name("farfetchd", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "Farfetchd") == 0);

    /* 截断:仍以 NUL 结尾,不溢出。 */
    CHECK(pokedex_pretty_name("pikachu", buf, 4) > 0);
    CHECK(buf[3] == '\0');
}

static void test_fit_scaled(void)
{
    uint32_t dw = 0, dh = 0;

    /* 已满足上限:原样输出。 */
    CHECK(pokedex_fit_scaled(96, 96, 96, 96, &dw, &dh));
    CHECK(dw == 96 && dh == 96);
    CHECK(pokedex_fit_scaled(32, 64, 96, 96, &dw, &dh));
    CHECK(dw == 32 && dh == 64);

    /* 大图等比缩到 ≤96。 */
    CHECK(pokedex_fit_scaled(475, 475, 96, 96, &dw, &dh));
    CHECK(dw <= 96 && dh <= 96);
    CHECK(dw == dh);
    CHECK(pokedex_fit_scaled(200, 100, 96, 96, &dw, &dh));
    CHECK(dw == 96 && dh == 48);
    CHECK(pokedex_fit_scaled(100, 200, 96, 96, &dw, &dh));
    CHECK(dw == 48 && dh == 96);

    /* 非法输入。 */
    CHECK(!pokedex_fit_scaled(0, 10, 96, 96, &dw, &dh));
    CHECK(!pokedex_fit_scaled(10, 10, 0, 96, &dw, &dh));
    CHECK(!pokedex_fit_scaled(10, 10, 96, 96, NULL, &dh));
    CHECK(!pokedex_fit_scaled(10, 10, 96, 96, &dw, NULL));

    /* 巨大输入也不产生 0 尺寸。 */
    CHECK(pokedex_fit_scaled(4096, 4096, 96, 96, &dw, &dh));
    CHECK(dw >= 1 && dh >= 1 && dw <= 96 && dh <= 96);
}

int main(void)
{
    test_init_and_bitmap();
    test_step();
    test_serialize_roundtrip();
    test_serialize_invalid();
    test_urls();
    test_formatting();
    test_fit_scaled();

    if (s_failures) {
        fprintf(stderr, "pokedex_core: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_core: all checks passed\n");
    return 0;
}