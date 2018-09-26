/*
 * (c) 2018 - idlab - UGent - imec
 *
 * Bart Moons
 *
 * This file is part of the SCHC stack implementation
 *
 */

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "schc_config.h"

#include "fragmenter.h"

uint8_t ATTEMPTS = 0; // for debugging

#if CLICK
#include <click/config.h>
#endif

// keep track of the active connections
struct schc_fragmentation_t schc_rx_conns[SCHC_CONF_RX_CONNS];
static uint8_t fragmentation_buffer[MAX_MTU_LENGTH];

// keep track of the mbuf's
static uint32_t MBUF_PTR;
static struct schc_mbuf_t MBUF_POOL[SCHC_CONF_MBUF_POOL_LEN];

#if !DYNAMIC_MEMORY
static uint8_t buf_ptr = 0;
uint8_t schc_buf[SCHC_BUFSIZE] = { 0 };
#endif

// ToDo
// create file bit_array.c?
// compressor will need this too

/**
 * sets bits at a certain position in a bit array
 * big endian
 *
 * @param A				the bit array
 * @param pos			which bit to set
 * @param len			the number of consecutive bits to set
 *
 */
static void set_bits(uint8_t A[], uint32_t pos, uint32_t len) {
	uint32_t i;
	for(i = pos; i < (len + pos); i++) {
		A[i / 8] |= 128 >> (i % 8);
	}
}

/**
 * get bits at a certain position in a bit array
 *
 * @param A				the bit array
 * @param pos			the position to start from
 * @param len			the number of consecutive bits to get
 *
 * @note  limited to 32 consecutive bits
 *
 */
static uint32_t get_bits(uint8_t A[], uint32_t pos, uint8_t len) {
	uint32_t i; uint32_t j = (len - 1); uint32_t number = 0;

	for(i = pos; i < (len + pos); i++) {
		uint8_t bit = A[i / 8] & 128 >> (i % 8);
		number |= (!!bit << j);
		j--;
	}

	return number;
}

/**
 * clear bits at a certain position in a bit array
 * big endian
 *
 * @param A				the bit array
 * @param pos			which bit to clear
 * @param len			the number of consecutive bits to clear
 *
 */
static void clear_bits(uint8_t A[], uint32_t pos, uint32_t len) {
	uint32_t i;
	for(i = pos; i < (len + pos); i++) {
		A[i / 8] &= ~(128 >> (i % 8));
	}
}

/**
 * copy bits to a certain position in a bit array
 * from another array
 * big endian
 *
 * @param DST			the array to copy to
 * @param dst_pos		which bit to start from
 * @param SRC			the array to copy from
 * @param src_pos		which bit to start from
 * @param len			the number of consecutive bits to set
 *
 */
static void copy_bits(uint8_t DST[], uint32_t dst_pos, uint8_t SRC[], uint32_t src_pos,
		uint32_t len) {
	uint32_t i;
	uint32_t k = 0;

	for(i = 0; i < len; i++) { // for each bit
		uint8_t src_val = ((128 >> ( (k + src_pos) % 8)) & SRC[((k + src_pos) / 8)]);
		if(src_val) {
			// DEBUG_PRINTF("set bits for %d at position %d len is %d", DST[i+dst_pos], i+dst_pos, len);
			set_bits(DST, i + dst_pos, 1);
		}
		k++;
	}
}

/**
 * compare two bit arrays
 *
 * @param 	SRC1		the array to compare
 * @param 	SRC2		the array to compare with
 * @param 	len			the number of consecutive bits to compare
 *
 * @return	1			both arrays match
 * 			0			the arrays differ
 *
 */
static uint8_t compare_bits(uint8_t SRC1[], uint8_t SRC2[], uint32_t len) {
	uint32_t i;

	for (i = 0; i < len; i++) {
		if ( (SRC1[i / 8] & (128 >> (i % 8) )) != (SRC2[i / 8] & (128 >> (i % 8) )) ) {
			return 0;
		}
	}

	return 1;
}

/**
 * shift a number of bits to the left
 *
 * @param 	SRC			the array to shift
 * @param	len			the length of the array
 * @param 	shift		the number of consecutive bits to shift
 *
 */
static void shift_bits_left(uint8_t SRC[], uint16_t len, uint32_t shift) {
	uint32_t i = 0; uint32_t j = 0;

	uint8_t start = shift / 8;
	uint8_t rest = shift % 8;

	for(i = start; i < len; i++) {
		uint8_t value = (SRC[i] << rest) | (SRC[i + 1] >> (8 - rest));
		SRC[j] = value;
		j++;
	}

}

/**
 * shift a number of bits to the right
 *
 * @param 	SRC			the array to shift
 * @param	len			the length of the array
 * @param 	shift		the number of consecutive bits to shift
 *
 */
static void shift_bits_right(uint8_t SRC[], uint16_t len, uint32_t shift) {
	uint32_t i = 0;

	uint8_t start = shift / 8;
	uint8_t rest = shift % 8;
	uint8_t previous = 0;

	for(i = 0; i < len; i++) {
		if(start <= i) {
			previous = SRC[i - start];
		}
		uint8_t value = (previous << (8 - rest)) | SRC[i + start] >> rest;
		SRC[i + start] = value;
	}
}

/**
 * logical XOR two bit arrays
 *
 * @param 	DST			the array to save the result in
 * @param 	SRC1		the array to compare with
 * @param 	SRC2		the array to compare with
 * @param 	len			the number of consecutive bits to compare
 *
 */
static void xor_bits(uint8_t DST[], uint8_t SRC1[], uint8_t SRC2[], uint32_t len) {
	uint32_t i;

	for(i = 0; i < len; i++) {
		DST[i / 8] |= (SRC1[i / 8] & (128 >> (i % 8) )) ^ (SRC2[i / 8] & (128 >> (i % 8) ));
	}
}

/**
 * logical AND two bit arrays
 *
 * @param 	DST			the array to save the result in
 * @param 	SRC1		the array to compare with
 * @param 	SRC2		the array to compare with
 * @param 	len			the number of consecutive bits to compare
 *
 */
static void and_bits(uint8_t DST[], uint8_t SRC1[], uint8_t SRC2[], uint32_t len) {
	uint32_t i;

	for(i = 0; i < len; i++) {
		DST[i / 8] |= (SRC1[i / 8] & (128 >> (i % 8) )) & (SRC2[i / 8] & (128 >> (i % 8) ));
	}
}

/**
 * print a bitmap
 *
 * @param bitmap		the bit array
 * @param len			the number of consecutive bits to print
 *
 */
static void print_bitmap(uint8_t bitmap[], uint32_t length) {
	uint32_t i;
	for (i = 0; i < length; i++) {
		uint8_t bit = bitmap[i / 8] & 128 >> (i % 8);
		printf("%d ", bit ? 1 : 0);
	}
	printf("\n"); // flush buffer
}

/**
 * get the FCN value
 *
 * @param  fragment		a pointer to the fragment to retrieve the FCN from
 *
 * @return FCN			the FCN as indicated by the fragment
 *
 * @note   only FCN values up to 16 bits are currently supported
 *
 */
static uint16_t get_fcn_value(uint8_t* fragment, schc_fragmentation_t* conn) {
	uint8_t offset = conn->RULE_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE;

	return (uint16_t) get_bits(fragment, offset, conn->FCN_SIZE);
}

/**
 * get the ALL-1 FCN value
 *
 * @return FCN			the all-1 fcn value
 *
 * @note   only FCN values up to 16 bits are currently supported
 *
 */
static uint16_t get_max_fcn_value(schc_fragmentation_t* conn) {
	uint8_t fcn[2] = { 0 };
	set_bits(fcn, 0, conn->FCN_SIZE);

	return (uint16_t) get_bits(fcn, 0, conn->FCN_SIZE);
}

/**
 * get the number of zero bits added to the end of the buffer
 *
 * @param byte			the byte to investigate
 *
 * @return padding		the length of the padding
 *
 */
static uint8_t get_padding_length(uint8_t byte) {
	uint8_t counter = 0; uint8_t i;
	for(i = 0; i < 8; i++) {
		if( !(byte & 1 << (i % 8)) ) {
			counter++;
		} else {
			break;
		}
	}

	return counter;
}


/**
 * get a bitmap mask for a number of bits
 *
 * @param len			the number of bits to set
 *
 * @return padding		the bitmask
 *
 */
static uint32_t get_bit_mask(uint8_t len) {
	int mask = 0; int i;

	for (i = 0; i < len; i++) {
	    mask = (1 << len) - 1;
	}

	return mask;
}

/**
 * print the complete mbuf chain
 *
 * @param  head			the head of the list
 *
 */
static void mbuf_print(schc_mbuf_t *head) {
	uint8_t i = 0; uint8_t j;
	schc_mbuf_t *curr = head;
	while (curr != NULL) {
		DEBUG_PRINTF("%d: 0x%X", curr->frag_cnt, curr->ptr);
		// DEBUG_PRINTF("0x%X", curr);
		for (j = 0; j < curr->len; j++) {
			printf("0x%02X ", curr->ptr[j]);
		}
		printf("\n");
		curr = curr->next;
		i++;
	}
}

/**
 * add an item to the end of the mbuf list
 * if head is NULL, the first item of the list
 * will be set
 *
 * @param head			the head of the list
 * @param data			a pointer to the data pointer
 * @param len			the length of the data
 *
 * @return	-1			no free mbuf slot was found
 * 			 0			ok
 */
static int8_t mbuf_push(schc_mbuf_t **head, uint8_t* data, uint16_t len) {
	// scroll to next free mbuf slot
	uint32_t i;
	for(i = 0; i < SCHC_CONF_MBUF_POOL_LEN; i++) {
		if(MBUF_POOL[i].len == 0 && MBUF_POOL[i].ptr == NULL) {
			break;
		}
	}

	if(i == SCHC_CONF_MBUF_POOL_LEN) {
		DEBUG_PRINTF("mbuf_push(): no free mbuf slots found");
		return SCHC_FAILURE;
	}

	DEBUG_PRINTF("mbuf_push(): selected mbuf slot %d", i);

	// check if this is a new connection
	if(*head == NULL) {
		*head = &MBUF_POOL[i];
		(*head)->len = len;
		(*head)->ptr = (uint8_t*) (data);
		(*head)->next = NULL;
		(*head)->slot = i;
		return SCHC_SUCCESS;
	}

	MBUF_POOL[i].slot = i;
	MBUF_POOL[i].next = NULL;
	MBUF_POOL[i].len = len;
	MBUF_POOL[i].ptr = (uint8_t*) (data);

	// find the last mbuf in the chain
	schc_mbuf_t *curr = *head;
	while (curr->next != NULL) {
		curr = curr->next;
	}

	// set next in chain
	curr->next = (schc_mbuf_t*) (MBUF_POOL + i);

	return SCHC_SUCCESS;
}

