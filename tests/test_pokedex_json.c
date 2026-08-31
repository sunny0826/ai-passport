// tests/test_pokedex_json.c —— 宿主机单测:流式 JSON 提取器。
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

/* 与 pokeapi 结构一致的小文档(不做排序假设,故意把 types/sprites 放中间)。 */
static const char DOC[] =
    "{"
    "\"abilities\":[{\"ability\":{\"name\":\"static\"}}],"
    "\"id\":25,"
    "\"name\":\"pikachu\","
    "\"height\":4,"
    "\"moves\":[{\"name\":\"growl\",\"desc\":\"say \\\"hi\\\"\"},"
    "{\"name\":\"thunder-shock\",\"url\":\"\\u0075rl\"}],"
    "\"species\":{\"name\":\"pikachu-species\",\"url\":\"x\"},"
    "\"sprites\":{\"front_default\":\"https://x/25.png\",\"back\":\"b\"},"
    "\"types\":[{\"slot\":1,\"type\":{\"name\":\"electric\"}},"
    "{\"slot\":2,\"type\":{\"name\":\"flying\"}}],"
    "\"stats\":[{\"base\":35}],"
    "\"weight\":60"
    "}";

static void feed_all(pokedex_json_t *j, const uint8_t *data, size_t len, size_t chunk)
{
    size_t i = 0;
    while (i < len) {
        size_t n = len - i;
        if (n > chunk) n = chunk;
        pokedex_json_feed(j, data + i, n);
        i += n;
    }
}

static void verify_pikachu(const pokedex_entry_t *e)
{
    CHECK(e->id == 25);
    CHECK(strcmp(e->name, "pikachu") == 0);
    CHECK(e->height_dm == 4);
    CHECK(e->weight_hg == 60);
    CHECK(e->type_count == 2);
    CHECK(strcmp(e->types[0], "electric") == 0);
    CHECK(strcmp(e->types[1], "flying") == 0);
    CHECK(strcmp(e->sprite_url, "https://x/25.png") == 0);
}

static void test_doc_single_chunk(void)
{
    pokedex_entry_t e;
    pokedex_json_t j;
    pokedex_json_init(&j, &e);
    memcpy(&e, (pokedex_entry_t[1]){{0}}, sizeof(e));

    CHECK(pokedex_json_feed(&j, (const uint8_t *)DOC, strlen(DOC)));
    CHECK(!pokedex_json_broken(&j));
    CHECK(pokedex_json_done(&j));
    verify_pikachu(&e);
}

static void test_doc_chunked(void)
{
    /* 任意分块(3 字节)也要得到同样结果。 */
    pokedex_entry_t e;
    pokedex_json_t j;
    pokedex_json_init(&j, &e);
    memset(&e, 0, sizeof(e));

    feed_all(&j, (const uint8_t *)DOC, strlen(DOC), 3);
    CHECK(!pokedex_json_broken(&j));
    CHECK(pokedex_json_done(&j));
    verify_pikachu(&e);
}

static void test_more_than_two_types(void)
{
    const char *doc =
        "{\"id\":7,\"name\":\"x\",\"height\":1,\"weight\":1,"
        "\"types\":[{\"type\":{\"name\":\"a\"}},{\"type\":{\"name\":\"b\"}},"
        "{\"type\":{\"name\":\"c\"}}],"
        "\"sprites\":{\"front_default\":\"s\"}}";
    pokedex_entry_t e;
    pokedex_json_t j;
    pokedex_json_init(&j, &e);
    memset(&e, 0, sizeof(e));
    CHECK(pokedex_json_feed(&j, (const uint8_t *)doc, strlen(doc)));
    CHECK(pokedex_json_done(&j));
    CHECK(e.type_count == 2);
    CHECK(strcmp(e.types[0], "a") == 0);
    CHECK(strcmp(e.types[1], "b") == 0);
}

static void test_incomplete_and_broken(void)
{
    pokedex_entry_t e;
    pokedex_json_t j;
    pokedex_json_init(&j, &e);
    memset(&e, 0, sizeof(e));
    /* 残缺文档:不 done 也不 broken。 */
    pokedex_json_feed(&j, (const uint8_t *)"{\"id\":1", 7);
    CHECK(!pokedex_json_done(&j));
    CHECK(!pokedex_json_broken(&j));

    /* 结构损坏:多余闭合括号。 */
    pokedex_json_init(&j, &e);
    pokedex_json_feed(&j, (const uint8_t *)"{]", 2);
    CHECK(pokedex_json_broken(&j));
}

static void test_early_abort_idempotent(void)
{
    pokedex_entry_t e;
    pokedex_json_t j;
    pokedex_json_init(&j, &e);
    memset(&e, 0, sizeof(e));

    size_t pos = 0;
    const uint8_t *d = (const uint8_t *)DOC;
    while (pos < strlen(DOC) && !pokedex_json_done(&j)) {
        pokedex_json_feed(&j, d + pos, 1);
        pos++;
    }
    CHECK(pokedex_json_done(&j));
    /* done 后再喂数据:状态不变。 */
    CHECK(pokedex_json_feed(&j, d + pos, strlen(DOC) - pos));
    CHECK(pokedex_json_done(&j));
    verify_pikachu(&e);
}

int main(void)
{
    test_doc_single_chunk();
    test_doc_chunked();
    test_more_than_two_types();
    test_incomplete_and_broken();
    test_early_abort_idempotent();

    if (s_failures) {
        fprintf(stderr, "pokedex_json: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_json: all checks passed\n");
    return 0;
}