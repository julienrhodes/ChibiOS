/*
    ChibiOS - Copyright (C) 2006..2019 Giovanni Di Sirio.

    This file is part of ChibiOS.

    ChibiOS is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    chobjcaches.c
 * @brief   Objects Caches code.
 * @details Objects caches.
 *          <h2>Operation mode</h2>
 *          An object cache allows to retrieve and release objects from a
 *          slow media, for example a disk or flash.<br>
 *          The most recently used objects are kept in a series of RAM
 *          buffers making access faster. Objects are identified by a
 *          pair <group, key> which could be mapped, for example, to a
 *          disk drive identifier and sector identifier.<br>
 *          Read and write operations are performed using externally-supplied
 *          functions, the cache is device-agnostic.<br>
 *          The cache uses internally an hash table, the size of the table
 *          should be dimensioned to minimize the risk of hash collisions,
 *          a factor of two is usually acceptable, it depends on the specific
 *          application requirements.<br>
 *          Operations defined for caches:
 *          - <b>Get Object</b>: Retrieves an object from cache, if not
 *            present then an empty buffer is returned.
 *          - <b>Read Object</b>: Retrieves an object from cache, if not
 *            present a buffer is allocated and the object is read from the
 *            media.
 *          - <b>Release Object</b>: Releases an object to the cache handling
 *            the media update, if required.
 *          .
 * @pre     In order to use the pipes APIs the @p CH_CFG_USE_OBJ_CACHES
 *          option must be enabled in @p chconf.h.
 * @note    Compatible with RT and NIL.
 *
 * @addtogroup oslib_objchaches
 * @{
 */

#include "ch.h"

#if (CH_CFG_USE_OBJ_CACHES == TRUE) || defined(__DOXYGEN__)

/*===========================================================================*/
/* Module local definitions.                                                 */
/*===========================================================================*/

/* Default hash function.*/
#if !defined(OC_HASH_FUNCTION) || defined(__DOXYGEN__)
#define OC_HASH_FUNCTION(ocp, group, key)                                   \
  (((unsigned)(group) + (unsigned)(key)) & ((unsigned)(ocp)->hashn - 1U))
#endif

/* Insertion into an hash slot list.*/
#define HASH_INSERT(ocp, objp, group, key) {                                \
  oc_hash_header_t *hhp;                                                    \
  (hhp) = &(ocp)->hashp[OC_HASH_FUNCTION(ocp, group, key)];                 \
  (objp)->hash_next = (hhp)->hash_next;                                     \
  (objp)->hash_prev = (oc_object_t *)(hhp);                                 \
  (hhp)->hash_next->hash_prev = (objp);                                     \
  (hhp)->hash_next = (objp);                                                \
}

/* Removal of an object from the hash.*/
#define HASH_REMOVE(objp) {                                                 \
  (objp)->hash_prev->hash_next = (objp)->hash_next;                         \
  (objp)->hash_next->hash_prev = (objp)->hash_prev;                         \
}

/* Insertion on LRU list head (newer objects).*/
#define LRU_INSERT_HEAD(ocp, objp) {                                        \
  (objp)->lru_next = (ocp)->lru.lru_next;                                   \
  (objp)->lru_prev = (oc_object_t *)&(ocp)->lru;                            \
  (ocp)->lru.lru_next->lru_prev = (objp);                                   \
  (ocp)->lru.lru_next = (objp);                                             \
}

/* Insertion on LRU list head (newer objects).*/
#define LRU_INSERT_TAIL(ocp, objp) {                                        \
  (objp)->lru_prev = (ocp)->lru.lru_prev;                                   \
  (objp)->lru_next = (oc_object_t *)&(ocp)->lru;                            \
  (ocp)->lru.lru_prev->lru_next = (objp);                                   \
  (ocp)->lru.lru_prev = (objp);                                             \
}

/* Removal of an object from the LRU list.*/
#define LRU_REMOVE(objp) {                                                  \
  (objp)->lru_prev->lru_next = (objp)->lru_next;                            \
  (objp)->lru_next->lru_prev = (objp)->lru_prev;                            \
}

/*===========================================================================*/
/* Module exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Module local types.                                                       */
/*===========================================================================*/

/*===========================================================================*/
/* Module local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Module local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Returns an object pointer from the cache, if present.
 *
 * @param[out] ocp      pointer to the @p objects_cache_t structure to be
 * @param[in] group     object group identifier
 * @param[in] key       object identifier within the group
 *                      initialized
 * @return              The pointer to the retrieved object.
 * @retval NULL         if the object is not in cache.
 */