/**
 * returns the last chain in the mbuf linked list
 *
 * @param  head			the head of the list
 * @param  mbuf			the mbuf to find the previous mbuf for
 *
 * @return prev			the previous mbuf
 */
static schc_mbuf_t* get_prev_mbuf(schc_mbuf_t *head, schc_mbuf_t *mbuf) {
	schc_mbuf_t *curr = head;

	while (curr->next != mbuf) {
		DEBUG_PRINTF(
				"head is 0x%x, looking for 0x%x with curr 0x%x, next is 0x%x",
				head, mbuf, curr, curr->next);
		curr = curr->next;
	}

	return curr;
}

/**
 * delete a mbuf from the chain
 *
 * @param  head			the head of the list
 * @param  mbuf			the mbuf to delete
 *
 */
static void mbuf_delete(schc_mbuf_t **head, schc_mbuf_t *mbuf) {
	uint32_t slot = 0;

	slot = mbuf->slot;
	schc_mbuf_t *prev = NULL;

	if(mbuf->next != NULL) {
		if(mbuf == *head) {
			DEBUG_PRINTF("mbuf_delete(): set head");
			(*head) = mbuf->next;
		}
	} else {
		if(mbuf == *head) { // head is last fragment
			DEBUG_PRINTF("mbuf_delete(): mbuf is head, delete head");
			(*head) = NULL;
		} else {
			DEBUG_PRINTF("mbuf_delete(): chain next to prev");
			prev = get_prev_mbuf(*head, mbuf);
			prev->next = mbuf->next;
		}
	}

	DEBUG_PRINTF("mbuf_delete(): clear slot %d in mbuf pool", slot);
#if DYNAMIC_MEMORY
	free(mbuf->ptr);
#else
	memset(mbuf->ptr, 0, mbuf->len);
#endif

	// clear slot in mbuf pool
	MBUF_POOL[slot].next = NULL;
	MBUF_POOL[slot].frag_cnt = 0;
	MBUF_POOL[slot].len = 0;
	MBUF_POOL[slot].ptr = NULL;
}

/**
 * check if an mbuf with the same fragment number already exists
 * and overwrite if so
 *
 * @param  	head			the head of the list
 * @param  	frag			the fragment number to overwrite
 * @param	mbuf			the fragment to overwrite with
 *
 * @return 	0				no matching fragment found
 * 			1				overwrote a matching packet
 */
static uint8_t mbuf_overwrite(schc_mbuf_t **head, uint16_t frag, schc_mbuf_t* mbuf) {
	schc_mbuf_t *curr = *head;

	while (curr->next != NULL) {
		if(curr->frag_cnt == frag) {
			mbuf_delete(head, curr);
			return 1;
		}
		curr = curr->next;
	}

	return 0;
}

/**
 * returns the total length of the mbuf
 *
 * @param  head			the head of the list
 *
 * @return len			the total length of the fragment
 */
uint16_t get_mbuf_len(schc_mbuf_t *head) {
	schc_mbuf_t *curr = head; uint32_t total_len = 0; uint32_t total_offset = 0;

	while (curr != NULL) {
		total_len += (curr->len * 8);
		total_offset += curr->offset;

		curr = curr->next;
	}

	return ((total_len - total_offset) / 8);
}

/**
 * returns the last chain in the mbuf linked list
 *
 * @param  head			the head of the list
 *
 * @return tail			the last mbuf in the linked list
 */
static schc_mbuf_t* get_mbuf_tail(schc_mbuf_t *head) {
	schc_mbuf_t *curr = head;

	if(head == NULL) {
		return NULL;
	}

	while (curr->next != NULL) {
		curr = curr->next;
	}

	return curr;
}

/**
 * copy the byte alligned contents of the mbuf chain to
 * the passed pointer
 *
 * @param  head			the head of the list
 * @param  ptr			the pointer to copy the contents to
 */
void mbuf_copy(schc_mbuf_t *head, uint8_t* ptr) {
	schc_mbuf_t *curr = head; uint16_t pos = 0;

	while (curr != NULL) {
		memcpy((uint8_t*) (ptr + pos), (uint8_t*) (curr->ptr + pos), curr->len);
		pos += curr->len;

		curr = curr->next;
	}
}


/**
 * delete all fragments chained in an mbuf
 *
 * @param  head			the head of the list
 */
void mbuf_clean(schc_mbuf_t **head) {
	schc_mbuf_t *curr = *head;
	schc_mbuf_t *temp = NULL;

	while (curr != NULL) {
		temp = curr->next;
		mbuf_delete(head, curr);
		curr = temp;
	}
}


/**
 * sort the complete mbuf chain based on fragment counter
 *
 * @param  head			double pointer to the head of the list
 *
 */
static void mbuf_sort(schc_mbuf_t **head) {
	schc_mbuf_t *hd = *head;
	*head = NULL;

	while (hd != NULL) {
		schc_mbuf_t **curr = &hd;
		schc_mbuf_t **next = &hd->next;
		uint8_t swapped = 0;

		while (*next != NULL) {
			if ((*next)->frag_cnt < (*curr)->frag_cnt) { // swap pointers for curr and curr->next
				schc_mbuf_t **temp;
				temp = *curr;
				*curr = *next;
				*next = temp;

				temp = (*curr)->next;
				(*curr)->next = (*next)->next;
				(*next)->next = temp;

				curr = &(*curr)->next;
				swapped = 1;
			} else {   // no swap. advance both pointer-pointers
				curr = next;
				next = &(*next)->next;
			}
		}

		*next = *head;
		if (swapped) {
			*head = *curr;
			*curr = NULL;
		} else {
			*head = hd;
			break;
		}
	}
}

/**
 * remove the fragmentation headers and
 * concat the data bits of the complete mbuf chain
 *
 * @param  head			double pointer to the head of the list
 *
 */
static void mbuf_format(schc_mbuf_t **head, schc_fragmentation_t* conn) {
	schc_mbuf_t **curr = &(*head);
		schc_mbuf_t **next = &((*head)->next);
		schc_mbuf_t **prev = NULL;

		uint8_t i = 0; uint8_t counter = 1; uint16_t total_bits_shifted = 0;

		while (*curr != NULL) {
			uint8_t fcn = get_fcn_value((*curr)->ptr, conn);
			uint32_t offset = conn->RULE_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE
					+ conn->FCN_SIZE; uint8_t overflow = 0;


			if(prev == NULL) { // first
				(*curr)->offset = offset - conn->RULE_SIZE;

				uint8_t rule_id[RULE_SIZE_BYTES] = { 0 }; // get rule id
				copy_bits(rule_id, 0, (*curr)->ptr, 0, conn->RULE_SIZE);

				shift_bits_left((*curr)->ptr, (*curr)->len, (*curr)->offset); // shift left

				clear_bits((*curr)->ptr, 0, conn->RULE_SIZE); // set rule id at first position
				copy_bits((*curr)->ptr, 0, rule_id, 0, conn->RULE_SIZE);

				total_bits_shifted += (*curr)->offset;
			} else { // normal
				if (fcn == get_max_fcn_value(conn)) {
					offset += (MIC_SIZE_BYTES * 8);
				}

				int16_t start = ((*prev)->len * 8) - (*prev)->offset;
				int16_t room_left = ((*prev)->len * 8) - start;
				int16_t bits_to_copy = (*curr)->len * 8 - offset;

				// copy (part of) curr buffer to prev
				clear_bits((*prev)->ptr, ((*prev)->len * 8) -  (*prev)->offset, (*prev)->offset);
				copy_bits((*prev)->ptr, ((*prev)->len * 8) -  (*prev)->offset, (*curr)->ptr, offset, (*prev)->offset);

				if(room_left > bits_to_copy) {
					// do not advance pointer and merge prev and curr in one buffer
					(*prev)->offset = start + offset;
					if((*curr)->next != NULL) {
						(*prev)->next = (*curr)->next;
						curr = next;
					}
					overflow = 1;
				} else {
					// shift bits left
					shift_bits_left((*curr)->ptr, (*curr)->len, offset + (*prev)->offset); // shift left to remove headers and bits that were copied
					overflow = 0;
				}

				(*curr)->offset = offset;
			}

			if(!overflow) { // do not advance prev if this contains parts of 3 fragments
				if(prev != NULL) {
					prev = &(*prev)->next; // could be that we skipped a buffer
				} else {
					prev = curr;
				}
			}

			i++;

			curr = next; // advance both pointer-pointers
			next = &(*next)->next;
		}
}


/**
 * Returns the number of bits the current header exists off
 *
 * @param  mbuf 		the mbuf to find th offset for
 *
 * @return length 		the length of the header
 *
 */
static uint8_t get_header_length(schc_mbuf_t *mbuf, schc_fragmentation_t* conn) {
	uint32_t offset = conn->RULE_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE
			+ conn->FCN_SIZE;

	uint8_t fcn = get_fcn_value(mbuf->ptr, conn);

	if (fcn == get_max_fcn_value(conn)) {
		offset += (MIC_SIZE_BYTES * 8);
	}

	return offset;
}

/**
 * Calculates the Message Integrity Check (MIC) over an unformatted mbuf chain
 * which is the 8- 16- or 32- bit Cyclic Redundancy Check (CRC)
 *
 * @param  head			the head of the list
 *
 * @return checksum 	the computed checksum
 *
 */
