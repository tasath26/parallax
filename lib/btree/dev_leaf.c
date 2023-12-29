// Copyright [1] [FORTH-ICS]
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "../btree/kv_pairs.h"
#include "../common/common.h"
#include "btree_node.h"
#include "device_level.h"
#include "parallax/structures.h"
#include <assert.h>
#include <log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if 0
static bool devl_check_leaf(struct leaf_node *leaf);
#endif

struct devl_slot_array {
	// The index points to the location of the kv pair in the leaf.
	uint16_t key_category : 2;
	// Tombstone notifies if the key is deleted.
	uint16_t tombstone : 1;
	uint16_t index : 13;
};

struct leaf_node {
	struct node_header header;
} __attribute__((packed));

static struct devl_slot_array *devl_get_slot_array_offset(const struct leaf_node *leaf)
{
	return (struct devl_slot_array *)(((char *)leaf) + sizeof(struct leaf_node));
}

static struct kv_splice_base devl_get_general_splice(struct leaf_node *leaf, int32_t position)
{
	struct kv_splice_base general_splice = { 0 };
	struct devl_slot_array *slot_array = devl_get_slot_array_offset(leaf);
	general_splice.kv_cat = slot_array[position].key_category;
	general_splice.is_tombstone = slot_array[position].tombstone;
	uint8_t *kv_addr = (uint8_t *)leaf + slot_array[position].index;
	if (general_splice.kv_cat == SMALL_INPLACE || general_splice.kv_cat == MEDIUM_INPLACE) {
		general_splice.kv_splice = (struct kv_splice *)kv_addr;
		general_splice.kv_type = KV_FORMAT;
	} else if (general_splice.kv_cat == MEDIUM_INLOG || general_splice.kv_cat == BIG_INLOG) {
		general_splice.kv_sep2 = (struct kv_seperation_splice2 *)kv_addr;
		general_splice.kv_type = KV_PREFIX;
	} else {
		log_fatal("Unknown kv category");
		BUG_ON();
	}
	return general_splice;
}

static void devl_fill_key_from_general_splice(struct kv_splice_base *general_splice, char **key, int32_t *key_size)
{
	if (general_splice->kv_cat == SMALL_INPLACE || general_splice->kv_cat == MEDIUM_INPLACE) {
		*key = kv_splice_get_key_offset_in_kv(general_splice->kv_splice);
		*key_size = kv_splice_get_key_size(general_splice->kv_splice);
	} else if (general_splice->kv_cat == MEDIUM_INLOG || general_splice->kv_cat == BIG_INLOG) {
		*key = kv_sep2_get_key(general_splice->kv_sep2);
		*key_size = kv_sep2_get_key_size(general_splice->kv_sep2);
	} else {
		log_fatal("Unknown kv category");
		BUG_ON();
	}
}

static int32_t devl_search_get_pos(struct leaf_node *leaf, char *key, int32_t key_size, bool *exact_match)
{
	*exact_match = false;
	if (NULL == leaf || leaf->header.num_entries == 0)
		return -1;

	int32_t cmp_return_value = 0;
	int32_t start = 0;
	int32_t end = leaf->header.num_entries - 1;

	int32_t middle = 0;
	while (start <= end) {
		middle = (start + end) / 2;

		struct kv_splice_base leaf_splice = devl_get_general_splice(leaf, middle);

		/*At zero position we have a guard or -oo*/
		char *leaf_key = NULL;
		int32_t leaf_key_size = 0;
		devl_fill_key_from_general_splice(&leaf_splice, &leaf_key, &leaf_key_size);
		// log_debug(
		// 	"Comparing leaf key size: %d leaf key data %.*s  pos is %d with look up key size: %d key data %.*s",
		// 	leaf_key_size, leaf_key_size, leaf_key, middle, key_size, key_size, key);
		assert(leaf_key_size > 0);

		cmp_return_value = memcmp(leaf_key, key, key_size <= leaf_key_size ? key_size : leaf_key_size);

		if (0 == cmp_return_value && leaf_key_size == key_size) {
			*exact_match = true;
			// log_debug("Found at Leaf is %p num entries %d pos is %d", (void *)leaf,
			// 	  leaf->header.num_entries, middle);
			return middle;
		}

		if (0 == cmp_return_value) {
			// log_debug(
			// 	"Partial match leaf key size: %d leaf key %.*s | lookup key size %d lookup key %.*s middle %d leaf entries %d",
			// 	leaf_key_size, leaf_key_size, leaf_key, key_size, key_size, key, middle,
			// 	leaf->header.num_entries);
			cmp_return_value = leaf_key_size - key_size;
		}
		if (cmp_return_value > 0)
			end = middle - 1;
		else
			start = middle + 1;
	}

	return cmp_return_value > 0 ? middle - 1 : middle;
}

static struct kv_splice_base devl_find_kv_in_dynamic_leaf(struct leaf_node *leaf, char *key, int32_t key_size,
							  const char **error)
{
	struct kv_splice_base kv_not_found = { 0 };
	bool exact_match = false;
	int32_t pos = devl_search_get_pos(leaf, key, key_size, &exact_match);

	if (exact_match)
		return devl_get_general_splice(leaf, pos);
	*error = "KV pair not found";
	return kv_not_found;
}

static bool devl_is_leaf_full(struct leaf_node *leaf, uint32_t kv_size)
{
	uint8_t *left_border = (uint8_t *)leaf + sizeof(struct node_header) +
			       ((leaf->header.num_entries + 1) * sizeof(struct devl_slot_array));

	uint8_t *right_border = (uint8_t *)leaf + leaf->header.log_size;
	right_border -= kv_size;
	// log_debug("kv_size %d right_border %lu left border %lu", kv_size, right_border, left_border);
	return right_border > left_border ? false : true;
}

