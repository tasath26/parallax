#include "level_read_cursor.h"
#include "index_node.h"
#include <assert.h>
#include <log.h>
#include <string.h>
#include <unistd.h>

static void WCURSOR_fill_heap_node_from_L0(struct RCURSOR_level_read_cursor *r_cursor, struct sh_heap_node *heap_node)
{
	heap_node->KV = r_cursor->L0_cursor->L0_scanner->keyValue;
	heap_node->level_id = r_cursor->level_id;
	heap_node->type = r_cursor->L0_cursor->L0_scanner->kv_format;
	heap_node->cat = r_cursor->L0_cursor->L0_scanner->cat;
	heap_node->kv_size = r_cursor->L0_cursor->L0_scanner->kv_size;
	heap_node->tombstone = r_cursor->L0_cursor->L0_scanner->tombstone;
	heap_node->active_tree = r_cursor->tree_id;
}

static void WCURSOR_fill_heap_node_from_device(struct RCURSOR_level_read_cursor *r_cursor, struct sh_heap_node *h_node)
{
	h_node->level_id = r_cursor->level_id;
	h_node->active_tree = r_cursor->tree_id;
	h_node->cat = r_cursor->cursor_key.kv_category;
	h_node->tombstone = r_cursor->cursor_key.tombstone;
	switch (h_node->cat) {
	case SMALL_INPLACE:
	case MEDIUM_INPLACE:
		h_node->type = KV_FORMAT;
		h_node->KV = (char *)r_cursor->cursor_key.kv_in_place;
		h_node->kv_size = get_kv_size((struct kv_splice *)h_node->KV);
		break;
	case BIG_INLOG:
	case MEDIUM_INLOG:
		h_node->type = KV_PREFIX;
		// log_info("Prefix %.12s dev_offt %llu", cur->cursor_key.in_log->prefix,
		//	 cur->cursor_key.in_log->device_offt);
		h_node->KV = (char *)&r_cursor->cursor_key.kv_inlog;
		h_node->kv_size = get_kv_seperated_splice_size();
		break;
	default:
		log_fatal("UNKNOWN_LOG_CATEGORY");
		BUG_ON();
	}
}

void WCURSOR_fill_heap_node(struct RCURSOR_level_read_cursor *r_cursor, struct sh_heap_node *h_node)
{
	h_node->db_desc = r_cursor->handle->db_desc;
	0 == r_cursor->level_id ? WCURSOR_fill_heap_node_from_L0(r_cursor, h_node) :
				  WCURSOR_fill_heap_node_from_device(r_cursor, h_node);
}

struct RCURSOR_level_read_cursor *RCURSOR_init_cursor(db_handle *handle, uint32_t level_id, uint32_t tree_id,
						      int file_desc)
{
	struct RCURSOR_level_read_cursor *r_cursor = calloc(1UL, sizeof(struct RCURSOR_level_read_cursor));

	r_cursor->level_id = level_id;
	r_cursor->tree_id = tree_id;
	r_cursor->handle = handle;
	r_cursor->is_end_of_level = false;

	if (0 == level_id) {
		node_header *root = r_cursor->handle->db_desc->levels[0].root_w[tree_id];
		if (NULL == root)
			root = r_cursor->handle->db_desc->levels[0].root_r[tree_id];

		r_cursor->L0_cursor = calloc(1UL, sizeof(struct RCURSOR_L0_cursor));
		r_cursor->L0_cursor->L0_scanner = _init_compaction_buffer_scanner(handle, level_id, root, NULL);
		return r_cursor;
	}

	r_cursor->device_cursor = NULL;
	if (posix_memalign((void **)&r_cursor->device_cursor, ALIGNMENT, sizeof(struct RCURSOR_device_cursor)) != 0) {
		log_fatal("Posix memalign failed");
		perror("Reason: ");
		BUG_ON();
	}
	memset(r_cursor->device_cursor, 0xFF, sizeof(struct RCURSOR_device_cursor));

	r_cursor->device_cursor->fd = file_desc;
	r_cursor->device_cursor->offset = 0;
	r_cursor->device_cursor->curr_segment = NULL;
	r_cursor->device_cursor->curr_leaf_entry = 0;
	r_cursor->device_cursor->state = COMP_CUR_FETCH_NEXT_SEGMENT;
	RCURSOR_get_next_kv(r_cursor);
	return r_cursor;
}

