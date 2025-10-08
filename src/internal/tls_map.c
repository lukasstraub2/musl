
#include <stdint.h>
#include <stddef.h>
#include "pthread_arch.h"
#include <bits/syscall.h>
#include "libc.h"
#include "sys/mman.h"
#include "syscall.h"
#include <assert.h>
#include "tls_lock.h"

#include "tls_map.h"

#define P1 1031
#define P2 32771
#define INTERVAL 1
#define TOMBSTONE (sizeof(uintptr_t) == 4? 0x80000000UL: 0x8000000000000000ULL)
#define KEY_MASK (~TOMBSTONE)

struct kv_t {
	uintptr_t key;
	uintptr_t value;
	int tid;
};

struct tls_map_t {
	uintptr_t num_entries;
	uintptr_t p;
	struct kv_t map[];
};

static struct tls_map_t *tls_map = NULL;
static int writer_lock = 0;

#define ROUND(x) (((x)+PAGE_SIZE-1)&-PAGE_SIZE)

static struct tls_map_t *tls_map_alloc(uintptr_t p) {
	size_t size = sizeof(struct tls_map_t) + p * sizeof(struct kv_t);
	size = ROUND(size);

	uintptr_t ret = __syscall(__NR_mmap, NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
	if (ret >= -4095UL) {
		return NULL;
	}

	struct tls_map_t *map = (struct tls_map_t *)ret;
	map->num_entries = 0;
	map->p = p;

	return map;
}

int __tls_map_init() {
	struct tls_map_t *map = tls_map_alloc(P1);
	if (!map) {
		return -1;
	}

	tls_map = map;
	return 0;
}

static struct kv_t *tls_map_lookup(struct tls_map_t *map, uintptr_t _search_key) {
	uintptr_t search_key = _search_key & KEY_MASK;
	uintptr_t idx = search_key % map->p;

	while (1) {
		struct kv_t *kv = map->map + idx;
		uintptr_t _key = __atomic_load_n(&kv->key, __ATOMIC_RELAXED);
		uintptr_t key = _key & KEY_MASK;
		if (__builtin_expect(key == search_key, 1)) {
			return kv;
		} else if(!_key) {
			return NULL;
		}

		idx = (idx + INTERVAL) % map->p;
	}
}

static struct kv_t *tls_map_lookup_tid(struct tls_map_t *map, int tid) {
	for (int i = 0; i < map->p; i++) {
		struct kv_t *kv = map->map + i;

		if (kv->tid == tid) {
			return kv;
		}
	}

	return NULL;
}

static int tls_map_insert(struct tls_map_t *map, uintptr_t _insert_key, uintptr_t value, int tid) {
	uintptr_t insert_key = _insert_key & KEY_MASK;
	uintptr_t idx = insert_key % map->p;

	while (1) {
		struct kv_t *kv = map->map + idx;
		uintptr_t _key = kv->key;
		uintptr_t key = _key & KEY_MASK;

		if (!key) {
			kv->key |= insert_key;
			kv->value = value;
			kv->tid = tid;
			return 0;
		}
		__atomic_store_n(&kv->key, _key | TOMBSTONE, __ATOMIC_RELAXED);

		idx = (idx + INTERVAL) % map->p;
	}
}

static void tls_map_clear(struct tls_map_t *map, struct kv_t *kv) {
	__atomic_store_n(&kv->key, kv->key & TOMBSTONE, __ATOMIC_RELAXED);
	kv->value = 0;
	kv->tid = 0;
}

static int tls_map_maybe_migrate() {
	if (tls_map->p != P1) {
		return 0;
	}

	if (tls_map->num_entries < (tls_map->p * 2 / 3)) {
		return 0;
	}

	struct tls_map_t *new_map = tls_map_alloc(P2);
	if (!new_map) {
		return -1;
	}

	for (int i = 0; i < tls_map->p; i++) {
		struct kv_t *kv = tls_map->map + i;
		if (kv->key & KEY_MASK) {
			tls_map_insert(new_map, kv->key, kv->value, kv->tid);
		}
	}
	new_map->num_entries = tls_map->num_entries;

	__atomic_store_n(&tls_map, new_map, __ATOMIC_RELEASE);
	return 0;
}

int __tls_map_set(uintptr_t key, void *_value, int tid) {
	uintptr_t value = (uintptr_t)_value;
	__tls_lock(&writer_lock);
	struct kv_t *kv = tls_map_lookup(tls_map, key);
	if (kv) {
		assert((kv->key & KEY_MASK) == (key & KEY_MASK));
		// This does not hold after fork()
		//assert(kv->tid == tid);
		kv->value = value;
		__tls_unlock(&writer_lock);
		return 0;
	}

	int ret = tls_map_maybe_migrate();
	if (ret < 0) {
		__tls_unlock(&writer_lock);
		return -1;
	}

	if (tls_map->num_entries >= tls_map->p) {
		__tls_unlock(&writer_lock);
		return -1;
	}

	tls_map_insert(tls_map, key, value, tid);
	tls_map->num_entries++;

	__tls_unlock(&writer_lock);
	return 0;
}

void __tls_map_del(int tid) {
	__tls_lock(&writer_lock);
	struct kv_t *kv = tls_map_lookup_tid(tls_map, tid);
	if (!kv) {
		__tls_unlock(&writer_lock);
		return;
	}
	tls_map_clear(tls_map, kv);
	tls_map->num_entries--;
	__tls_unlock(&writer_lock);
}

uintptr_t __tls_map_get_tp() {
	struct tls_map_t *map = __atomic_load_n(&tls_map, __ATOMIC_ACQUIRE);
	uintptr_t ptr = __get_tp();
	struct kv_t *kv;

	kv = tls_map_lookup(map, ptr);
	if (__builtin_expect(kv != NULL, 1)) {
		return kv->value;
	}

	int tid = __syscall(__NR_gettid);

	__tls_lock(&writer_lock);
	kv = tls_map_lookup_tid(tls_map, tid);
	assert(kv);
	uintptr_t value = kv->value;

	tls_map_clear(tls_map, kv);
	tls_map_insert(tls_map, ptr, value, tid);
	__tls_unlock(&writer_lock);

	return value;
}