/*
 * Argon2 source code package
 *
 * Written by Daniel Dinu and Dmitry Khovratovich, 2015
 *
 * This work is licensed under a Creative Commons CC0 1.0 License/Waiver.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with
 * this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

/*For memory wiping*/
#ifdef _MSC_VER
#include <windows.h>
#include <winbase.h> /* For SecureZeroMemory */
#endif
#if defined __STDC_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif
#define VC_GE_2005(version) (version >= 1400)

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argon2.h"
#include "cores.h"
#include "blake2/blake2.h"
#include "blake2/blake2-impl.h"

#ifdef GENKAT
#include "genkat.h"
#endif

#if defined(__clang__)
#if __has_attribute(optnone)
#define NOT_OPTIMIZED __attribute__((optnone))
#endif
#elif defined(__GNUC__)
#define GCC_VERSION                                                            \
    (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#if GCC_VERSION >= 40400
#define NOT_OPTIMIZED __attribute__((optimize("O0")))
#endif
#endif
#ifndef NOT_OPTIMIZED
#define NOT_OPTIMIZED
#endif

/***************Instance and Position constructors**********/
void init_block_value(block *b, uint8_t in) { memset(b->v, in, sizeof(b->v)); }

void copy_block(block *dst, const block *src) {
    memcpy(dst->v, src->v, sizeof(uint64_t) * ARGON2_WORDS_IN_BLOCK);
}

void xor_block(block *dst, const block *src) {
    int i;
    for (i = 0; i < ARGON2_WORDS_IN_BLOCK; ++i) {
        dst->v[i] ^= src->v[i];
    }
}

static void load_block(block *dst, const void *input) {
    unsigned i;
    for (i = 0; i < ARGON2_WORDS_IN_BLOCK; ++i) {
        dst->v[i] = load64((const uint8_t *)input + i * sizeof(dst->v[i]));
    }
}

static void store_block(void *output, const block *src) {
    unsigned i;
    for (i = 0; i < ARGON2_WORDS_IN_BLOCK; ++i) {
        store64((uint8_t *)output + i * sizeof(src->v[i]), src->v[i]);
    }
}

/***************Memory allocators*****************/
int allocate_memory(block **memory, uint32_t m_cost) {
    if (memory != NULL) {
        size_t memory_size = sizeof(block) * m_cost;
        if (m_cost != 0 &&
            memory_size / m_cost !=
                sizeof(block)) { /*1. Check for multiplication overflow*/
            return ARGON2_MEMORY_ALLOCATION_ERROR;
        }

        *memory = (block *)malloc(memory_size); /*2. Try to allocate*/

        if (!*memory) {
            return ARGON2_MEMORY_ALLOCATION_ERROR;
        }

        return ARGON2_OK;
    } else {
        return ARGON2_MEMORY_ALLOCATION_ERROR;
    }
}

/*
void NOT_OPTIMIZED secure_wipe_memory(void *v, size_t n) {
#if defined(_MSC_VER) && VC_GE_2005(_MSC_VER)
    SecureZeroMemory(v, n);
#elif defined memset_s
    memset_s(v, n);
#elif defined(__OpenBSD__)
    explicit_bzero(v, n);
#else
    static void *(*const volatile memset_sec)(void *, int, size_t) = &memset;
    memset_sec(v, 0, n);
#endif
}
*/

void secure_wipe_memory(void *v, size_t n) {
    memset(v, 0, n);
}

/*********Memory functions*/

void clear_memory(argon2_instance_t *instance, int clear) {
    if (instance->memory != NULL && clear) {
        secure_wipe_memory(instance->memory,
                           sizeof(block) * /*instance->memory_blocks*/16);
    }
}

void free_memory(block *memory) { free(memory); }

void finalize(const argon2_context *context, argon2_instance_t *instance) {
    if (context != NULL && instance != NULL) {
        block blockhash;
        uint32_t l;

        copy_block(&blockhash, instance->memory + /*instance->lane_length - 1*/15);

        /* XOR the last blocks */
        /*for (l = 1; l < instance->lanes; ++l) {
            uint32_t last_block_in_lane =
                l * instance->lane_length + (instance->lane_length - 1)15;
            xor_block(&blockhash, instance->memory + last_block_in_lane);
        }*/

        /* Hash the result */
        {
            uint8_t blockhash_bytes[ARGON2_BLOCK_SIZE];
            store_block(blockhash_bytes, &blockhash);
            blake2b_long(context->out, /*context->outlen*/32, blockhash_bytes,
                         ARGON2_BLOCK_SIZE);
            secure_wipe_memory(blockhash.v,
                               ARGON2_BLOCK_SIZE); /* clear blockhash */
            secure_wipe_memory(blockhash_bytes,
                               ARGON2_BLOCK_SIZE); /* clear blockhash_bytes */
        }

#ifdef GENKAT
        print_tag(context->out, context->outlen);
#endif

        /* Clear memory */
        clear_memory(instance, /*context->flags & ARGON2_FLAG_CLEAR_PASSWORD*/1);

        /* Deallocate the memory */
        /*if (NULL != context->free_cbk) {
            context->free_cbk((uint8_t *)instance->memory,
                              instance->memory_blocks16 * sizeof(block));
        } else {*/
            free_memory(instance->memory);
        /*}*/
    }
}

uint32_t index_alpha(const argon2_instance_t *instance,
                     const argon2_position_t *position, uint32_t pseudo_rand,
                     int same_lane) {
    /*
     * Pass 0:
     *      This lane : all already finished segments plus already constructed
     * blocks in this segment
     *      Other lanes : all already finished segments
     * Pass 1+:
     *      This lane : (SYNC_POINTS - 1) last segments plus already constructed
     * blocks in this segment
     *      Other lanes : (SYNC_POINTS - 1) last segments
     */
    uint32_t reference_area_size;
    uint64_t relative_position;
    uint32_t start_position, absolute_position;

    if (0 == position->pass) {
        /* First pass */
        if (0 == position->slice) {
            /* First slice */
            reference_area_size =
                position->index - 1; /* all but the previous */
        } else {
            if (same_lane) {
                /* The same lane => add current segment */
                reference_area_size =
                    position->slice * 4 +
                    position->index - 1;
            } else {
                reference_area_size =
                    position->slice * 4 +
                    ((position->index == 0) ? (-1) : 0);
            }
        }
    } else {
        /* Second pass */
        if (same_lane) {
            reference_area_size = /*instance->lane_length -
                                  instance->segment_length*/11 + position->index; /*-
                                  1;*/
        } else {
            reference_area_size = /*instance->lane_length -
                                  instance->segment_length*/12 - (position->index == 0);
                                  /*((position->index == 0) ? (-1) : 0);*/
        }
    }

    /* 1.2.4. Mapping pseudo_rand to 0..<reference_area_size-1> and produce
     * relative position */
    relative_position = pseudo_rand;
    relative_position = relative_position * relative_position >> 32;
    relative_position = reference_area_size - 1 -
                        (reference_area_size * relative_position >> 32);

    /* 1.2.5 Computing starting position */
    start_position = 0;

    if (0 != position->pass) {
        start_position = (position->slice == ARGON2_SYNC_POINTS - 1)
                             ? 0
                             : (position->slice + 1) * /*instance->segment_length*/4;
    }

    /* 1.2.6. Computing absolute position */
    absolute_position = (start_position + relative_position) %
                        /*instance->lane_length*/16; /* absolute position */
    return absolute_position;
}

void fill_memory_blocks(argon2_instance_t *instance) {
    uint32_t r, s;

    /*if (instance == NULL || instance->lanes == 0) {
        return;
    }*/

    for (r = 0; r < /*instance->passes*/2; ++r) {
        for (s = 0; s < ARGON2_SYNC_POINTS; ++s) {

            argon2_position_t position;
            position.pass = r;
            position.lane = 0;
            position.slice = (uint8_t)s;
            position.index = 0;
            fill_segment(instance, position);
        }

#ifdef GENKAT
        internal_kat(instance, r); /* Print all memory blocks */
#endif
    }
}

void fill_first_blocks(uint8_t *blockhash, const argon2_instance_t *instance) {
    uint32_t l;
    /* Make the first and second block in each lane as G(H0||i||0) or
       G(H0||i||1) */
    uint8_t blockhash_bytes[ARGON2_BLOCK_SIZE];
    /*for (l = 0; l < instance->lanes; ++l) {*/

        store32(blockhash + ARGON2_PREHASH_DIGEST_LENGTH, 0);
        store32(blockhash + ARGON2_PREHASH_DIGEST_LENGTH + 4, 0);
        blake2b_long(blockhash_bytes, ARGON2_BLOCK_SIZE, blockhash,
                     ARGON2_PREHASH_SEED_LENGTH);
        load_block(&instance->memory[0], blockhash_bytes);

        store32(blockhash + ARGON2_PREHASH_DIGEST_LENGTH, 1);
        blake2b_long(blockhash_bytes, ARGON2_BLOCK_SIZE, blockhash,
                     ARGON2_PREHASH_SEED_LENGTH);
        load_block(&instance->memory[1], blockhash_bytes);
    /*}*/
    secure_wipe_memory(blockhash_bytes, ARGON2_BLOCK_SIZE);
}

void initial_hash(uint8_t *blockhash, argon2_context *context,
                  argon2_type type) {
    blake2b_state BlakeHash;
    uint8_t value[sizeof(uint32_t)];

    /*if (NULL == context || NULL == blockhash) {
        return;
    }*/

    blake2b_init(&BlakeHash, ARGON2_PREHASH_DIGEST_LENGTH);

    store32(&value, /*context->lanes*/1);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, /*context->outlen*/32);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, /*context->m_cost*/16);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, /*context->t_cost*/2);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, ARGON2_VERSION_NUMBER);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, (uint32_t)type);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, /*context->pwdlen*/32);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

 
    blake2b_update(&BlakeHash, (const uint8_t *)context->pwd,
                   /*context->pwdlen*/32);


    secure_wipe_memory(context->pwd, /*context->pwdlen*/32);
    context->pwdlen = 0;

    store32(&value, /*context->saltlen*/32);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

 
    blake2b_update(&BlakeHash, (const uint8_t *)context->salt,
                   /*context->saltlen*/32);
 
    store32(&value, /*context->secretlen*/0);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    /*if (context->secret != NULL) {
        blake2b_update(&BlakeHash, (const uint8_t *)context->secret,
                       context->secretlen);

        if (context->flags & ARGON2_FLAG_CLEAR_SECRET) {
            secure_wipe_memory(context->secret, context->secretlen);
            context->secretlen = 0;
        }
    }*/

    store32(&value, /*context->adlen*/0);
    blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    /*if (context->ad != NULL) {
        blake2b_update(&BlakeHash, (const uint8_t *)context->ad,
                       context->adlen);
    }*/

    blake2b_final(&BlakeHash, blockhash, ARGON2_PREHASH_DIGEST_LENGTH);
}

