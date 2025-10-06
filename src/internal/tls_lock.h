#ifndef _TLS_LOCK_H
#define _TLS_LOCK_H

void __tls_lock(int *mutex);
void __tls_unlock(int *mutex);

#endif