static uint16_t devl_append_data_splice_in_dynamic_leaf(struct leaf_node *leaf, struct kv_splice_base *general_splice)
{
	int32_t kv_size = kv_splice_base_calculate_size(general_splice);
	if (devl_is_leaf_full(leaf, kv_size)) {
		log_warn("Leaf is full cannot serve request");
		return 0;
	}
	assert(leaf->header.log_size > kv_size);
	leaf->header.log_size -= kv_size;
	char *src = (char *)leaf;
	char *dest = &src[leaf->header.log_size];
	kv_splice_base_serialize(general_splice, dest, kv_size);

	return leaf->header.log_size;
}

static bool devl_append_splice_in_dynamic_leaf(struct leaf_node *leaf, struct kv_splice_base *general_splice,
					       bool is_tombstone)
{
	uint16_t offt = devl_append_data_splice_in_dynamic_leaf(leaf, general_splice);
	if (!offt) {
		log_fatal("Leaf is full cannot serve request to avoid overflow (it shouldn't at this point)");
		_exit(EXIT_FAILURE);
	}
	struct devl_slot_array *slot_array = devl_get_slot_array_offset(leaf);
	slot_array[leaf->header.num_entries].index = offt;
	slot_array[leaf->header.num_entries].tombstone = 0;
	if (is_tombstone)
		slot_array[leaf->header.num_entries].tombstone = 1;
	slot_array[leaf->header.num_entries++].key_category = general_splice->kv_cat;
	return true;
}

// static bool devl_insert_in_dynamic_leaf(struct leaf_node *leaf, struct kv_splice_base *splice, bool is_tombstone,
// 			       bool *exact_match)
// {
// 	log_info("-->");
//   assert(0);
// 	return false;
// }

// static void devl_init_leaf_iterator(struct leaf_node *leaf, struct leaf_iterator *iter, char *key, int32_t key_size)
// {
// 	log_info("-->");
// 	assert(0);
// }

// static bool devl_is_leaf_iterator_valid(struct leaf_iterator *iter)
// {
// 	log_info("-->");
// 	assert(0);
// 	return false;
// }

// static void devl_leaf_iterator_next(struct leaf_iterator *iter)
// {
// 	log_info("-->");
// 	assert(0);
// }

// static struct kv_splice_base devl_leaf_iterator_curr(struct leaf_iterator *iter)
// {
// 	log_info("-->");
// 	assert(0);
// 	struct kv_splice_base splice = { 0 };
// 	return splice;
// }

// static struct kv_splice_base devl_split_dynamic_leaf(struct leaf_node *leaf, struct leaf_node *left, struct leaf_node *right)
// {
// 	log_info("-->");
// 	assert(0);
// 	struct kv_splice_base pivot_splice = {0};
// 	return pivot_splice;
// }

// static inline bool devl_is_reorganize_possible(struct leaf_node *leaf, int32_t kv_size)
// {
// 	log_info("-->");
// 	assert(0);
// 	return false;
// }

// static void devl_reorganize_dynamic_leaf(struct leaf_node *leaf, struct leaf_node *target)
// {
// 	log_info("-->");
// 	assert(0);
// }

static inline void devl_set_leaf_node_type(struct leaf_node *leaf, nodeType_t node_type)
{
	leaf->header.type = node_type;
}

static void devl_init_leaf_node(struct leaf_node *leaf, uint32_t leaf_size)
{
	_Static_assert(sizeof(struct devl_slot_array) == 2,
		       "Dynamic slot array is not 2 bytes, are you sure you want to continue?");
	memset(leaf, 0x00, leaf_size);
	devl_set_leaf_node_type(leaf, leafNode);
	leaf->header.log_size = leaf_size;
	leaf->header.node_size = leaf_size;
}

// cppcheck-suppress unusedFunction
static uint32_t devl_leaf_get_node_size(struct leaf_node *leaf)
{
	(void)leaf;
	log_fatal("XXX TODO XXX unimplemented");
	_exit(EXIT_FAILURE);
	return 0;
}

static inline nodeType_t devl_get_leaf_node_type(struct leaf_node *leaf)
{
	(void)leaf;
	log_fatal("XXX TODO XXX unimplemented");
	_exit(EXIT_FAILURE);
	return 0;
}

static inline int32_t devl_get_leaf_num_entries(struct leaf_node *leaf)
{
	return leaf->header.num_entries;
}

static struct kv_splice_base devl_get_last_splice(struct leaf_node *leaf)
{
	struct kv_splice_base splice = { 0 };
	if (0 == leaf->header.num_entries)
		return splice;

	splice = devl_get_general_splice(leaf, leaf->header.num_entries - 1);
	return splice;
}

bool dev_leaf_register(struct level_leaf_api *leaf_api)
{
	leaf_api->leaf_append = devl_append_splice_in_dynamic_leaf;

	leaf_api->leaf_find = devl_find_kv_in_dynamic_leaf;

	leaf_api->leaf_init = devl_init_leaf_node;

	leaf_api->leaf_is_full = devl_is_leaf_full;

	leaf_api->leaf_get_pos = devl_search_get_pos;

	leaf_api->leaf_get_splice = devl_get_general_splice;

	leaf_api->leaf_set_type = devl_set_leaf_node_type;

	leaf_api->leaf_get_type = devl_get_leaf_node_type;

	leaf_api->leaf_get_entries = devl_get_leaf_num_entries;

	leaf_api->leaf_get_size = devl_leaf_get_node_size;

	leaf_api->leaf_get_last = devl_get_last_splice;

	return true;
}
