/* Copyright (C) 2017 Adrien Guatto.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

pointer HM_Promote(GC_state s,
                   HM_chunkList dst_list,
                   pointer src) {
    assert(HM_isLevelHead(dst_list));
    HM_chunkList src_chunk = HM_getLevelHeadPathCompress(HM_getChunkOf(src));
    HM_chunkList tgtChunkList = dst_list;
    struct HM_HierarchicalHeap *dst_hh = dst_list->containingHH;
    bool needToUpdateLCS = false;

    struct HM_HierarchicalHeap *current_hh = getHierarchicalHeapCurrent(s);
    if (dst_list->level >= HM_HH_getLowestPrivateLevel(s, current_hh)) {
      assert (dst_hh == current_hh);
      needToUpdateLCS = true;
      dst_hh->locallyCollectibleSize -= tgtChunkList->size;
    }

    LOG(LM_HH_PROMOTION, LL_INFO,
        "Promoting src %p to chunk list %p",
        (void *)src, (void *)dst_list);

    Trace2(EVENT_PROMOTION_ENTER, (EventInt)src, (EventInt)dst_list);
    TraceResetCopy(); /* Reset copy events. */

    /* AG_NOTE is this needed? */
    getStackCurrent(s)->used = sizeofGCStateCurrentStackUsed (s);
    getThreadCurrent(s)->exnStack = s->exnStack;

    // assert (!dst_hh->newLevelList);

    // SAM_NOTE: TODO: I think this is broken. We need clearer invariants for
    // identifying intermediate valid states of an HH.
    HM_chunk tgtChunk = HM_getChunkListLastChunk(tgtChunkList);
    if (dst_hh->lastAllocatedChunk == tgtChunk ||
        !tgtChunk->mightContainMultipleObjects ||
        !inSameBlock((pointer)tgtChunk, tgtChunk->frontier)) {
      LOG(LM_HH_PROMOTION, LL_DEBUG,
          "Appending a new chunk. Chunk %p at level %u can't be used because:\n"
          "  in use? %s\n"
          "  single-object chunk? %s\n"
          "  variable-sized chunk? %s",
          (void*)HM_getChunkListLastChunk(tgtChunkList),
          tgtChunkList->level,
          dst_hh->lastAllocatedChunk == tgtChunk ? "yes" : "no",
          !tgtChunk->mightContainMultipleObjects ? "yes" : "no",
          !inSameBlock((pointer)tgtChunk, tgtChunk->frontier) ? "yes" : "no");
      HM_allocateChunk(tgtChunkList, GC_HEAP_LIMIT_SLOP);
    }

    struct ForwardHHObjptrArgs forwardHHObjptrArgs = {
        .hh = dst_hh,
        .minLevel = dst_list->level + 1,
        .maxLevel = src_chunk->level,
        .tgtChunkList = tgtChunkList,
        .bytesCopied = 0,
        .objectsCopied = 0,
        .stacksCopied = 0
    };

    LOG(LM_HH_PROMOTION, LL_DEBUG,
        "promoting %p to chunk %p:\n"
        "  scope is %u -> %u\n",
        (void *)src,
        (void *)dst_list,
        forwardHHObjptrArgs.minLevel,
        forwardHHObjptrArgs.maxLevel);

    objptr srcobj = pointerToObjptr(src, NULL);

    LOG(LM_HH_PROMOTION, LL_DEBUG, "START src copy");

    forwardHHObjptr(s, &srcobj, &forwardHHObjptrArgs);

    pointer start = foreachObjptrInObject(s,
                                          objptrToPointer(srcobj, NULL),
                                          FALSE,
                                          trueObjptrPredicate,
                                          NULL,
                                          forwardHHObjptr,
                                          &forwardHHObjptrArgs);

    LOG(LM_HH_PROMOTION, LL_DEBUG, "START copy loop at %p", (void *)start);

    HM_forwardHHObjptrsInChunkList(
        s,
        start,
        trueObjptrPredicate,
        NULL,
        &forwardHHObjptrArgs);

    /* We need to ensure some invariants.

       1/ Reset the to-space level list.

       2/ Reset the cached pointer from the from-space chunk list to the
       to-space chunk list.

       3/ Update locallyCollectibleSize if we have been promoting to a locally
       collectible level.
    */

    assert (forwardHHObjptrArgs.tgtChunkList);

    if (needToUpdateLCS) {
        dst_hh->locallyCollectibleSize += tgtChunkList->size;
    }

    assertInvariants(s, dst_hh, LIVE);

    s->cumulativeStatistics->bytesPromoted += forwardHHObjptrArgs.bytesCopied;

    /* Exit. */

    TraceResetCopy();
    Trace0(EVENT_PROMOTION_LEAVE);

    pointer res = objptrToPointer(srcobj, NULL);

    LOG(LM_HH_PROMOTION, LL_DEBUG,
        "Promoted src %p to replica %p",
        (void *)src, (void *)res);

    Trace2(EVENT_PROMOTION, (EventInt)(void *)src, (EventInt)(void *)res);

    return res;
}