static unsigned int mbuf_compute_mic(schc_fragmentation_t *conn) {
	schc_mbuf_t *curr = conn->head;
	schc_mbuf_t *prev = NULL;

	int i, j, k;
	uint32_t offset = 0;
	uint8_t first = 1; uint8_t last = 0;
	uint16_t len;
	uint8_t start, rest, byte;
	uint8_t prev_offset = 0;
	uint32_t crc, crc_mask;

	crc = 0xFFFFFFFF;

	while (curr != NULL) {
		uint8_t fcn = get_fcn_value(curr->ptr, conn);
		uint8_t cont = 1;
		offset = (get_header_length(curr, conn) + prev_offset);

		i = offset;
		len = (curr->len * 8);
		start = offset / 8;
		rest = offset % 8;
		j = start;

		while (cont) {
			if (prev == NULL && first) { // first byte(s) originally contained the rule id
				byte = (curr->ptr[0] << (8 - conn->RULE_SIZE))
						| (curr->ptr[1] >> conn->RULE_SIZE);
				prev_offset = (RULE_SIZE_BYTES * 8) - conn->RULE_SIZE;
				first = 0;
			} else {
				i += 8;
				if (i >= len) {
					if (curr->next != NULL) {
						uint8_t next_header_len = get_header_length(curr->next, conn);
						uint8_t start_next = next_header_len / 8;
						uint8_t start_offset = next_header_len % 8;
						uint8_t remainder = ((8 - rest) + (8 - start_offset));

						uint8_t mask1 = get_bit_mask((8 - start_offset));

						if(remainder < 8) {
							byte = (curr->ptr[j] << rest) | ((curr->next->ptr[start_next] & mask1) << (8 - remainder)) | ((curr->next->ptr[start_next + 1] >> remainder));
						} else {
							uint8_t mask2 = get_bit_mask((8 - start_offset));
							byte = (curr->ptr[j] << rest) | ((curr->next->ptr[start_next] & mask2) >> ((remainder - (8 - rest)) - rest));
						}

						if(curr->next->next == NULL && byte == 0x00) { // hack
							// todo
							last = 1;
						}

						prev_offset = (i - len);
					} else {
						last = 1;
					}
					cont = 0;
				} else {
					byte = (curr->ptr[j] << rest)
							| (curr->ptr[j + 1] >> (8 - rest));
				}

				j++;
			}

			if (!last) { // todo
				// hack in order not to include padding
				crc = crc ^ byte;
				for (k = 7; k >= 0; k--) {    // do eight times.
					crc_mask = -(crc & 1);
					crc = (crc >> 1) ^ (0xEDB88320 & crc_mask);
				}
				// printf("0x%02X ", byte);
			}
		}
		// printf("\n");

		prev = curr;
		curr = curr->next;
	}

	crc = ~crc;
	uint8_t mic[MIC_SIZE_BYTES] = { ((crc & 0xFF000000) >> 24),
			((crc & 0xFF0000) >> 16), ((crc & 0xFF00) >> 8), ((crc & 0xFF)) };

	memcpy((uint8_t *) conn->mic, mic, MIC_SIZE_BYTES);

	DEBUG_PRINTF("compute_mic(): MIC is %02X%02X%02X%02X \n", mic[0], mic[1], mic[2],
			mic[3]);

	return crc;
}


////////////////////////////////////////////////////////////////////////////////////
//                                LOCAL FUNCIONS                                  //
////////////////////////////////////////////////////////////////////////////////////

/**
 * Calculates the Message Integrity Check (MIC)
 * which is the 8- 16- or 32- bit Cyclic Redundancy Check (CRC)
 *
 * @param conn 			pointer to the connection
 *
 * @return checksum 	the computed checksum
 *
 */
static unsigned int compute_mic(schc_fragmentation_t *conn) {
	int i, j; uint8_t byte;
	unsigned int crc, mask;

	// ToDo
	// check conn->mic length
	// and calculate appropriate crc

	i = 0;
	crc = 0xFFFFFFFF;

	uint16_t len = (conn->tail_ptr - conn->data_ptr);

	while (i < len) {
		byte = conn->data_ptr[i];
		crc = crc ^ byte;
		for (j = 7; j >= 0; j--) {    // do eight times.
			mask = -(crc & 1);
			crc = (crc >> 1) ^ (0xEDB88320 & mask);
		}
		i++;
	}

	crc = ~crc;
	uint8_t mic[MIC_SIZE_BYTES] = { ((crc & 0xFF000000) >> 24), ((crc & 0xFF0000) >> 16),
			((crc & 0xFF00) >> 8), ((crc & 0xFF)) };

	memcpy((uint8_t *) conn->mic, mic, MIC_SIZE_BYTES);

	DEBUG_PRINTF("compute_mic(): MIC for device %d is %02X%02X%02X%02X \n",
			conn->device_id, mic[0], mic[1], mic[2], mic[3]);

	return crc;
}

/**
 * get the window bit
 *
 * @param fragment		a pointer to the fragment to retrieve the window number from
 *
 * @return window		the window number as indicated by the fragment
 *
 */
static uint8_t get_window_bit(uint8_t* fragment, schc_fragmentation_t* conn) {
	uint8_t offset = conn->RULE_SIZE + conn->DTAG_SIZE;

	return (uint8_t) get_bits(fragment, offset, conn->WINDOW_SIZE);
}

/**
 * get the MIC value
 *
 * @param  fragment		a pointer to the fragment to retrieve the MIC from
 * @param  mic
 *
 */
static void get_received_mic(uint8_t* fragment, uint8_t mic[], schc_fragmentation_t* conn) {
	uint8_t offset = conn->RULE_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE + conn->FCN_SIZE;

	copy_bits(mic, 0, fragment, offset, (MIC_SIZE_BYTES * 8));
}

/**
 * set the fragmentation counter of the current connection
 * which is the inverse of the fcn value
 *
 * @param  conn			a pointer to the connection
 * @param  frag			the fcn value
 *
 */
static void set_conn_frag_cnt(schc_fragmentation_t* conn, uint8_t frag) {
	uint8_t value = conn->MAX_WND_FCN - frag;
	if(frag == get_max_fcn_value(conn)) {
		value = (conn->window_cnt + 1) * get_max_fcn_value(conn);
	} else {
		value += (conn->window_cnt * (conn->MAX_WND_FCN + 1));
	}

	DEBUG_PRINTF("value is %d frag is %d", value, frag);

	conn->frag_cnt = value;
}

/**
 * initializes a new tx transmission for a device:
 * set the starting and ending point of the packet
 * calculate the MIC over the complete SCHC packet
 *
 * @param conn 				a pointer to the connection to initialize
 *
 * @return	 1				on success
 * 			 0				on error
 * 			-1				if no fragmentation is needed
 *
 */
static int8_t init_tx_connection(schc_fragmentation_t* conn) {
	if (!conn->data_ptr) {
		DEBUG_PRINTF(
				"init_connection(): no pointer to compressed packet given");
		return 0;
	}
	if (!conn->mtu) {
		DEBUG_PRINTF("init_connection(): no mtu specified");
		return 0;
	}
	if (conn->mtu > MAX_MTU_LENGTH) {
		DEBUG_PRINTF(
				"init_connection(): MAX_MTU_LENGTH should be set according to conn->mtu");
		return 0;
	}
	if (!conn->packet_len) {
		DEBUG_PRINTF("init_connection(): packet_length not specified");
		return 0;
	}
	if(conn->packet_len < conn->mtu) {
		DEBUG_PRINTF("init_connection(): no fragmentation needed");
		return -1;
	}
	if (conn->send == NULL) {
		DEBUG_PRINTF("init_connection(): no send function specified");
		return 0;
	}
	if (conn->post_timer_task == NULL) {
		DEBUG_PRINTF("init_connection(): no timer function specified");
		return 0;
	}
	if(conn->MAX_WND_FCN >= get_max_fcn_value(conn)) {
		DEBUG_PRINTF("init_connection(): MAX_WIND_FCN must be smaller than all-1");
		return 0;
	}
	if(!conn->mode) {
		DEBUG_PRINTF("init_connection(): no reliability mode specified");
		return 0;
	}
	if(conn->mode == NO_ACK) {
		conn->FCN_SIZE = 1;
		conn->WINDOW_SIZE = 0;
	}

	memcpy(conn->rule_id, (uint8_t*) (conn->data_ptr + 0), RULE_SIZE_BYTES); // set rule id

	conn->tail_ptr = (uint8_t*) (conn->data_ptr + conn->packet_len); // set end of packet

	conn->window = 0;
	conn->window_cnt = 0;
	memset(conn->bitmap, 0, BITMAP_SIZE_BYTES); // clear bitmap
	conn->fcn = conn->MAX_WND_FCN;
	conn->frag_cnt = 0;
	conn->attempts = 0;

	compute_mic(conn); // calculate MIC over compressed, unfragmented packet

	return 1;
}

/**
 * reset a connection
 *
 * @param conn 			a pointer to the connection to reset
 *
 */
void schc_reset(schc_fragmentation_t* conn) {
	/* reset connection variables */
	conn->device_id = 0;
	conn->packet_len = 0;
	conn->data_ptr = 0;
	conn->tail_ptr = 0;
	conn->dc = 0;
	conn->mtu = 0;
	conn->fcn = 0;
	conn->dtag = 0;
	conn->frag_cnt = 0;
	memset(conn->bitmap, 0, BITMAP_SIZE_BYTES);
	conn->attempts = 0;
	conn->TX_STATE = INIT_TX;
	conn->RX_STATE = RECV_WINDOW;
	conn->window = 0;
	conn->window_cnt = 0;
	conn->timer_flag = 0;
	conn->input = 0;
	memset(conn->rule_id, 0, RULE_SIZE_BYTES);
	memset(conn->mic, 0, MIC_SIZE_BYTES);

	/* reset ack structure */
	memset(conn->ack.rule_id, 0, RULE_SIZE_BYTES);
	memset(conn->ack.bitmap, 0, BITMAP_SIZE_BYTES);
	memset(conn->ack.window, 0, 1);
	memset(conn->ack.dtag, 0, 1);
	conn->ack.mic = 0;
	conn->ack.fcn = 0;

	if(conn->head != NULL ){
		mbuf_clean(&conn->head);
	}
	conn->head = NULL;
}

/**
 * check if a connection has more fragments to deliver
 *
 * @param conn 					a pointer to the connection
 *
 * @return	0					the connection still has fragments to send
 * 			total_bit_offset	the total bit offset inside the packet
 *
 */
static uint32_t has_no_more_fragments(schc_fragmentation_t* conn) {
	uint8_t total_fragments = ((conn->tail_ptr - conn->data_ptr) / conn->mtu);

	if (conn->frag_cnt > total_fragments) { // this is the last packet
		uint16_t bit_offset = conn->RULE_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE
				+ conn->FCN_SIZE + (MIC_SIZE_BYTES * 8); // fragmentation header bits
		uint32_t total_bit_offset = ((conn->mtu * 8)
				- (conn->RULE_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE
						+ conn->FCN_SIZE)) * (conn->frag_cnt - 1); // packet bit offset
		uint16_t total_byte_offset = total_bit_offset / 8;
		uint8_t remaining_bit_offset = total_bit_offset % 8;

		int16_t packet_len = conn->tail_ptr - (conn->data_ptr
				+ total_byte_offset)
				+ (ceil((bit_offset + remaining_bit_offset) / 8));

		// check if fragmentation header created a longer packet than allowed
		if (packet_len <= conn->mtu) {
			return total_bit_offset;
		}
	}

	return 0;
}

