#ifndef _TLS_MAP_H
#define _TLS_MAP_H

#include <stdint.h>

int __tls_map_init();
int __tls_map_set(uintptr_t key, void *value, int tid);
void __tls_map_del(int tid);
uintptr_t __tls_map_get_tp();

#endif