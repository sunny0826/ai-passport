// main/pokedex_core.h —— Pokédex 核心模型。
// 纯 C 实现,不依赖 ESP-IDF/LVGL,可在宿主机上直接单测(tests/test_pokedex_core.c)。
// 职责:图鉴范围与捕捉/见过位图、掉电保存的序列化格式、PokeAPI URL 构造、
// 数值与名称格式化、精灵图缩放尺寸计算。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 全国图鉴第 I–IX 世代(1..1025),与 PokeAPI 的 pokemon-species/{id} 一一对应。
#define POKEDEX_DEX_FIRST  1u
#define POKEDEX_DEX_LAST   1025u
#define POKEDEX_DEX_SIZE   (POKEDEX_DEX_LAST - POKEDEX_DEX_FIRST + 1u) /* 1025 */
#define POKEDEX_GEN_COUNT  9u

// 精灵图显示上限:由 240x320 屏幕布局决定(见 demo_pokedex.c 的精灵面板)。
#define POKEDEX_SPRITE_MAX_W       96u
#define POKEDEX_SPRITE_MAX_H       96u
#define POKEDEX_SPRITE_MAX_BYTES   (POKEDEX_SPRITE_MAX_W * POKEDEX_SPRITE_MAX_H * 2u) /* RGB565 */

// 掉电保存的序列化 blob:定长、显式偏移、小端。改布局必须升版本号并
// 同时更新 tests/test_pokedex_core.c 里的黄金向量。
// v1(PDX1,52B,20 字节位图,仅 1..151)仍可反序列化并迁到 v2。
#define POKEDEX_STATE_BLOB_VERSION 2u
#define POKEDEX_BITMAP_BYTES       130u /* 4 字节对齐,覆盖 1025 个 id(需 129) */
#define POKEDEX_STATE_BLOB_SIZE    272u /* 4+4+4+130+130,无填充 */
#define POKEDEX_STATE_MAGIC        0x50445832u /* "PDX2" */
#define POKEDEX_STATE_V1_MAGIC     0x50445831u /* "PDX1" */
#define POKEDEX_STATE_V1_BLOB_SIZE 52u
#define POKEDEX_STATE_V1_BITMAP    20u

// 捕捉/见过位图:bit(id - 1)。130 字节覆盖 1025 个 id。
typedef struct {
    uint32_t magic;   /* POKEDEX_STATE_MAGIC,校验 blob 是否有效 */
    uint32_t last_id; /* 上次查看的图鉴编号,进入页面时恢复 */
    uint32_t save_seq; /* 每次持久化 +1,仅排障用 */
    uint8_t  caught[POKEDEX_BITMAP_BYTES];
    uint8_t  seen[POKEDEX_BITMAP_BYTES];
} pokedex_state_t;

// static 断言由宿主机测试保证:sizeof(pokedex_state_t) == POKEDEX_STATE_BLOB_SIZE。

// 初始化空白图鉴(last_id = 1)。
void pokedex_state_init(pokedex_state_t *st);

// 图鉴编号是否在 1..1025 范围内。
bool pokedex_id_in_range(uint32_t id);

// 位图查询/修改。越界 id 一律按未捕捉/不存在处理,不写越界内存。
bool pokedex_is_caught(const pokedex_state_t *st, uint32_t id);
void pokedex_set_caught(pokedex_state_t *st, uint32_t id, bool caught);
bool pokedex_is_seen(const pokedex_state_t *st, uint32_t id);
void pokedex_mark_seen(pokedex_state_t *st, uint32_t id);

// 统计已捕捉/已见过数量。
uint32_t pokedex_count_caught(const pokedex_state_t *st);
uint32_t pokedex_count_seen(const pokedex_state_t *st);

// 在 1..1025 内以 delta 步进并回绕(UP=-1, DOWN=+1)。用于按键导航。
uint32_t pokedex_step(uint32_t id, int32_t delta);

// 世代:1..9 对应 I–IX;越界 id 返回 0。
uint32_t pokedex_generation(uint32_t id);
// 该世代全国图鉴起始编号;越界 id 返回 0。
uint32_t pokedex_gen_first(uint32_t id);
// 跳到相邻世代的第一只并回绕(长按 UP/DOWN)。dir<0 上一代,dir>0 下一代。
uint32_t pokedex_step_gen(uint32_t id, int32_t dir);

// 序列化/反序列化。serialize 保证写入 POKEDEX_STATE_BLOB_SIZE 字节;
// deserialize 接受 v2(272B)或 v1(52B,迁到 v2);非法数据返回 false 且不改动 st。
size_t pokedex_state_serialize(const pokedex_state_t *st, uint8_t *buf, size_t cap);
bool pokedex_state_deserialize(pokedex_state_t *st, const uint8_t *buf, size_t len);