int initialize(argon2_instance_t *instance, argon2_context *context) {
    uint8_t blockhash[ARGON2_PREHASH_SEED_LENGTH];
    int result = ARGON2_OK;

    /*if (instance == NULL || context == NULL)
        return ARGON2_INCORRECT_PARAMETER;*/

    /* 1. Memory allocation */


    result = allocate_memory(&(instance->memory), 16);
    /*if (ARGON2_OK != result) {  return result;  }*/

    /* 2. Initial hashing */
    /* H_0 + 8 extra bytes to produce the first blocks */
    /* uint8_t blockhash[ARGON2_PREHASH_SEED_LENGTH]; */
    /* Hashing all inputs */
    initial_hash(blockhash, context, instance->type);
    /* Zeroing 8 extra bytes */
    secure_wipe_memory(blockhash + ARGON2_PREHASH_DIGEST_LENGTH,
                       ARGON2_PREHASH_SEED_LENGTH -
                           ARGON2_PREHASH_DIGEST_LENGTH);

#ifdef GENKAT
    initial_kat(blockhash, context, instance->type);
#endif

    /* 3. Creating first blocks, we always have at least two blocks in a slice
     */
    fill_first_blocks(blockhash, instance);
    /* Clearing the hash */
    secure_wipe_memory(blockhash, ARGON2_PREHASH_SEED_LENGTH);

    return ARGON2_OK;
}

