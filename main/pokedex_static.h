// main/pokedex_static.h —— 离线静态图鉴数据(生成文件,勿手改)。
// 生成:tools/gen_pokedex_static.py;数据来源 PokeAPI (CC-BY 4.0)。
#pragma once
#include <stdint.h>
#include <stddef.h>

#define POKEDEX_STATIC_DEX_W 48u
#define POKEDEX_STATIC_DEX_H 48u
#define POKEDEX_STATIC_SPRITE_BYTES 4608u
#define POKEDEX_DESC_MAX 192u

typedef struct {
    uint16_t id;      /* 1..151 */
    int16_t  height_dm;
    int16_t  weight_hg;
    uint8_t  type0;   /* POKEDEX_STATIC_TYPE_* 索引 */
    uint8_t  type1;   /* 0xFF = 无第二属性 */
    char     name[16];
    char     desc[POKEDEX_DESC_MAX]; /* 英文图鉴描述 */
} pokedex_static_entry_t;

enum {
    POKEDEX_STATIC_TYPE_BUG = 0,
    POKEDEX_STATIC_TYPE_DRAGON = 1,
    POKEDEX_STATIC_TYPE_ELECTRIC = 2,
    POKEDEX_STATIC_TYPE_FAIRY = 3,
    POKEDEX_STATIC_TYPE_FIGHTING = 4,
    POKEDEX_STATIC_TYPE_FIRE = 5,
    POKEDEX_STATIC_TYPE_FLYING = 6,
    POKEDEX_STATIC_TYPE_GHOST = 7,
    POKEDEX_STATIC_TYPE_GRASS = 8,
    POKEDEX_STATIC_TYPE_GROUND = 9,
    POKEDEX_STATIC_TYPE_ICE = 10,
    POKEDEX_STATIC_TYPE_NORMAL = 11,
    POKEDEX_STATIC_TYPE_POISON = 12,
    POKEDEX_STATIC_TYPE_PSYCHIC = 13,
    POKEDEX_STATIC_TYPE_ROCK = 14,
    POKEDEX_STATIC_TYPE_STEEL = 15,
    POKEDEX_STATIC_TYPE_WATER = 16,
    POKEDEX_STATIC_TYPE_COUNT,
    POKEDEX_STATIC_TYPE_NONE = 0xFF,
};

extern const pokedex_static_entry_t pokedex_static_dex[152]; /* 下标=id */
extern const char *pokedex_static_type_names[POKEDEX_STATIC_TYPE_COUNT];

// 精灵图 blob 的 TOC/数据位于 pokedex_sprites.bin(由 CMake 嵌入):
extern const uint8_t _binary_pokedex_sprites_bin_start[];
extern const uint8_t _binary_pokedex_sprites_bin_end[];