static oc_object_t *hash_get(objects_cache_t *ocp,
                             uint32_t group,
                             uint32_t key) {
  oc_hash_header_t *hhp;
  oc_object_t *objp;

  /* Hash slot where to search for an hit.*/
  hhp  = &ocp->hashp[OC_HASH_FUNCTION(ocp, group, key)];
  objp = hhp->hash_next;

  /* Scanning the siblings collision list.*/
  while (objp != (oc_object_t *)hhp) {
    if ((objp->obj_key == key) && (objp->obj_group == group)) {

      /* Cache hit.*/
      return objp;
    }
    objp = objp->hash_next;
  }

  return NULL;
}

/*===========================================================================*/
/* Module exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Initializes a @p objects_cache_t object.
 *
 * @param[out] ocp      pointer to the @p objects_cache_t structure to be
 *                      initialized
 * @param[in] hashn     number of elements in the hash table array, must be
 *                      a power of two and not lower than @p objn
 * @param[in] hashp     pointer to the hash table as an array of
 *                      @p oc_hash_header_t
 * @param[in] objn      number of elements in the objects table array
 * @param[in] hashp     pointer to the hash objects as an array of
 *                      @p oc_object_t
 * @param[in] readf     pointer to an object reader function
 * @param[in] writef    pointer to an object writer function
 *
 * Object records states:
 * - Invalid not owned:
 *    (OC_FLAG_INLRU, cnt==1).
 * - Caching an object not owned:
 *    (OC_FLAG_INLRU, OC_FLAG_INHASH, OC_FLAG_CACHEHIT, cnt==1).
 * - Representing an object owned:
 *    (OC_FLAG_INHASH, cnt<=0).
 * - Caching an object owned:
 *    (OC_FLAG_INHASH, OC_FLAG_CACHEHIT, cnt<=0).
 *
 * @init
 */
void chCacheObjectInit(objects_cache_t *ocp,
                       ucnt_t hashn,
                       oc_hash_header_t *hashp,
                       ucnt_t objn,
                       oc_object_t *objp,
                       oc_readf_t readf,
                       oc_writef_t writef) {

  chDbgCheck((ocp != NULL) && (hashp != NULL) && (objp != NULL) &&
             ((hashn & (hashn - 1U)) == 0U) &&
             (objn > (size_t)0) && (hashn >= objn));

  chSemObjectInit(&ocp->cache_sem, (cnt_t)1);
  chSemObjectInit(&ocp->lru_sem, (cnt_t)objn);
  ocp->hashn            = hashn;
  ocp->hashp            = hashp;
  ocp->objn             = objn;
  ocp->objp             = objp;
  ocp->readf            = readf;
  ocp->writef           = writef;
  ocp->lru.hash_next    = NULL;
  ocp->lru.hash_prev    = NULL;
  ocp->lru.lru_next     = (oc_object_t *)&ocp->lru;
  ocp->lru.lru_prev     = (oc_object_t *)&ocp->lru;

  /* Hash headers initialization.*/
  while (hashp < &ocp->hashp[ocp->hashn]) {
    hashp->hash_next = (oc_object_t *)hashp;
    hashp->hash_prev = (oc_object_t *)hashp;
  }

  /* Object headers initialization.*/
  while (objp < &ocp->objp[ocp->objn]) {
    chSemObjectInit(&objp->obj_sem, (cnt_t)1);
    LRU_INSERT_HEAD(ocp, objp);
    objp->obj_group = 0U;
    objp->obj_key   = 0U;
    objp->obj_flags = OC_FLAG_INLRU;
    objp->data      = NULL;
  }
}

/**
 * @brief   Retrieves an object from the cache.
 * @note    If the object is not in cache then the returned object is marked
 *          as @p OC_FLAG_INVALID meaning its data contains garbage and must
 *          be initialized.
 *
 * @param[in] ocp       pointer to the @p objects_cache_t structure
 * @param[in] group     object group identifier
 * @param[in] key       object identifier within the group
 * @return              The pointer to the retrieved object.
 * @retval NULL         is a reserved value.
 *
 * @api
 */