static bool RCURSOR_get_next_KV_from_L0(struct RCURSOR_level_read_cursor *r_cursor)
{
	bool ret = END_OF_DATABASE == level_scanner_get_next(r_cursor->L0_cursor->L0_scanner) ? false : true;
	return ret;
}

static bool RCURSOR_get_next_kv_from_device(struct RCURSOR_level_read_cursor *r_cursor)
{
	struct RCURSOR_device_cursor *device_cursor = r_cursor->device_cursor;

	uint32_t level_leaf_size = r_cursor->handle->db_desc->levels[r_cursor->level_id].leaf_size;
	if (r_cursor->is_end_of_level)
		return false;
	while (1) {
	fsm_entry:
		switch (device_cursor->state) {
		case COMP_CUR_CHECK_OFFT: {
			if (device_cursor->offset >=
			    r_cursor->handle->db_desc->levels[r_cursor->level_id].offset[r_cursor->tree_id]) {
				log_debug("Done read level %u", r_cursor->level_id);
				r_cursor->is_end_of_level = true;
				assert(device_cursor->offset ==
				       r_cursor->handle->db_desc->levels[r_cursor->level_id].offset[r_cursor->tree_id]);
				return false;
			}
			if (device_cursor->offset % SEGMENT_SIZE == 0)
				device_cursor->state = COMP_CUR_FETCH_NEXT_SEGMENT;
			else
				device_cursor->state = COMP_CUR_FIND_LEAF;
			break;
		}

		case COMP_CUR_FETCH_NEXT_SEGMENT: {
			if (device_cursor->curr_segment == NULL) {
				device_cursor->curr_segment = r_cursor->handle->db_desc->levels[r_cursor->level_id]
								      .first_segment[r_cursor->tree_id];
			} else {
				if (device_cursor->curr_segment->next_segment == NULL) {
					assert((uint64_t)device_cursor->curr_segment ==
					       (uint64_t)r_cursor->handle->db_desc->levels[r_cursor->level_id]
						       .last_segment[r_cursor->tree_id]);
					log_debug("Done reading level %u cursor offset %lu total offt %lu",
						  r_cursor->level_id, device_cursor->offset,
						  r_cursor->handle->db_desc->levels[r_cursor->level_id]
							  .offset[r_cursor->tree_id]);
					assert(device_cursor->offset ==
					       r_cursor->handle->db_desc->levels[r_cursor->level_id]
						       .offset[r_cursor->tree_id]);
					device_cursor->state = COMP_CUR_CHECK_OFFT;
					//TODO replace goto with continue;
					//TODO Rename device_offt
					goto fsm_entry;
				} else
					device_cursor->curr_segment = (segment_header *)REAL_ADDRESS(
						(uint64_t)device_cursor->curr_segment->next_segment);
			}
			/*log_info("Fetching next segment id %llu for [%lu][%lu]", c->curr_segment->segment_id,
				 c->level_id, c->tree_id);*/
			/*read the segment*/

			off_t dev_offt = ABSOLUTE_ADDRESS(device_cursor->curr_segment);
			//	log_info("Reading level segment from dev_offt: %llu", dev_offt);
			ssize_t bytes_read = 0; //sizeof(struct segment_header);
			while (bytes_read < SEGMENT_SIZE) {
				ssize_t bytes = pread(device_cursor->fd, &device_cursor->segment_buf[bytes_read],
						      SEGMENT_SIZE - bytes_read, dev_offt + bytes_read);
				if (-1 == bytes) {
					log_fatal("Failed to read error code");
					perror("Error");
					BUG_ON();
				}
				bytes_read += bytes;
			}
			device_cursor->offset += sizeof(struct segment_header);
			device_cursor->state = COMP_CUR_FIND_LEAF;
			break;
		}

		case COMP_CUR_DECODE_KV: {
			struct bt_dynamic_leaf_node *leaf =
				(struct bt_dynamic_leaf_node *)((uint64_t)device_cursor->segment_buf +
								(device_cursor->offset % SEGMENT_SIZE));
			// slot array entry
			if (device_cursor->curr_leaf_entry >= leaf->header.num_entries) {
				// done with this leaf
				device_cursor->curr_leaf_entry = 0;
				device_cursor->offset += level_leaf_size;
				device_cursor->state = COMP_CUR_CHECK_OFFT;
				break;
			}
			struct bt_dynamic_leaf_slot_array *slot_array = get_slot_array_offset(leaf);

			r_cursor->cursor_key.kv_category = slot_array[device_cursor->curr_leaf_entry].key_category;
			r_cursor->cursor_key.tombstone = slot_array[device_cursor->curr_leaf_entry].tombstone;
			char *kv_loc =
				get_kv_offset(leaf, level_leaf_size, slot_array[device_cursor->curr_leaf_entry].index);
			switch (r_cursor->cursor_key.kv_category) {
			case SMALL_INPLACE:
			case MEDIUM_INPLACE: {
				// Real key in KV_FORMAT
				r_cursor->cursor_key.kv_in_place = (struct kv_splice *)fill_keybuf(
					kv_loc, get_kv_format(slot_array[device_cursor->curr_leaf_entry].key_category));
				break;
			}
			case MEDIUM_INLOG:
			case BIG_INLOG:
				r_cursor->cursor_key.kv_inlog = *(struct kv_seperation_splice *)kv_loc;
				r_cursor->cursor_key.kv_inlog.dev_offt =
					(uint64_t)REAL_ADDRESS(r_cursor->cursor_key.kv_inlog.dev_offt);
				break;
			default:
				log_fatal("Cannot handle this category");
				BUG_ON();
			}
			++device_cursor->curr_leaf_entry;
			return true;
		}

		case COMP_CUR_FIND_LEAF: {
			/*read four bytes to check what is the node format*/
			nodeType_t type =
				*(uint32_t *)(&device_cursor->segment_buf[device_cursor->offset % SEGMENT_SIZE]);
			switch (type) {
			case leafNode:
			case leafRootNode:
				//__sync_fetch_and_add(&leaves, 1);
				//log_info("Found a leaf!");
				device_cursor->state = COMP_CUR_DECODE_KV;
				goto fsm_entry;

			case rootNode:
			case internalNode:
				/*log_info("Found an internal");*/
				device_cursor->offset += index_node_get_size();
				device_cursor->state = COMP_CUR_CHECK_OFFT;
				goto fsm_entry;

			case paddedSpace:
				/*log_info("Found padded space of size %llu",
					 (SEGMENT_SIZE - (c->offset % SEGMENT_SIZE)));*/
				device_cursor->offset += (SEGMENT_SIZE - (device_cursor->offset % SEGMENT_SIZE));
				device_cursor->state = COMP_CUR_CHECK_OFFT;
				goto fsm_entry;
			default:
				log_fatal("Faulty read cursor of level %u Wrong node type %u offset "
					  "was %lu total level offset %lu faulty segment offt: %lu",
					  r_cursor->level_id, type, device_cursor->offset,
					  r_cursor->handle->db_desc->levels[r_cursor->level_id].offset[0],
					  ABSOLUTE_ADDRESS(device_cursor->curr_segment));
				BUG_ON();
			}

			break;
		}
		default:
			log_fatal("Error state");
			BUG_ON();
		}
	}
}

bool RCURSOR_get_next_kv(struct RCURSOR_level_read_cursor *r_cursor)
{
	if (NULL == r_cursor) {
		log_fatal("NULL cursor!");
		BUG_ON();
	}
	return 0 == r_cursor->level_id ? RCURSOR_get_next_KV_from_L0(r_cursor) :
					 RCURSOR_get_next_kv_from_device(r_cursor);
}

void RCURSOR_close_cursor(struct RCURSOR_level_read_cursor *r_cursor)
{
	if (NULL == r_cursor)
		return;

	if (0 == r_cursor->level_id) {
		close_compaction_buffer_scanner(r_cursor->L0_cursor->L0_scanner);
		free(r_cursor->L0_cursor);
		free(r_cursor);
		return;
	}
	free(r_cursor->device_cursor);
	free(r_cursor);
}