// PokeAPI 数据 URL(https://pokeapi.co/api/v2/pokemon/<id>)。
// 返回写入长度(不含 NUL);缓冲区不足返回 0。
size_t pokedex_api_url(uint32_t id, char *buf, size_t cap);

// 默认精灵图 URL(PokeAPI JSON 里 sprites.front_default 缺失时的兜底)。
size_t pokedex_sprite_url(uint32_t id, char *buf, size_t cap);

// "#025" 形式的图鉴编号。
int pokedex_format_number(uint32_t id, char *buf, size_t cap);

// 身高:输入为 PokeAPI 的分米,输出 "0.4 m";体重:输入为百克,输出 "6.0 kg"。
int pokedex_format_height(int decimeters, char *buf, size_t cap);
int pokedex_format_weight(int hectograms, char *buf, size_t cap);

// "pikachu"→"Pikachu","mr-mime"→"Mr Mime","nidoran-f"→"Nidoran F"。
// raw 必须非空;结果截断后仍保证 NUL 结尾。
int pokedex_pretty_name(const char *raw, char *buf, size_t cap);

// 计算源图 (sw,sh) 等比缩放后不超 (max_w,max_h) 的目标尺寸(最近邻,至少 1px)。
bool pokedex_fit_scaled(uint32_t sw, uint32_t sh, uint32_t max_w, uint32_t max_h,
                        uint32_t *dw, uint32_t *dh);

// ---------------------------------------------------------------------------
// PokeAPI pokemon/{id} 响应的单只宝可梦解析结果(页面展示用)。
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t id;
    char     name[24];      /* 原始小写,如 "mr-mime" */
    int      height_dm;     /* PokeAPI height(分米) */
    int      weight_hg;     /* PokeAPI weight(百克) */
    char     types[2][12];  /* 最多两种属性,原始小写 */
    int      type_count;
    char     sprite_url[160]; /* sprites.front_default */
} pokedex_entry_t;

// ---------------------------------------------------------------------------
// 流式 JSON 字段提取器。
//
// 背景:pokeapi 的 pokemon/{id} 完整文档约 290KB(含 moves/stats),ESP32-C3
// 无 PSRAM,且 IDF 5.5 的 esp_http_client 不支持 gzip 解压。因此本提取器
// 逐字节增量解析,只摘出本应用需要的字段,不驻留整份 JSON:
//
//   id / name / height / weight(top 层)、sprites.front_default、
//   types[0..1].type.name(最多两个)。
//
// 用法:一字不漏地把(解压后的)JSON 字节流按任意分块喂给 pokedex_json_feed,
// 每个块之间状态保留在 ctx 里。全部必需字段齐了 pokedex_json_done() 为 true,
// 可提前中止下载;结构损坏 pokedex_json_broken() 为 true。
//
// pokedex_json_t 的结构体公开(定长、可静态分配);字段含义是实现细节,
// 请勿依赖,只通过本头文件声明的函数操作。
#define POKEDEX_JSON_MAX_DEPTH 24

typedef struct pokedex_json {
    pokedex_entry_t *out;      /* 结果(可 NULL,只跑解析不输出) */

    uint8_t stack[POKEDEX_JSON_MAX_DEPTH]; /* 容器类型 '{' / '[' */
    uint8_t marks[POKEDEX_JSON_MAX_DEPTH]; /* 容器标记(见 .c 的 PM_*) */
    unsigned depth;

    unsigned last;        /* pjson_last_t(见 .c) */
    unsigned char mode;   /* PJSON_S_* */
    bool str_is_key;      /* 当前字符串是键名 */
    bool in_escape;       /* 字符串内刚遇到 '\\' */
    unsigned esc_u_rem;   /* 待跳过的 \\u 十六进制位数 */

    char key[24];      /* 键名缓冲(目标键都很短) */
    unsigned key_len;
    char val[168];     /* 字符串值缓冲(sprite URL 最长) */
    unsigned val_len;
    char num[10];      /* 数字缓冲 */
    unsigned num_len;

    bool pending_types_arr;   /* 等待 types 的 '[' */
    bool pending_type_obj;    /* 等待 element.type 的 '{' */
    bool pending_sprites_obj; /* 等待 sprites 的 '{' */

    unsigned pending;     /* PP_* 当前待捕获槽位 */
    bool have_id, have_name, have_height, have_weight, have_front;
    unsigned type_count;

    bool broken;
    bool done;
} pokedex_json_t;

void pokedex_json_init(pokedex_json_t *ctx, pokedex_entry_t *out);
bool pokedex_json_feed(pokedex_json_t *ctx, const uint8_t *data, size_t len);
bool pokedex_json_done(const pokedex_json_t *ctx);
bool pokedex_json_broken(const pokedex_json_t *ctx);