/**
 * set the fragmentation header
 *
 * @param conn 			a pointer to the connection
 * @param buffer		a pointer to the buffer to set the header
 *
 * @return bit_offset	the number of bits added to the front of the fragment
 *
 */
static uint16_t set_fragmentation_header(schc_fragmentation_t* conn,
		uint8_t* fragmentation_buffer) {
	uint8_t bit_offset = conn->RULE_SIZE;

	 // set rule id
	copy_bits(fragmentation_buffer, 0, conn->rule_id, 0, bit_offset);

	// set dtag field
	uint8_t dtag[1] = { conn->dtag << (8 - conn->DTAG_SIZE) };
	copy_bits(fragmentation_buffer, bit_offset, dtag, 0, conn->DTAG_SIZE); // right after rule id

	bit_offset += conn->DTAG_SIZE;

	// set window bit
	uint8_t window[1] = { conn->window << (8 - conn->WINDOW_SIZE) };
	copy_bits(fragmentation_buffer, bit_offset, window, 0, conn->WINDOW_SIZE); // right after dtag

	bit_offset += conn->WINDOW_SIZE;

	// set fcn value
	uint8_t fcn[1] = { conn->fcn << (8 - conn->FCN_SIZE) };
	copy_bits(fragmentation_buffer, bit_offset, fcn, 0, conn->FCN_SIZE); // right after window bits

	bit_offset += conn->FCN_SIZE;

	if (has_no_more_fragments(conn)) { // all-1 fragment
		// shift in MIC
		copy_bits(fragmentation_buffer, bit_offset, conn->mic, 0, (MIC_SIZE_BYTES * 8));
		bit_offset += (MIC_SIZE_BYTES * 8);
	}

	return bit_offset;
}

/**
 * sets the local bitmap at the current fragment offset
 * without encoding the bitmap
 *
 *
 * @param conn 			a pointer to the connection
 *
 */
static void set_local_bitmap(schc_fragmentation_t* conn) {
	int8_t frag = (((conn->MAX_WND_FCN + 1) - conn->fcn) - 1);
	if(frag < 0) {
		frag = conn->MAX_WND_FCN;
	}
	set_bits(conn->bitmap, frag, 1);

	DEBUG_PRINTF("set_local_bitmap(): for fcn %d at index %d \n", conn->fcn, frag);
	print_bitmap(conn->bitmap, conn->MAX_WND_FCN + 1);
}

/**
 * clear the received and local bitmap
 *
 * @param conn 			a pointer to the connection
 *
 */
static void clear_bitmap(schc_fragmentation_t* conn) {
	memset(conn->bitmap, 0, BITMAP_SIZE_BYTES); // clear local bitmap
	memset(conn->ack.bitmap, 0, BITMAP_SIZE_BYTES); // clear received bitmap
}

/**
 * encode the bitmap by removing all the right
 * most contiguous BYTES in the non-encoded bitmap
 *
 * @param conn 			a pointer to the connection
 *
 */
static void encode_bitmap(schc_fragmentation_t* conn) {
	// ToDo
}

/**
 * reconstruct an encoded bitmap
 *
 * @param conn 			a pointer to the connection
 *
 */
static void decode_bitmap(schc_fragmentation_t* conn) {
	// ToDo
}

/**
 * loop over a bitmap to check if all bits are set to
 * 1, starting from MAX_WIND_FCN
 *
 * @param conn 			a pointer to the connection
 * @param len			the length of the bitmap
 *
 */
static uint8_t is_bitmap_full(schc_fragmentation_t* conn, uint8_t len) {
	uint8_t i;
	for (i = 0; i < len; i++) {
		if (!(conn->bitmap[i / 8] & 128 >> (i % 8))) {
			return 0;
		}
	}
	return 1;
}

/**
 * get the next fragment to retransmit according the fragmentation counter
 *
 * @param conn 			a pointer to the connection
 *
 * @return  frag		the next fragment to retransmit
 * 			0			no more fragments to retransmit
 *
 */
static uint16_t get_next_fragment_from_bitmap(schc_fragmentation_t* conn) {
	uint16_t i;

	uint8_t start = (conn->frag_cnt) - ((conn->MAX_WND_FCN + 1)* conn->window_cnt);
	for (i = start; i <= conn->MAX_WND_FCN; i++) {
		uint8_t bit = conn->ack.bitmap[i / 8] & 128 >> (i % 8);
		if(bit) {
			return (i + 1);
		}
	}

	return 0;
}
/**
 * discard a fragment
 *
 * @param conn 			a pointer to the connection
 *
 */
static void discard_fragment(schc_fragmentation_t* conn) {
	DEBUG_PRINTF("discard_fragment():");
	schc_mbuf_t* tail = get_mbuf_tail(conn->head); // get last received fragment
	mbuf_delete(&conn->head, tail);
	return;
}

/**
 * abort an ongoing transmission because the
 * inactivity timer has expired
 *
 * @param conn 			a pointer to the connection
 *
 */
static void abort_connection(schc_fragmentation_t* conn) {
	// todo
	DEBUG_PRINTF("abort_connection(): inactivity timer expired");
	schc_reset(conn);
	return;
}

/**
 * sets the retransmission timer to re-enter the fragmentation loop
 * and changes the retransmission_timer flag
 *
 * @param conn 			a pointer to the connection
 *
 */
static void set_retrans_timer(schc_fragmentation_t* conn) {
	conn->timer_flag = 1;
	DEBUG_PRINTF("set_retrans_timer(): for %d ms", conn->dc * 4);
	conn->post_timer_task(&schc_fragment, conn->device_id, conn->dc * 4, conn);
}

/**
 * sets the duty cycle timer to re-enter the fragmentation loop
 *
 * @param conn 			a pointer to the connection
 *
 */
static void set_dc_timer(schc_fragmentation_t* conn) {
	DEBUG_PRINTF("set_dc_timer(): for %d ms", conn->dc);
	conn->post_timer_task(&schc_fragment, conn->device_id, conn->dc, conn);
}

/**
 * sets the inactivity timer to re-enter the fragmentation loop
 * and changes the retransmission_timer flag
 *
 * @param conn 			a pointer to the connection
 *
 */
static void set_inactivity_timer(schc_fragmentation_t* conn) {
	conn->timer_flag = 1;
	DEBUG_PRINTF("set_inactivity_timer(): for %d ms", conn->dc);
	conn->post_timer_task(&schc_reassemble, conn->device_id, conn->dc, conn);
}

/**
 * checks if the fragment inside the mbuf is
 * an all-0 empty
 *
 * @param mbuf 			a pointer to the mbuf
 *
 * @return 	0			this is not an empty all-0
 * 			1			this is an empty all-0
 *
 */
static uint8_t empty_all_0(schc_mbuf_t* mbuf, schc_fragmentation_t* conn) {
	uint8_t offset = conn->RULE_SIZE + conn->FCN_SIZE + conn->DTAG_SIZE + conn->WINDOW_SIZE;
	uint8_t len = (mbuf->len * 8);

	if((len - offset) > 8) { // if number of bits is larger than 8, there was payload
		return 0;
	}
	return 1;
}

/**
 * checks if the fragment inside the mbuf is
 * an all-1 empty
 *
 * @param mbuf 			a pointer to the mbuf
 *
 * @return 	0			this is not an empty all-1
 * 			1			this is an empty all-1
 *
 */
static uint8_t empty_all_1(schc_mbuf_t* mbuf, schc_fragmentation_t* conn) {
	uint8_t offset = conn->RULE_SIZE + conn->FCN_SIZE + conn->DTAG_SIZE
			+ conn->WINDOW_SIZE + (MIC_SIZE_BYTES * 8);
	uint8_t len = (mbuf->len * 8);

	if ((len - offset) > 8) { // if number of bits is larger than 8, there was payload
		return 0;
	}
	return 1;
}

/**
 * composes a packet based on the type of the packet
 * and calls the callback function to transmit the packet
 *
 * @param 	conn 			a pointer to the connection
 *
 * @ret		0				the packet was not sent
 * 			1				the packet was transmitted
 *
 */
static uint8_t send_fragment(schc_fragmentation_t* conn) {
	memset(fragmentation_buffer, 0, MAX_MTU_LENGTH); // set and reset buffer

	uint16_t header_offset = set_fragmentation_header(conn, fragmentation_buffer); // set fragmentation header
	uint16_t packet_bit_offset = has_no_more_fragments(conn);

	uint16_t packet_len = 0; uint16_t total_byte_offset; uint8_t remaining_bit_offset;
	uint16_t total_bit_offset = ((conn->tail_ptr - conn->data_ptr) * 8);

	if(!packet_bit_offset) { // normal fragment
		packet_len = conn->mtu;
		packet_bit_offset = ((conn->mtu * 8) - header_offset) * (conn->frag_cnt - 1); // the number of bits left to copy

		if( (((conn->tail_ptr - conn->data_ptr) * 8) - packet_bit_offset) < (packet_len * 8) ) { // special case when mic is sent in the next packet seperately
			packet_len = ((conn->tail_ptr - conn->data_ptr) - (packet_bit_offset / 8)) + 1;
		}
	}

	int32_t packet_bits = ((packet_len * 8) - header_offset);

	total_byte_offset = packet_bit_offset / 8;
	remaining_bit_offset = (packet_bit_offset % 8);

	uint8_t padding = 0;

	if (!packet_len) { // all-1 fragment

		packet_bits = (((conn->tail_ptr - conn->data_ptr) * 8)
				- ((total_byte_offset * 8) + remaining_bit_offset))
				- conn->RULE_SIZE; // rule was not sent and is thus deducted from the total length

		if(packet_bits < 0) { // MIC will be send in a seperate packet
			packet_bits = conn->RULE_SIZE + conn->WINDOW_SIZE + conn->FCN_SIZE + conn->DTAG_SIZE + (MIC_SIZE_BYTES * 8);
			header_offset = 0; // header offset is included in packet bits now
		}

		padding = (8 - ((header_offset + packet_bits) % 8));

		// todo
		// check if last byte contains 0x0
		// because padding is added

		uint8_t zerobuf[1] = { 0 };
		copy_bits(fragmentation_buffer, header_offset + packet_bits, zerobuf, 0, padding); // add padding

		packet_len = (padding + header_offset + packet_bits) / 8; // last packet length
	}

	copy_bits(fragmentation_buffer, header_offset,
				(conn->data_ptr + total_byte_offset),
				(remaining_bit_offset + conn->RULE_SIZE), packet_bits); // copy bits

	// if(conn->frag_cnt != 10 || ATTEMPTS == 1) {
		DEBUG_PRINTF("send_fragment(): sending fragment %d with length %d to device %d",
			conn->frag_cnt, packet_len, conn->device_id);
		return conn->send(fragmentation_buffer, packet_len, conn->device_id);
	/*} else {
		ATTEMPTS++;
	}*/
}

