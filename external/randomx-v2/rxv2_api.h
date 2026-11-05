/*
 * Prefix nama API publik RandomX V2 (ConnectCoin) supaya tidak bentrok
 * dengan library RandomX lain yang di-link ke biner yang sama (RandomY
 * milik Core Coin XCB memakai nama simbol yang identik: 640 simbol global
 * bertabrakan). Daftar ini dipakai dua sisi:
 *   - wrapper kita (src/Hashing/RandomXV2.cpp) sebelum include randomx.h
 *   - prefix_symbols.sh yang me-rename simbol di librandomx_v2.a
 *
 * Kalau daftar di randomx.h berubah, update di sini juga.
 */
#ifndef RXV2_API_H
#define RXV2_API_H

#define randomx_get_flags rxv2_randomx_get_flags
#define randomx_alloc_cache rxv2_randomx_alloc_cache
#define randomx_init_cache rxv2_randomx_init_cache
#define randomx_get_cache_memory rxv2_randomx_get_cache_memory
#define randomx_release_cache rxv2_randomx_release_cache
#define randomx_alloc_dataset rxv2_randomx_alloc_dataset
#define randomx_dataset_item_count rxv2_randomx_dataset_item_count
#define randomx_init_dataset rxv2_randomx_init_dataset
#define randomx_get_dataset_memory rxv2_randomx_get_dataset_memory
#define randomx_release_dataset rxv2_randomx_release_dataset
#define randomx_create_vm rxv2_randomx_create_vm
#define randomx_vm_set_cache rxv2_randomx_vm_set_cache
#define randomx_vm_set_dataset rxv2_randomx_vm_set_dataset
#define randomx_destroy_vm rxv2_randomx_destroy_vm
#define randomx_calculate_hash rxv2_randomx_calculate_hash
#define randomx_calculate_hash_first rxv2_randomx_calculate_hash_first
#define randomx_calculate_hash_next rxv2_randomx_calculate_hash_next
#define randomx_calculate_hash_last rxv2_randomx_calculate_hash_last
#define randomx_calculate_commitment rxv2_randomx_calculate_commitment

#endif /* RXV2_API_H */