int argon2_core(argon2_context *context, argon2_type type) {
    /* 1. Validate all inputs */
    /*int result = validate_inputs(context);
    uint32_t memory_blocks, segment_length;*/
    

    /*if (ARGON2_OK != result) {
        return result;
    }

    if (Argon2_d != type && Argon2_i != type) {
        return ARGON2_INCORRECT_TYPE;
    }*/

    /* 2. Align memory size */
    /* Minimum memory_blocks = 8L blocks, where L is the number of lanes */
    /*memory_blocks = 16;*/

    /*if (memory_blocks < 2 * ARGON2_SYNC_POINTS) {
        memory_blocks = 2 * ARGON2_SYNC_POINTS;
    }*/

    /*segment_length = 4;*/
argon2_instance_t instance;
    instance.memory = NULL;
    /*instance.passes = 2;
    instance.memory_blocks = 16;
    instance.segment_length = 4;
    instance.lane_length = 16;
    instance.lanes = 1;
    instance.threads = 1;*/
    instance.type = type;

    /* 3. Initialization: Hashing inputs, allocating memory, filling first
     * blocks
     */

    int result = initialize(&instance, context);
    if (ARGON2_OK != result) return result;

    /* 4. Filling memory */
    fill_memory_blocks(&instance);

    /* 5. Finalization */
    finalize(context, &instance);

    return ARGON2_OK;
}