/**
 * composes an ack based on the parameters found in the connection
 * and calls the callback function to transmit the packet
 *
 * @param conn 			a pointer to the connection
 *
 * @ret		0				the packet was not sent
 * 			1				the packet was transmitted
 *
 */
static uint8_t send_ack(schc_fragmentation_t* conn) {
	uint8_t ack[RULE_SIZE_BYTES + DTAG_SIZE_BYTES + BITMAP_SIZE_BYTES] = { 0 };
	uint8_t offset = conn->RULE_SIZE;

	copy_bits(ack, 0, conn->ack.rule_id, 0, offset); // set rule id
	copy_bits(ack, offset, conn->ack.dtag, 0, conn->DTAG_SIZE); // set dtag
	offset += conn->DTAG_SIZE;

	uint8_t window[1] = { conn->window << (8 - conn->WINDOW_SIZE) }; // set window
	copy_bits(ack, offset, window, 0, conn->WINDOW_SIZE);
	offset += conn->WINDOW_SIZE;

	if(conn->ack.fcn == get_max_fcn_value(conn)) { // all-1 window
		uint8_t c[1] = { conn->ack.mic << (8 - MIC_C_SIZE_BITS) }; // set mic c bit
		copy_bits(ack, offset, c, 0, MIC_C_SIZE_BITS);
		offset += MIC_C_SIZE_BITS;
	}

	if(!conn->ack.mic) { // if mic c bit is 0 (zero by default)
		DEBUG_PRINTF("send_ack(): sending bitmap");
		copy_bits(ack, offset, conn->bitmap, 0, conn->MAX_WND_FCN + 1); // copy the bitmap
		offset += conn->MAX_WND_FCN + 1; // todo must be encoded
		print_bitmap(conn->bitmap, conn->MAX_WND_FCN + 1);
	}

	uint8_t packet_len = ((offset - 1) / 8) + 1;
	DEBUG_PRINTF("send_ack(): sending ack to device %d for fragment %d with length %d (%d b)",
			conn->device_id, conn->frag_cnt + 1, packet_len, offset);

	int i;
	for(i = 0; i < packet_len; i++) {
		printf("%02X ", ack[i]);
	}

	DEBUG_PRINTF("");

	return conn->send(ack, packet_len, conn->device_id);
}

/**
 * composes an all-empty fragment based on the parameters
 * found in the connection
 * and calls the callback function to transmit the packet
 *
 * @param conn 			a pointer to the connection
 *
 * @ret		0				the packet was not sent
 * 			1				the packet was transmitted
 *
 */
static uint8_t send_empty(schc_fragmentation_t* conn) {
	// set and reset buffer
	memset(fragmentation_buffer, 0, MAX_MTU_LENGTH);

	// set fragmentation header
	uint16_t header_offset = set_fragmentation_header(conn, fragmentation_buffer);

	uint8_t padding = header_offset % 8;
	uint8_t zerobuf[1] = { 0 };
	copy_bits(fragmentation_buffer, header_offset, zerobuf, 0, padding); // add padding

	uint8_t packet_len = (padding + header_offset) / 8;

	DEBUG_PRINTF("send_empty(): sending all-x empty to device %d with length %d (%d b)",
			conn->device_id, packet_len, header_offset);

	return conn->send(fragmentation_buffer, packet_len, conn->device_id);
}

/**
 * composes an all-empty fragment based on the parameters
 * found in the connection
 * and calls the callback function to transmit the packet
 *
 * @param conn 			a pointer to the connection
 *
 * @ret		0				the packet was not sent
 * 			1				the packet was transmitted
 *
 */
