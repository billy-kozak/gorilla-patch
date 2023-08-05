/******************************************************************************
* Copyright (C) 2023  Billy Kozak                                             *
*                                                                             *
* This file is part of the gorilla-patch program                              *
*                                                                             *
* This program is free software: you can redistribute it and/or modify        *
* it under the terms of the GNU Lesser General Public License as published by *
* the Free Software Foundation, either version 3 of the License, or           *
* (at your option) any later version.                                         *
*                                                                             *
* This program is distributed in the hope that it will be useful,             *
* but WITHOUT ANY WARRANTY; without even the implied warranty of              *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the               *
* GNU Lesser General Public License for more details.                         *
*                                                                             *
* You should have received a copy of the GNU Lesser General Public License    *
* along with this program.  If not, see <http://www.gnu.org/licenses/>.       *
******************************************************************************/
/******************************************************************************
*                                  INCLUDES                                   *
******************************************************************************/
#include "gmalloc/gorilla-malloc.h"

#include <picounit/picounit.h>
#include <utl/math-utl.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
/******************************************************************************
*                                    DATA                                     *
******************************************************************************/
static size_t page_size;
/******************************************************************************
*                                    TYPES                                    *
******************************************************************************/
struct marked_mem {
	size_t len;
	void *mem[];
};

enum random_action {
	ALLOC = 0,
	FREE,
	_RANDOM_ACTION_TOP
};
/******************************************************************************
*                                  CONSTANTS                                  *
******************************************************************************/
static const int TEST_RNG_SEED = 1728263374;

#define RANDOM_ALLOCATIONS 128
/******************************************************************************
*                                   HELPERS                                   *
******************************************************************************/
static bool mem_test(void *ptr, size_t size)
{
	uint8_t *bptr = ptr;

	for(size_t i = 0; i < size; i++) {
		bptr[i] = i & 0xFF;
	}

	for(size_t i = 0; i < size; i++) {
		if(bptr[i] != (i & 0xFF)) {
			return false;
		}
	}
	return true;
}
/*****************************************************************************/
static struct marked_mem* malloc_and_mark(
	struct gorilla_heap *heap, size_t size
) {
	if(size < sizeof(struct marked_mem)) {
		return NULL;
	}

	struct marked_mem *mem = gorilla_malloc(heap, size);

	mem->len = (size - sizeof(struct marked_mem)) / sizeof(void*);

	for(size_t i = 0; i < mem->len; i++) {
		mem->mem[i] = (mem->mem + i);
	}

	return mem;
}
/*****************************************************************************/
static struct marked_mem* realloc_and_remark(
	struct gorilla_heap *heap, struct marked_mem *mem, size_t size
) {
	if(size < sizeof(struct marked_mem)) {
		return NULL;
	}

	struct marked_mem *new = gorilla_realloc(heap, mem, size);

	new->len = (size - sizeof(struct marked_mem)) / sizeof(void*);

	for(size_t i = 0; i < new->len; i++) {
		new->mem[i] = (new->mem + i);
	}

	return new;
}
/*****************************************************************************/
static bool check_marked_mem(struct marked_mem *mem)
{
	for(size_t i = 0; i < mem->len; i++) {
		void *addr = mem->mem + i;
		if(mem->mem[i] != addr) {
			return false;
		}
	}

	return true;
}
/*****************************************************************************/
static bool free_and_check_marked_mem(
	struct gorilla_heap *heap,
	struct marked_mem *mem
) {
	bool ret = check_marked_mem(mem);
	gorilla_free(heap, mem);

	return ret;
}
/*****************************************************************************/
static double piece_of_rng(
	double p,
	double p0,
	double p1,
	double r0,
	double r1
) {
	double p_eff = (p - p0) / (p1 - p0);
	return ((r1 - r0) * p_eff) + r0;
}
/*****************************************************************************/
static size_t random_size(struct drand48_data *rng)
{
	double p;
	double s;

	drand48_r(rng, &p);

	if(p >= 0.8) {
		s = piece_of_rng(p, 0.8, 1.0, page_size * 4, page_size * 8);
	} else if(p >= 0.6) {
		s = piece_of_rng(p, 0.6, 0.8, page_size, page_size * 4);
	} else if(p >= 0.4) {
		s = piece_of_rng(p, 0.4, 0.6, 256, page_size);
	} else {
		s = piece_of_rng(p, 0.0, 0.4, sizeof(void*), 256);
	}

	return align_down_unsigned(math_utl_round(s), sizeof(void*));
}
/*****************************************************************************/
static enum random_action random_test_action(struct drand48_data *rng)
{
	long l;
	lrand48_r(rng, &l);

