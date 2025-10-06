
#include "pthread_impl.h"
#include <bits/syscall.h>
#include <errno.h>

#include "tls_lock.h"

#define WAITERS 0x80000000
#define TID_MASK (~WAITERS)

void __tls_lock(int *mutex) {
	int expected = 0;

	if (__atomic_compare_exchange_n(mutex, &expected, EBUSY, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		return;
	}

	for (int spins = 0; spins < 100; spins++) {
		expected = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
		if (expected == 0)
			break;

		a_spin();
	}

	expected = 0;
	while (!__atomic_compare_exchange_n(mutex, &expected, EBUSY, 0,
					    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		if (!(expected & WAITERS)) {
			__atomic_compare_exchange_n(mutex, &expected, expected | WAITERS, 0,
						    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
			if (expected == 0) {
				continue;
			}
			expected |= WAITERS;
		}

		__futexwait(mutex, expected, 0);
		expected = 0;
	}
}

void __tls_unlock(int *mutex) {
	int old = __atomic_exchange_n(mutex, 0, __ATOMIC_RELEASE);
	if (old & WAITERS) {
		__wake(mutex, INT_MAX, 0);
	}
}