
#include <stdint.h>
#include "pthread_impl.h"
#include "stdlib.h"
#include "string.h"

typedef unsigned int gcc_word __attribute__((mode(word)));
typedef unsigned int gcc_pointer __attribute__((mode(pointer)));

struct __emutls_control {
	gcc_word size;
	gcc_word align;
	union {
		uintptr_t index;
		void *address;
	};
	void *value;
};

struct emutls_array {
	uintptr_t len;
	void *data[];
};

static pthread_mutex_t emutls_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t emutls_num_object = 0;

void emutls_free() {
	struct pthread *self = __pthread_self();
	struct emutls_array *array = self->emutls_array;

	if (!array) {
		return;
	}

	for (uintptr_t i = 0; i < array->len; ++i) {
		if (array->data[i]) {
			free(array->data[i]);
		}
	}

	free(array);
}

void *__emutls_get_address(struct __emutls_control *control)
{
	struct pthread *self = __pthread_self();
	struct emutls_array *array = self->emutls_array;

	uintptr_t index = __atomic_load_n(&control->index, __ATOMIC_ACQUIRE);
	if (__builtin_expect(index == 0, 0)) {
		pthread_mutex_lock(&emutls_mutex);
		index = control->index;
		if (index == 0) {
			index = ++emutls_num_object;
			__atomic_store_n(&control->index, index, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&emutls_mutex);
	}

	if (__builtin_expect(!array, 0)) {
		uintptr_t len = index + 32;
		size_t size = sizeof(struct emutls_array) + len * sizeof(void *);
		array = malloc(size);
		if (!array) {
			abort();
		}
		memset(array->data, 0, len * sizeof(void *));
		array->len = len;
		self->emutls_array = array;

	} else if (__builtin_expect(index > array->len, 0)) {
		uintptr_t orig_len = array->len;
		uintptr_t len = orig_len * 2;
		if (index > len) {
			len = index + 32;
		}

		size_t size = sizeof(struct emutls_array) + len * sizeof(void *);
		array = realloc(array, size);
		if (!array) {
			abort();
		}
		memset(array->data + orig_len, 0, (len - orig_len) * sizeof(void *));
		array->len = len;
		self->emutls_array = array;
	}

	void *ret = array->data[index - 1];
	if (__builtin_expect(!ret, 0)) {
		gcc_word align = control->align;
		if (align < sizeof(void *)) {
			align = sizeof(void *);
		}
		int ret2 = posix_memalign(&ret, align, control->size);
		if (ret2) {
			abort();
		}
		if (control->value) {
			memcpy(ret, control->value, control->size);
		} else {
			memset(ret, 0, control->size);
		}
		array->data[index - 1] = ret;
	}
	return ret;
}

void __emutls_register_common(struct __emutls_control *control, gcc_word size, gcc_word align, void *value)
{
	if (control->size < size)
	{
		control->size = size;
		control->value = NULL;
	}
	if (control->align < align) {
		control->align = align;
	}
	if (value && size == control->size) {
		control->value = value;
	}
}