	return (l % _RANDOM_ACTION_TOP);
}
/*****************************************************************************/
static int random_allocation_slot(struct drand48_data *rng)
{
	long l;
	lrand48_r(rng, &l);

	return (l % RANDOM_ALLOCATIONS);
}
/******************************************************************************
*                                    TESTS                                    *
******************************************************************************/
static bool test_can_init(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();
	PUNIT_ASSERT(heap != NULL);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_alloc_small(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	void *data = gorilla_malloc(heap, 256);
	PUNIT_ASSERT(mem_test(data, 256));

	gorilla_free(heap, data);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_can_merge(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	/* both b1 and b2 should be split from the original chunk */
	void *b1 = gorilla_malloc(heap, 128);
	void *b2 = gorilla_malloc(heap, 128);

	/* once freed, they should be merged back together after the first
	 * call to gorilla_malloc */
	gorilla_free(heap, b1);
	gorilla_free(heap, b2);

	bool ret = false;

	/* 128 works for current parameters of the heap, but if those parameters
	* change this constant my also need to be raised. */
	void *allocations[128];
	int i = 0;
	for(; i < 128; i++) {
		/* this will continue to split from, and consume the original
		 * chunk until such time as the orignal chunk is too small,
		 * at which point, the merged b1+b2 should be reused */
		allocations[i] = gorilla_malloc(heap, 128);
		if(allocations[i] == b1) {
			ret = true;
			break;
		}
	}
	for(i = (i < 128) ? i : 127; i >= 0; i--) {
		gorilla_free(heap, allocations[i]);
	}

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return ret;

}
/*****************************************************************************/
static bool test_alloc_on_top(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	void *data = gorilla_malloc(heap, page_size * 2);
	PUNIT_ASSERT(mem_test(data, page_size * 2));

	gorilla_free(heap, data);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_pure_mmap_alloc(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();
	void *data = gorilla_malloc(heap, page_size * 8);
	PUNIT_ASSERT(mem_test(data, page_size * 8));

	gorilla_free(heap, data);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_realloc_simple_growth(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	void *data = gorilla_malloc(heap, 128);
	void *grow = gorilla_realloc(heap, data, 256);

	PUNIT_ASSERT(grow != NULL);
	PUNIT_ASSERT(grow == data);

	PUNIT_ASSERT(mem_test(data, 256));

	gorilla_free(heap, grow);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_realloc_shrink(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	uint8_t *data = gorilla_malloc(heap, page_size);
	uint8_t *shrink = gorilla_realloc(heap, data, 128);

	PUNIT_ASSERT(shrink != NULL);
	PUNIT_ASSERT(shrink == data);

	PUNIT_ASSERT(mem_test(data, 128));

	uint8_t *next = gorilla_malloc(heap, 128);

	PUNIT_ASSERT(next < (data + page_size));

	gorilla_free(heap, shrink);
	gorilla_free(heap, next);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_realloc_mmap_grow(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	uint8_t *data = gorilla_malloc(heap, page_size);
	uint8_t *grow = gorilla_realloc(heap, data, page_size * 4);

	PUNIT_ASSERT(grow != NULL);
	PUNIT_ASSERT(grow == data);

	PUNIT_ASSERT(mem_test(grow, page_size * 4));

	gorilla_free(heap, grow);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);

	return true;
}
/*****************************************************************************/
static bool test_mem_move_realloc(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	uint8_t *d1 = gorilla_malloc(heap, 128);
	uint8_t *d2 = gorilla_malloc(heap, 128);

	for(int i = 0; i < 128; i++) {
		d1[i] = i & 0xFF;
	}

	uint8_t *grow = gorilla_realloc(heap, d1, 256);

	PUNIT_ASSERT(d1 != grow);

	for(int i = 0; i < 128; i++) {
		PUNIT_ASSERT(grow[i] == (i & 0xFF));
	}

	gorilla_free(heap, grow);
	gorilla_free(heap, d2);

	PUNIT_ASSERT(gorilla_heap_destroy(heap) == 0);
return true;
}
/*****************************************************************************/
static bool test_random_allocations(void)
{
	struct gorilla_heap *heap = gorilla_heap_init();

	struct drand48_data rng;
	srand48_r(TEST_RNG_SEED, &rng);

	const size_t count = RANDOM_ALLOCATIONS;
	const int rounds = 1024 * 1024;

	struct marked_mem* allocations[RANDOM_ALLOCATIONS] = {};

	for(int i = 0; i < rounds; i++) {
		int slot = random_allocation_slot(&rng);
		size_t size = random_size(&rng);
		enum random_action action = random_test_action(&rng);

		if(allocations[slot] == NULL) {
			allocations[slot] = malloc_and_mark(heap, size);
			PUNIT_ASSERT(allocations[slot] != NULL);
		} else if(action == ALLOC) {
			struct marked_mem *old = allocations[slot];
			PUNIT_ASSERT(check_marked_mem(old));
			struct marked_mem *new = realloc_and_remark(
				heap,
				old,
				size
			);
			PUNIT_ASSERT(new != NULL);
			allocations[slot] = new;
		} else {
			PUNIT_ASSERT(free_and_check_marked_mem(
				heap, allocations[slot]
			));
			allocations[slot] = NULL;
		}
	}

	for(int i = 0; i < count; i++) {
		if(allocations[i] != NULL) {
			PUNIT_ASSERT(free_and_check_marked_mem(
				heap, allocations[i]
			));
		}
	}

	PUNIT_ASSERT(gorilla_malloc_check_leaks(heap, NULL) == NULL);
	gorilla_heap_destroy(heap);

	return true;
}
/*****************************************************************************/
static void test_suite(void)
{
	PUNIT_RUN_TEST(test_can_init);
	PUNIT_RUN_TEST(test_alloc_small);
	PUNIT_RUN_TEST(test_can_merge);
	PUNIT_RUN_TEST(test_alloc_on_top);
	PUNIT_RUN_TEST(test_pure_mmap_alloc);
	PUNIT_RUN_TEST(test_realloc_simple_growth);
	PUNIT_RUN_TEST(test_realloc_shrink);
	PUNIT_RUN_TEST(test_realloc_mmap_grow);
	PUNIT_RUN_TEST(test_mem_move_realloc);
	PUNIT_RUN_TEST(test_random_allocations);
}
/*****************************************************************************/
int main(int argc, char **argv)
{
	page_size = getpagesize();
	PUNIT_RUN_SUITE(test_suite);
	punit_print_stats();
	return 0;
}
/*****************************************************************************/