oc_object_t *chCacheGetObject(objects_cache_t *ocp,
                              uint32_t group,
                              uint32_t key) {

  /* If the requested object is not a generic one.*/
  if (group == OC_NO_GROUP) {
    /* Any buffer will do.*/
  }
  else {
    while (true) {
      oc_object_t *objp;

      /* Critical section enter, the hash check operation is fast.*/
      chSysLock();

      /* Checking the cache for a hit.*/
      objp = hash_get(ocp, group, key);

      chDbgAssert((objp->obj_flags & OC_FLAG_INHASH) != 0U, "not in hash");

      if (objp != NULL) {
        /* Cache hit, checking if the buffer is owned by some
           other thread.*/
        if (chSemGetCounterI(&objp->obj_sem) > (cnt_t)0) {
          /* Not owned case.*/

          chDbgAssert((objp->obj_flags & OC_FLAG_INLRU) != 0U, "not in LRU");

          /* Removing the object from LRU, now it is "owned".*/
          LRU_REMOVE(objp);
          objp->obj_flags &= ~OC_FLAG_INLRU;

          /* Getting the object semaphore, we know there is no wait so
             using the "fast" variant.*/
          chSemFastWaitI(&objp->obj_sem);
        }
        else {
          /* Owned case.*/
          msg_t msg;

          chDbgAssert((objp->obj_flags & OC_FLAG_INLRU) == 0U, "in LRU");

          /* Getting the buffer semaphore, note it could have been
             invalidated by the previous owner, in this case the
             semaphore has been reset.*/
          msg = chSemWaitS(&objp->obj_sem);

          /* Out of critical section and retry.*/
          chSysUnlock();

          /* The semaphore has been signaled, the object is OK.*/
          if (msg == MSG_OK) {

            return objp;
          }
        }
      }
      else {
        /* Cache miss, waiting for an object to become available
           in the LRU.*/
        chSemWaitS(&ocp->lru_sem);

        /* Now a buffer is in the LRU for sure, taking it from the
           LRU tail.*/
        objp = ocp->lru.lru_prev;

        chDbgAssert((objp->obj_flags & OC_FLAG_INLRU) != 0U, "not in LRU");
        chDbgAssert(chSemGetCounterI(&objp->obj_sem) == (cnt_t)1,
                    "semaphore counter not 1");

        LRU_REMOVE(objp);
        objp->obj_flags &= ~OC_FLAG_INLRU;

        /* Getting the object semaphore, we know there is no wait so
           using the "fast" variant.*/
        chSemFastWaitI(&objp->obj_sem);

        /* Naming this object and publishing it in the hash table.*/
        objp->obj_group  = group;
        objp->obj_key    = key;
        objp->obj_flags |= OC_FLAG_INHASH;
        HASH_INSERT(ocp, objp, group, key);
      }
    }
  }
}

/**
 * @brief   Releases an object into the cache.
 * @note    This function gives a meaning to the following flags:
 *          - @p OC_FLAG_INLRU should not happen, it is caught by an
 *            assertion.
 *          - @p OC_FLAG_ERROR the object is invalidated and queued on
 *            the LRU tail.
 *          - @p OC_FLAG_MODIFIED is ignored and kept.
 *          .
 *
 * @param[in] ocp       pointer to the @p objects_cache_t structure
 * @param[in] objp      pointer to the @p oc_object_t structure
 *
 * @iclass
 */
void chCacheReleaseObjectI(objects_cache_t *ocp,
                           oc_object_t *objp) {

  chDbgAssert((objp->obj_flags & OC_FLAG_INLRU) == 0U, "in LRU");
  chDbgAssert((objp->obj_flags & OC_FLAG_INHASH) != 0U, "not in hash");
  chDbgAssert(chSemGetCounterI(&objp->obj_sem) <= (cnt_t)0,
              "semaphore counter greater than 0");

  /* Cases where the object should be invalidated and discarded.*/
  if ((objp->obj_flags & OC_FLAG_ERROR) != 0U) {
    HASH_REMOVE(objp);
    LRU_INSERT_TAIL(ocp, objp);
    objp->obj_flags = OC_FLAG_INLRU;
    objp->obj_group = 0U;
    objp->obj_key   = 0U;
    return;
  }

  /* If some thread is waiting for this specific buffer then it is
     released directly without going in the LRU.*/
  if (chSemGetCounterI(&objp->obj_sem) < (cnt_t)0) {
    chSemSignalI(&objp->obj_sem);
    return;
  }
}

#endif /* CH_CFG_USE_OBJ_CACHES == TRUE */

/** @} */