static uint8_t send_tx_empty(schc_fragmentation_t* conn) {
	DEBUG_PRINTF("send_tx_empty()");
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////
//                               GLOBAL FUNCIONS                                  //
////////////////////////////////////////////////////////////////////////////////////

/**
 * find a connection based on a device id
 * or open a new connection if there was no connection
 * for this device yet
 *
 * @param 	device_id	the id of the device to open a connection for
 *
 * @return 	conn		a pointer to the selected connection
 * 			0 			if no free connections are available
 *
 */
schc_fragmentation_t* schc_get_connection(uint32_t device_id) {
	uint8_t i; schc_fragmentation_t *conn;
	conn = 0;

	for (i = 0; i < SCHC_CONF_RX_CONNS; i++) {
		// first look for the the old connection
		if (schc_rx_conns[i].device_id == device_id) {
			conn = &schc_rx_conns[i];
			break;
		}
	}

	if (conn == 0) { // check if we were given an old connection
		for (i = 0; i < SCHC_CONF_RX_CONNS; i++) {
			if (schc_rx_conns[i].device_id == 0) { // look for an empty connection
				conn = &schc_rx_conns[i];
				schc_rx_conns[i].device_id = device_id;
				break;
			}
		}
	}

	if(conn) {
		DEBUG_PRINTF("schc_get_connection(): selected connection %d for device %d", i, device_id);
	}

	return conn;
}


/**
 * sort the mbuf chain, find the MIC inside the last received fragment
 * and compare with the calculated one
 *
 * @param 	rx_conn		a pointer to the rx connection structure
 *
 */
static int8_t mic_correct(schc_fragmentation_t* rx_conn) {
	uint8_t recv_mic[MIC_SIZE_BYTES] = { 0 };

	mbuf_sort(&rx_conn->head); // sort the mbuf chain

	schc_mbuf_t* tail = get_mbuf_tail(rx_conn->head); // get new tail before looking for mic

	if (tail == NULL) { // hack
		// rx_conn->timer_flag or rx_conn->input has not been changed
		abort_connection(rx_conn); // todo
		return -1;
	}

	get_received_mic(tail->ptr, recv_mic, rx_conn);
	DEBUG_PRINTF("MIC is %02X%02X%02X%02X", recv_mic[0], recv_mic[1],
			recv_mic[2], recv_mic[3]);

	mbuf_print(rx_conn->head);
	mbuf_compute_mic(rx_conn); // compute the mic over the mbuf chain

	if (!compare_bits(rx_conn->mic, recv_mic, (MIC_SIZE_BYTES * 8))) { // mic wrong
		return 0;
	}

	return 1;
}


/**
 * the function to call when the state machine is in WAIT END state
 *
 * @param 	rx_conn		a pointer to the rx connection structure
 *
 */
static uint8_t wait_end(schc_fragmentation_t* rx_conn, schc_mbuf_t* tail) {
	uint8_t window = get_window_bit(tail->ptr, rx_conn); // the window bit from the fragment
	uint8_t fcn = get_fcn_value(tail->ptr, rx_conn); // the fcn value from the fragment

	DEBUG_PRINTF("WAIT END");
	if (rx_conn->timer_flag && !rx_conn->input) { // inactivity timer expired
		abort_connection(rx_conn); // todo
		return 0;
	}

	if (mic_correct(rx_conn) < 0) { // tail is NULL
		return 0;
	} else {
		if (!mic_correct(rx_conn)) { // mic incorrect
			DEBUG_PRINTF("mic wrong");
			rx_conn->ack.mic = 0;
			rx_conn->RX_STATE = WAIT_END;
			if (window == rx_conn->window) { // expected window
				DEBUG_PRINTF("expected window");
				set_local_bitmap(rx_conn);
			}
			if (fcn == get_max_fcn_value(rx_conn) && rx_conn->mode == ACK_ALWAYS) { // all-1
				DEBUG_PRINTF("all-1");
				if (empty_all_1(tail, rx_conn)) {
					discard_fragment(rx_conn); // remove last fragment (empty)
				}
				send_ack(rx_conn);
			}
		} else { // mic right
			DEBUG_PRINTF("mic correct");
			if (window == rx_conn->window) { // expected window
				DEBUG_PRINTF("expected window");
				rx_conn->RX_STATE = END_RX;
				rx_conn->ack.fcn = get_max_fcn_value(rx_conn); // c bit is set when ack.fcn is max
				rx_conn->ack.mic = 1; // bitmap is not sent when mic correct
				set_local_bitmap(rx_conn);
				send_ack(rx_conn);
				return 2; // stay alive to answer lost acks
			}
		}
	}

	if (fcn == get_max_fcn_value(rx_conn) && rx_conn->mode == ACK_ON_ERROR) { // all-1
		DEBUG_PRINTF("all-1");
		if (empty_all_1(tail, rx_conn)) {
			discard_fragment(rx_conn); // remove last fragment (empty)
		}
		rx_conn->RX_STATE = WAIT_END;
		send_ack(rx_conn);
	}
	return 0;
}

/**
 * the receiver state machine
 *
 * @param 	conn		a pointer to the connection
 *
 * @return 	0			TBD
 *
 */
int8_t schc_reassemble(schc_fragmentation_t* rx_conn) {
	uint8_t recv_mic[MIC_SIZE_BYTES] = { 0 };
	schc_mbuf_t* tail = get_mbuf_tail(rx_conn->head); // get last received fragment

	copy_bits(rx_conn->ack.rule_id, 0, tail->ptr, 0, rx_conn->RULE_SIZE); // get the rule id from the fragment
	uint8_t window = get_window_bit(tail->ptr, rx_conn); // the window bit from the fragment
	uint8_t fcn = get_fcn_value(tail->ptr, rx_conn); // the fcn value from the fragment

	if(rx_conn->mode == NO_ACK) { // can not find fragment from fcn value
		rx_conn->frag_cnt++; // update fragment counter
	} else {
		set_conn_frag_cnt(rx_conn, fcn);
	}

	DEBUG_PRINTF("fcn is %d, window is %d", fcn, window);

	rx_conn->fcn = fcn;
	rx_conn->ack.fcn = fcn;

	if (window == (!rx_conn->window)) {
		rx_conn->window_cnt++;
	}

	tail->frag_cnt = rx_conn->frag_cnt; // update tail frag count

	if(rx_conn->input) { // set inactivity timer if the loop was triggered by a fragment input
		set_inactivity_timer(rx_conn);
	}

	/*
	 * ACK ALWAYS MODE
	 */
	if (rx_conn->mode == ACK_ALWAYS) {
		switch (rx_conn->RX_STATE) {
		case RECV_WINDOW: {
			DEBUG_PRINTF("RECV WINDOW");
			if (rx_conn->timer_flag && !rx_conn->input) { // inactivity timer expired
				abort_connection(rx_conn); // todo
				break;
			}
			if (rx_conn->window != window) { // unexpected window
				DEBUG_PRINTF("w != window");
				discard_fragment(rx_conn);
				rx_conn->RX_STATE = RECV_WINDOW;
				break;
			} else if (window == rx_conn->window) { // expected window
				DEBUG_PRINTF("w == window");
				if (fcn != 0 && fcn != get_max_fcn_value(rx_conn)) { // not all-x
					DEBUG_PRINTF("not all-x");
					set_local_bitmap(rx_conn);
					rx_conn->RX_STATE = RECV_WINDOW;
				} else if (fcn == 0) { // all-0
					DEBUG_PRINTF("all-0");
					if (!empty_all_0(tail, rx_conn)) {
						set_local_bitmap(rx_conn); // indicate that we received a fragment
					} else {
						discard_fragment(rx_conn);
					}
					rx_conn->RX_STATE = WAIT_NEXT_WINDOW;
					rx_conn->ack.mic = 0; // bitmap will be sent when c = 0
					send_ack(rx_conn); // send local bitmap
				} else if (fcn == get_max_fcn_value(rx_conn)) { // all-1
					if (!empty_all_1(tail, rx_conn)) {
						DEBUG_PRINTF("all-1");
						set_local_bitmap(rx_conn);
						if(!mic_correct(rx_conn)) { // mic wrong
							rx_conn->RX_STATE = WAIT_END;
							rx_conn->ack.mic = 0;
						} else { // mic right
							rx_conn->RX_STATE = END_RX;
							rx_conn->ack.fcn = get_max_fcn_value(rx_conn); // c bit is set when ack.fcn is max
							rx_conn->ack.mic = 1; // bitmap is not sent when mic correct
							send_ack(rx_conn);
							return 2; // stay alive to answer lost acks
						}
					} else {
						discard_fragment(rx_conn);
					}
					send_ack(rx_conn);
				}
			}
			break;
		}
		case WAIT_NEXT_WINDOW: {
			DEBUG_PRINTF("WAIT NEXT WINDOW");
			if (rx_conn->timer_flag && !rx_conn->input) { // inactivity timer expired
				abort_connection(rx_conn); // todo
				break;
			}
			if (window == (!rx_conn->window)) { // next window
				DEBUG_PRINTF("w != window");
				if (fcn != 0 && fcn != get_max_fcn_value(rx_conn)) { // not all-x
					DEBUG_PRINTF("not all-x");
					rx_conn->window = !rx_conn->window; // set expected window to next window
					clear_bitmap(rx_conn);
					set_local_bitmap(rx_conn);
					rx_conn->RX_STATE = RECV_WINDOW; // return to receiving window
				} else if (fcn == 0) { // all-0
					DEBUG_PRINTF("all-0");
					if (empty_all_0(tail, rx_conn)) {
						discard_fragment(rx_conn); // remove last fragment (empty)
					} else {
						rx_conn->window = !rx_conn->window;
						clear_bitmap(rx_conn);
						set_local_bitmap(rx_conn);
					}
					rx_conn->ack.mic = 0; // bitmap will be sent when c = 0
					send_ack(rx_conn);
				} else if (fcn == get_max_fcn_value(rx_conn)) { // all-1
					DEBUG_PRINTF("all-1");
					if (empty_all_1(tail, rx_conn)) {
						discard_fragment(rx_conn); // remove last fragment (empty)
					} else {
						if(!mic_correct(rx_conn)) { // mic wrong
							rx_conn->RX_STATE = WAIT_END;
							rx_conn->ack.mic = 0;
						} else { // mic right
							rx_conn->RX_STATE = END_RX;
							rx_conn->ack.fcn = get_max_fcn_value(rx_conn); // c bit is set when ack.fcn is max
							rx_conn->ack.mic = 1; // bitmap is not sent when mic correct
							send_ack(rx_conn);
							return 2; // stay alive to answer lost acks
						}
						set_local_bitmap(rx_conn);
					}
					send_ack(rx_conn);
				}
			} else if (window == rx_conn->window) { // expected window
				DEBUG_PRINTF("w == window");
				if (fcn == 0) { // all-0
					if (empty_all_0(tail, rx_conn)) {
						discard_fragment(rx_conn);
					} else {
						DEBUG_PRINTF("all-0");
						rx_conn->RX_STATE = WAIT_NEXT_WINDOW;
					}
					rx_conn->ack.mic = 0; // bitmap will be sent when c = 0
					send_ack(rx_conn);
				} else if (fcn == get_max_fcn_value(rx_conn)) { // all-1
					DEBUG_PRINTF("all-1");
					rx_conn->RX_STATE = WAIT_NEXT_WINDOW;
					discard_fragment(rx_conn);
				} else if (fcn != 0 && fcn != get_max_fcn_value(rx_conn)) { // not all-x
					set_local_bitmap(rx_conn);
					DEBUG_PRINTF("not all-x, is bitmap full? %d",
							is_bitmap_full(rx_conn, (rx_conn->MAX_WND_FCN + 1)));
					rx_conn->RX_STATE = WAIT_NEXT_WINDOW;
					if (is_bitmap_full(rx_conn, (rx_conn->MAX_WND_FCN + 1))) { // bitmap is full; the last fragment of a retransmission is received
						rx_conn->ack.mic = 0; // bitmap will be sent when c = 0
						send_ack(rx_conn);
					}
				}
			}
			break;
		}
		case WAIT_END: {
			uint8_t ret = wait_end(rx_conn, tail);
			if(ret) {
				return ret;
			}
			break;
		}
		case END_RX: {
			DEBUG_PRINTF("END RX");
			if (rx_conn->timer_flag && !rx_conn->input) { // inactivity timer expired
				// end the transmission
				mbuf_sort(&rx_conn->head); // sort the mbuf chain
				mbuf_format(&rx_conn->head, rx_conn); // remove headers to pass to application
				// todo
				// call function to forward to ipv6 network
				schc_reset(rx_conn);
				return 1; // end reception
			}
			if (fcn != get_max_fcn_value(rx_conn)) { // not all-1
				DEBUG_PRINTF("not all-x");
				discard_fragment(rx_conn);
			} else { // all-1
				DEBUG_PRINTF("all-1");
				send_ack(rx_conn);
				mbuf_sort(&rx_conn->head); // sort the mbuf chain
				mbuf_format(&rx_conn->head, rx_conn); // remove headers to pass to application
				return 1; // end reception
			}
			break;
		}
		}
	}
	/*
	 * NO ACK MODE
	 */
	else if (rx_conn->mode == NO_ACK) {
		switch (rx_conn->RX_STATE) {
		case RECV_WINDOW: {
			if (rx_conn->timer_flag && !rx_conn->input) { // inactivity timer expired
				abort_connection(rx_conn); // todo no send abort
				break;
			}
			if (fcn == get_max_fcn_value(rx_conn)) { // all-1
				// clear inactivity timer
				rx_conn->timer_flag = 0;
				if(!mic_correct(rx_conn)) { // mic wrong
					abort_connection(rx_conn); // todo no send abort
				} else { // mic correct
					rx_conn->RX_STATE = END_RX;
					rx_conn->ack.fcn = get_max_fcn_value(rx_conn); // c bit is set when ack.fcn is max
					rx_conn->ack.mic = 1; // bitmap is not sent when mic correct
					return 1;
				}
			}
			break;
		}
		case END_RX: {
			DEBUG_PRINTF("END RX"); // end the transmission
			mbuf_sort(&rx_conn->head); // sort the mbuf chain
			mbuf_format(&rx_conn->head, rx_conn); // remove headers to pass to application
			// todo
			// call function to forward to ipv6 network
			schc_reset(rx_conn);
			return 1; // end reception
		}
		}
	}
	/*
	 * ACK ON ERROR MODE
	 */
	else if (rx_conn->mode == ACK_ON_ERROR) {
		switch (rx_conn->RX_STATE) {
		case RECV_WINDOW: {
			DEBUG_PRINTF("RECV WINDOW");
			if (rx_conn->timer_flag && !rx_conn->input) { // inactivity timer expired
				abort_connection(rx_conn); // todo
				break;
			}
			if (rx_conn->window != window) { // unexpected window
				DEBUG_PRINTF("w != window");
				discard_fragment(rx_conn);
				rx_conn->RX_STATE = ERROR;
				break;
			} else if (window == rx_conn->window) { // expected window
				DEBUG_PRINTF("w == window");
				if (fcn != 0 && fcn != get_max_fcn_value(rx_conn)) { // not all-x
					DEBUG_PRINTF("not all-x");
					set_local_bitmap(rx_conn);
					rx_conn->RX_STATE = RECV_WINDOW;
				} else if (fcn == 0) { // all-0
					DEBUG_PRINTF("all-0");
					if(empty_all_0(tail, rx_conn)) {
						send_ack(rx_conn);
						break;
					}
					set_local_bitmap(rx_conn);
					if(is_bitmap_full(rx_conn, (rx_conn->MAX_WND_FCN + 1))) {
						clear_bitmap(rx_conn);
						rx_conn->window = !rx_conn->window;
						rx_conn->RX_STATE = RECV_WINDOW;
						break;
					} else {
						rx_conn->RX_STATE = WAIT_MISSING_FRAG;
						send_ack(rx_conn);
						break;
					}
				} else if (fcn == get_max_fcn_value(rx_conn)) { // all-1
					if (!empty_all_1(tail, rx_conn)) {
						DEBUG_PRINTF("all-1");
						set_local_bitmap(rx_conn);
						if (!mic_correct(rx_conn)) { // mic wrong
							rx_conn->RX_STATE = WAIT_END;
							rx_conn->ack.mic = 0;
						} else { // mic right
							rx_conn->RX_STATE = END_RX;
							rx_conn->ack.fcn = get_max_fcn_value(rx_conn); // c bit is set when ack.fcn is max
							rx_conn->ack.mic = 1; // bitmap is not sent when mic correct
							send_ack(rx_conn);
							return 2; // stay alive to answer lost acks
						}
					} else {
						discard_fragment(rx_conn);
					}
					send_ack(rx_conn);
				}
			}
			break;
		}
		case WAIT_MISSING_FRAG: {
			if (window == rx_conn->window) { // expected window
				DEBUG_PRINTF("w == window");
				if (fcn != 0 && fcn != get_max_fcn_value(rx_conn)
						&& is_bitmap_full(rx_conn, rx_conn->MAX_WND_FCN)) { // not all-x and bitmap not full
					set_local_bitmap(rx_conn);
					rx_conn->window = !rx_conn->window;
					rx_conn->RX_STATE = RECV_WINDOW;
				}
				if (empty_all_0(tail, rx_conn)) {
					rx_conn->RX_STATE = WAIT_MISSING_FRAG;
					send_ack(rx_conn);
					break;
				}
				if (fcn == get_max_fcn_value(rx_conn)) { // all-1
					abort_connection(rx_conn);
					break;
				}
			}
			break;
		}
		case WAIT_END: {
			uint8_t ret = wait_end(rx_conn, tail);
			if(ret) {
				return ret;
			}
			break;
		}
		case END_RX: {
			DEBUG_PRINTF("END RX");
			// end the transmission
			mbuf_sort(&rx_conn->head); // sort the mbuf chain
			mbuf_format(&rx_conn->head, rx_conn); // remove headers to pass to application
			// todo
			// call function to forward to ipv6 network
			schc_reset(rx_conn);
			return 1; // end reception
		}
		}
	}

	return 0;
}

/**
 * Initializes the SCHC fragmenter
 *
 * @param tx_conn		a pointer to the tx initialization structure
 *
 * @return error codes on error
 *
 */
int8_t schc_fragmenter_init(schc_fragmentation_t* tx_conn,
		void (*send)(uint8_t* data, uint16_t length, uint32_t device_id)) {
	uint32_t i;

	// initializes the schc tx connection
	schc_reset(tx_conn);

	// initializes the schc rx connections
	for (i = 0; i < SCHC_CONF_RX_CONNS; i++) {
		schc_reset(&schc_rx_conns[i]);
		schc_rx_conns[i].send = send;
		schc_rx_conns[i].frag_cnt = 0;
		schc_rx_conns[i].window_cnt = 0;
		schc_rx_conns[i].input = 0;
		schc_rx_conns[i].mode = 0;
		// in case these parameters were not configured properly
		schc_rx_conns[i].DTAG_SIZE = DTAG_SIZE_BITS;
		schc_rx_conns[i].WINDOW_SIZE = WINDOW_SIZE_BITS;
		schc_rx_conns[i].RULE_SIZE = RULE_SIZE_BITS;
		schc_rx_conns[i].FCN_SIZE = FCN_SIZE_BITS;
		schc_rx_conns[i].MAX_WND_FCN = MAX_WIND_FCN;
	}

	// initializes the mbuf pool
	MBUF_PTR = 0;
	for(i = 0; i < SCHC_CONF_MBUF_POOL_LEN; i++) {
		MBUF_POOL[i].ptr = NULL;
		MBUF_POOL[i].len = 0;
		MBUF_POOL[i].next = NULL;
		MBUF_POOL[i].offset = 0;
	}

	return 1;
}

/**
 * the function to call when the state machine is in SEND state
 *
 * @param 	tx_conn		a pointer to the tx connection structure
 *
 */
static void tx_fragment_send(schc_fragmentation_t *tx_conn) {
	uint8_t fcn = 0;
	tx_conn->frag_cnt++;
	tx_conn->attempts = 0; // reset number of attempts

	if (has_no_more_fragments(tx_conn)) {
		DEBUG_PRINTF("schc_fragment(): all-1 window");
		fcn = tx_conn->fcn;
		tx_conn->fcn = (pow(2, tx_conn->FCN_SIZE) - 1); // all 1-window
		if (send_fragment(tx_conn)) { // only continue when packet was transmitted
			tx_conn->TX_STATE = WAIT_BITMAP;
			set_local_bitmap(tx_conn); // set bitmap according to fcn
			set_retrans_timer(tx_conn);
		} else {
			DEBUG_PRINTF("schc_fragment(): radio occupied retrying in %d ms",
					tx_conn->dc);
			tx_conn->frag_cnt--;
			tx_conn->fcn = fcn; // reset fcn and frag_count before retrying
			set_dc_timer(tx_conn);
		}
	} else if (tx_conn->fcn == 0 && !has_no_more_fragments(tx_conn)) { // all-0 window
		DEBUG_PRINTF("schc_fragment(): all-0 window");
		if (send_fragment(tx_conn)) {
			tx_conn->TX_STATE = WAIT_BITMAP;
			set_local_bitmap(tx_conn); // set bitmap according to fcn
			tx_conn->fcn = tx_conn->MAX_WND_FCN; // reset the FCN
			set_retrans_timer(tx_conn);
		} else {
			DEBUG_PRINTF("schc_fragment(): radio occupied retrying in %d ms",
					tx_conn->dc);
			tx_conn->frag_cnt--;
			set_dc_timer(tx_conn);
		}
	} else if (tx_conn->fcn != 0 && !has_no_more_fragments(tx_conn)) { // normal fragment
		DEBUG_PRINTF("schc_fragment(): normal fragment");
		if (send_fragment(tx_conn)) {
			tx_conn->TX_STATE = SEND;
			set_local_bitmap(tx_conn); // set bitmap according to fcn
			tx_conn->fcn--;
		} else {
			tx_conn->frag_cnt--;
		}
		set_dc_timer(tx_conn);
	}
}

/**
 * the function to call when the state machine is in RESEND state
 *
 * @param 	tx_conn		a pointer to the tx connection structure
 *
 */
static void tx_fragment_resend(schc_fragmentation_t *tx_conn) {
	// get the next fragment offset; set frag_cnt
	uint8_t frag_cnt = tx_conn->frag_cnt;
	uint8_t last = 0;

	if (get_next_fragment_from_bitmap(tx_conn) == get_max_fcn_value(tx_conn)) {
		tx_conn->frag_cnt = ((tx_conn->tail_ptr - tx_conn->data_ptr)
				/ tx_conn->mtu) + 1;
		tx_conn->fcn = get_max_fcn_value(tx_conn);
		last = 1;
	} else {
		tx_conn->frag_cnt = (((tx_conn->MAX_WND_FCN + 1) * tx_conn->window_cnt)
				+ get_next_fragment_from_bitmap(tx_conn)); // send_fragment() uses frag_cnt to transmit a particular fragment
		tx_conn->fcn = ((tx_conn->MAX_WND_FCN + 1) * (tx_conn->window_cnt + 1))
				- tx_conn->frag_cnt;
		if (!get_next_fragment_from_bitmap(tx_conn)) {
			last = 1;
		}
	}

	DEBUG_PRINTF("schc_fragment(): sending missing fragments for bitmap: ");
	print_bitmap(tx_conn->ack.bitmap, (tx_conn->MAX_WND_FCN + 1));
	DEBUG_PRINTF("with FCN %d, window count %d, frag count %d", tx_conn->fcn,
			tx_conn->window_cnt, tx_conn->frag_cnt);

	if (last) { // check if this was the last fragment
		DEBUG_PRINTF("schc_fragment(): last missing fragment to send");
		if (send_fragment(tx_conn)) { // retransmit the fragment
			tx_conn->TX_STATE = WAIT_BITMAP;
			tx_conn->frag_cnt = (tx_conn->window_cnt + 1)
					* (tx_conn->MAX_WND_FCN + 1);
			set_retrans_timer(tx_conn);
		} else {
			tx_conn->frag_cnt = frag_cnt;
			set_dc_timer(tx_conn);
		}

	} else {
		if (send_fragment(tx_conn)) { // retransmit the fragment
			tx_conn->TX_STATE = RESEND;
		} else {
			tx_conn->frag_cnt = frag_cnt;
		}
		set_dc_timer(tx_conn);
	}
}

/**
 * the function to call when the state machine has to continue transmission
 *
 * @param 	tx_conn		a pointer to the tx connection structure
 *
 */
static void no_missing_fragments_more_to_come(schc_fragmentation_t *tx_conn) {
	DEBUG_PRINTF("no missing fragments & more fragments to come");
	tx_conn->timer_flag = 0; // stop retransmission timer
	clear_bitmap(tx_conn);
	tx_conn->window = !tx_conn->window; // change window
	tx_conn->window_cnt++;
	tx_conn->fcn = tx_conn->MAX_WND_FCN;
	tx_conn->frag_cnt = (tx_conn->window_cnt) * (tx_conn->MAX_WND_FCN + 1);
	tx_conn->TX_STATE = SEND;
}

/**
 * the sender state machine
 *
 * @param 	tx_conn		a pointer to the tx connection structure
 *
 * @return 	 0			TBD
 *        	-1			failed to initialize the connection
 *        	-2			no fragmentation was needed for this packet
 *
 */
int8_t schc_fragment(schc_fragmentation_t *tx_conn) {
	uint8_t fcn = 0;
	uint8_t frag_cnt = 0;

	if (tx_conn->TX_STATE == INIT_TX) {
		DEBUG_PRINTF("INIT_TX");
		int8_t ret = init_tx_connection(tx_conn);
		if (!ret) {
			return SCHC_FAILURE;
		} else if (ret < 0) {
			tx_conn->send(tx_conn->data_ptr,
					(tx_conn->tail_ptr - tx_conn->data_ptr),
					tx_conn->device_id); // send packet right away
			return SCHC_NO_FRAGMENTATION;
		}
		tx_conn->TX_STATE = SEND;
		schc_fragment(tx_conn);
	}

	if (tx_conn->TX_STATE == END_TX) {
		DEBUG_PRINTF("schc_fragment(): end transmission cycle");
		tx_conn->timer_flag = 0;
		schc_reset(tx_conn); // todo ??
		return SCHC_SUCCESS;
	}

	/*
	 * ACK ALWAYS MODE
	 */
	if (tx_conn->mode == ACK_ALWAYS) {
		switch (tx_conn->TX_STATE) {
		case SEND: {
			DEBUG_PRINTF("SEND");
			tx_fragment_send(tx_conn);
			break;
		}
		case WAIT_BITMAP: {
			DEBUG_PRINTF("WAIT_BITMAP");
			uint8_t resend_window[BITMAP_SIZE_BYTES] = { 0 }; // if ack.bitmap is all-0, there are no packets to retransmit

			if (tx_conn->attempts >= MAX_ACK_REQUESTS) {
				DEBUG_PRINTF(
						"tx_conn->attempts >= MAX_ACK_REQUESTS: send abort"); // todo
				tx_conn->TX_STATE = ERROR;
				tx_conn->timer_flag = 0; // stop retransmission timer
				// send_abort();
				schc_fragment(tx_conn);
				break;
			}
			if (tx_conn->ack.window[0] != tx_conn->window) { // unexpected window
				DEBUG_PRINTF("w != w, discard fragment");
				discard_fragment(tx_conn);
				tx_conn->TX_STATE = WAIT_BITMAP;
				break;
			}
			if (tx_conn->ack.window[0] == tx_conn->window) {
				DEBUG_PRINTF("w == w");
				if (!has_no_more_fragments(tx_conn)
						&& compare_bits(resend_window, tx_conn->ack.bitmap,
								(tx_conn->MAX_WND_FCN + 1))) { // no missing fragments & more fragments
					no_missing_fragments_more_to_come(tx_conn);
					schc_fragment(tx_conn);
				}
				if (has_no_more_fragments(tx_conn) && tx_conn->ack.mic) { // mic and bitmap check succeeded
					DEBUG_PRINTF("no more fragments, MIC ok");
					tx_conn->timer_flag = 0; // stop retransmission timer
					tx_conn->TX_STATE = END_TX;
					schc_fragment(tx_conn);
					break;
				}
			}
			if (!compare_bits(resend_window, tx_conn->ack.bitmap,
					(tx_conn->MAX_WND_FCN + 1))) { //ack.bitmap contains the missing fragments
				DEBUG_PRINTF("bitmap contains the missing fragments");
				tx_conn->attempts++;
				tx_conn->frag_cnt = (tx_conn->window_cnt)
						* (tx_conn->MAX_WND_FCN + 1);
				tx_conn->timer_flag = 0; // stop retransmission timer
				tx_conn->TX_STATE = RESEND;
				schc_fragment(tx_conn);
				break;
			}
			if (tx_conn->timer_flag) { // timer expired
				DEBUG_PRINTF("timer expired"); // todo
				if (send_empty(tx_conn)) { // requests retransmission of all-x ack with empty all-x
					tx_conn->attempts++;
					set_retrans_timer(tx_conn);
				} else {
					set_dc_timer(tx_conn);
				}
				break;
			}
			break;
		}
		case RESEND: {
			DEBUG_PRINTF("RESEND");
			tx_fragment_resend(tx_conn);
			break;
		}
		case ERROR: {
			DEBUG_PRINTF("ERROR");
			break;
		}
		}
	}
	/*
	 * NO ACK MODE
	 */
	else if (tx_conn->mode == NO_ACK) {
		switch (tx_conn->TX_STATE) {
		case SEND: {
			DEBUG_PRINTF("SEND");
			tx_conn->frag_cnt++;

			if (has_no_more_fragments(tx_conn)) { // last fragment
				DEBUG_PRINTF("last fragment");
				tx_conn->fcn = 1;
				tx_conn->TX_STATE = END_TX;
			} else {
				DEBUG_PRINTF("normal fragment");
				tx_conn->fcn = 0;
				tx_conn->TX_STATE = SEND;
			}
			if (!send_fragment(tx_conn)) { // only continue when packet was transmitted
				DEBUG_PRINTF(
						"schc_fragment(): radio occupied retrying in %d ms",
						tx_conn->dc);
				tx_conn->frag_cnt--;
			}
			set_dc_timer(tx_conn); // send next fragment in dc ms or end transmission
			break;
		}
		case END_TX: {
			DEBUG_PRINTF("schc_fragment(): end transmission cycle");
			schc_reset(tx_conn);
			return SCHC_SUCCESS;
			break;
		}
		}
	}
	/*
	 * ACK ON ERROR MODE
	 */
	else if (tx_conn->mode == ACK_ON_ERROR) {
		switch (tx_conn->TX_STATE) {
		case SEND: {
			DEBUG_PRINTF("SEND");
			tx_fragment_send(tx_conn);
			break;
		}
		case WAIT_BITMAP: {
			DEBUG_PRINTF("WAIT_BITMAP");
			uint8_t resend_window[BITMAP_SIZE_BYTES] = { 0 }; // if ack.bitmap is all-0, there are no packets to retransmit

			if (tx_conn->attempts >= MAX_ACK_REQUESTS) {
				DEBUG_PRINTF(
						"tx_conn->attempts >= MAX_ACK_REQUESTS: send abort"); // todo
				tx_conn->TX_STATE = ERROR;
				tx_conn->timer_flag = 0; // stop retransmission timer
				// send_abort();
				schc_fragment(tx_conn);
				break;
			}
			if (tx_conn->timer_flag && !tx_conn->input) { // timer expired
				DEBUG_PRINTF("timer expired"); // todo

				if (!has_no_more_fragments(tx_conn)) { // more fragments to come
					no_missing_fragments_more_to_come(tx_conn);
					schc_fragment(tx_conn);
				} else if (has_no_more_fragments(tx_conn)) {
					tx_conn->timer_flag = 0; // stop retransmission timer
					send_tx_empty(tx_conn); // todo
					tx_conn->TX_STATE = WAIT_BITMAP;
					set_dc_timer(tx_conn);
				}
				break;
			}
			if (tx_conn->ack.window[0] != tx_conn->window) { // unexpected window
				DEBUG_PRINTF("w != w, discard fragment");
				discard_fragment(tx_conn);
				tx_conn->TX_STATE = WAIT_BITMAP;
				break;
			}
			if (!compare_bits(resend_window, tx_conn->ack.bitmap,
					(tx_conn->MAX_WND_FCN + 1))) { //ack.bitmap contains the missing fragments
				DEBUG_PRINTF("bitmap contains the missing fragments");
				tx_conn->attempts++;
				tx_conn->frag_cnt = (tx_conn->window_cnt)
						* (tx_conn->MAX_WND_FCN + 1);
				tx_conn->timer_flag = 0; // stop retransmission timer
				tx_conn->TX_STATE = RESEND;
				schc_fragment(tx_conn);
				break;
			} else if (compare_bits(resend_window, tx_conn->ack.bitmap,
					(tx_conn->MAX_WND_FCN + 1))) {
				DEBUG_PRINTF("received bitmap == local bitmap");
				tx_conn->timer_flag = 0; // stop retransmission timer
				tx_conn->TX_STATE = END_TX;
				schc_fragment(tx_conn); // end
				break;
			}
			case RESEND:
			{
				DEBUG_PRINTF("RESEND");
				tx_fragment_resend(tx_conn);
				break;
			}
		}
		}
	}

	tx_conn->input = 0;

	return 0;
}

/**
 * This function should be called whenever a packet is received
 *
 * @param 	data			a pointer to the received data
 * @param 	len				the length of the received packet
 * @param 	tx_conn			a pointer to the tx initialization structure
 * @param 	device_id		the device id from the rx source
 *
 */
schc_fragmentation_t* schc_input(uint8_t* data, uint16_t len, schc_fragmentation_t* tx_conn,
		uint32_t device_id) {
	if ((tx_conn->TX_STATE == WAIT_BITMAP || tx_conn->TX_STATE == RESEND)
			&& compare_bits(tx_conn->rule_id, data, tx_conn->RULE_SIZE)) { // acknowledgment
		schc_ack_input(data, len, tx_conn, device_id);
		return tx_conn;
	} else {
		schc_fragmentation_t* rx_conn = schc_fragment_input((uint8_t*) data, len, device_id);
		return rx_conn;
	}

	// todo
	// how to return if last fragment received??
}

/**
 * This function should be called whenever an ack is received
 *
 * @param 	data			a pointer to the received data
 * @param 	len				the length of the received packet
 * @param 	tx_conn			a pointer to the tx initialization structure
 * @param   device_id		the device id from the rx source
 *
 */
void schc_ack_input(uint8_t* data, uint16_t len, schc_fragmentation_t* tx_conn,
		uint32_t device_id) {
	uint8_t bit_offset = tx_conn->RULE_SIZE;
	tx_conn->input = 1;

	memset(tx_conn->ack.dtag, 0, 1); // clear dtag from prev reception
	copy_bits(tx_conn->ack.dtag, (8 - tx_conn->DTAG_SIZE), (uint8_t*) data,
			bit_offset, tx_conn->DTAG_SIZE); // get dtag
	bit_offset += tx_conn->DTAG_SIZE;

	memset(tx_conn->ack.window, 0, 1); // clear window from prev reception
	copy_bits(tx_conn->ack.window, (8 - tx_conn->WINDOW_SIZE), (uint8_t*) data,
			bit_offset, tx_conn->WINDOW_SIZE); // get window
	bit_offset += tx_conn->WINDOW_SIZE;

	uint8_t bitmap_len = (tx_conn->MAX_WND_FCN + 1);

	if(has_no_more_fragments(tx_conn)) { // all-1 window
		uint8_t mic[1] = { 0 };
		copy_bits(mic, 7, (uint8_t*) data, bit_offset, 1);
		bit_offset += 1;
		tx_conn->ack.mic = mic[0];
		bitmap_len = (BITMAP_SIZE_BYTES * 8);
		if(mic[0]) { // do not process bitmap
			schc_fragment(tx_conn);
			return;
		}
	}

	// ToDo
	// decode_bitmap(tx_conn);
	memset(tx_conn->ack.bitmap, 0, 1); // clear bitmap from prev reception
	copy_bits(tx_conn->ack.bitmap, 0, (uint8_t*) data, bit_offset,
			bitmap_len);

	// copy bits for retransmit bitmap to intermediate buffer
	uint8_t resend_window[BITMAP_SIZE_BYTES] = { 0 };

	xor_bits(resend_window, tx_conn->bitmap, tx_conn->ack.bitmap,
			bitmap_len); // to indicate which fragments to retransmit

	// copy retransmit bitmap for current window to ack.bitmap
	memset(tx_conn->ack.bitmap, 0, BITMAP_SIZE_BYTES);
	copy_bits(tx_conn->ack.bitmap, 0, resend_window, 0, bitmap_len);

	// continue with state machine
	schc_fragment(tx_conn);
}

/**
 * This function should be called whenever a fragment is received
 * an open connection is picked for the device
 * out of a pool of connections to keep track of the packet
 *
 * @param 	data			a pointer to the data packet
 * @param 	len				the length of the received packet
 * @param 	device_id		the device id from the rx source
 *
 * @return 	conn			the connection
 *
 */
schc_fragmentation_t* schc_fragment_input(uint8_t* data, uint16_t len,
		uint32_t device_id) {
	schc_fragmentation_t *conn;

	// get a connection for the device
	conn = schc_get_connection(device_id);
	if (!conn) { // return if there was no connection available
		DEBUG_PRINTF("schc_fragment_input(): no free connections found!");
		return NULL;
	}

	uint8_t* fragment;
#if DYNAMIC_MEMORY
	fragment = (uint8_t*) malloc(len); // allocate memory for fragment
#else
	fragment = (uint8_t*) (schc_buf + buf_ptr); // take fixed memory block
	buf_ptr += len;
#endif

	memcpy(fragment, data, len);

	int8_t err = mbuf_push(&conn->head, fragment, len);

	// mbuf_print(conn->head);

	if(err != SCHC_SUCCESS) {
		return NULL;
	}

	conn->input = 1; // set fragment input to 1, to distinguish between inactivity callbacks

	return conn;
}

#if CLICK
ELEMENT_PROVIDES(schcFRAGMENTER)
#endif
