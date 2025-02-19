/**
 * @file concurrent-collection.c
 *
 * @author Jatin Arora
 *
 * @brief
 * Implementation of the concurrent collection interface
 */
#include "concurrent-collection.h"
#define SPLIT_SIZE 5



#if (defined (MLTON_GC_INTERNAL_BASIS))

void GC_updateObjectHeader(
  __attribute__((unused)) GC_state s,
  pointer p,
  GC_header newHeader)
{
  while (TRUE) {
    GC_header oldHeader = getHeader(p);
    GC_header desired = (oldHeader & MARK_MASK) | newHeader;
    if (__sync_bool_compare_and_swap(getHeaderp(p), oldHeader, desired)) {
      assert((oldHeader & MARK_MASK) == (desired & MARK_MASK));
      return;
    }
  }
}

#endif



#if (defined (MLTON_GC_INTERNAL_FUNCS))
#define casCC(F, O, N) ((__sync_val_compare_and_swap(F, O, N)))

// void forwardPtrChunk (GC_state s, objptr *opp, void* rawArgs);
void saveChunk(HM_chunk chunk, ConcurrentCollectArgs* args);

#define ASSERT2 0


enum CC_freedChunkType {
  CC_FREED_REMSET_CHUNK,
  CC_FREED_STACK_CHUNK,
  CC_FREED_NORMAL_CHUNK
};

static const char* CC_freedChunkTypeToString[] = {
  "CC_FREED_REMSET_CHUNK",
  "CC_FREED_STACK_CHUNK",
  "CC_FREED_NORMAL_CHUNK"
};

struct CC_chunkInfo {
  uint32_t initialDepth;
  uint32_t finalDepth;
  int32_t procNum;
  enum CC_freedChunkType freedType;
};


void CC_writeFreeChunkInfo(
  __attribute__((unused)) GC_state s,
  char* infoBuffer,
  size_t bufferLen,
  void* env)
{
  struct CC_chunkInfo *info = env;

  snprintf(infoBuffer, bufferLen,
    "freed %s by CC at depth [%u,%u] by proc %d",
    CC_freedChunkTypeToString[info->freedType],
    info->initialDepth,
    info->finalDepth,
    info->procNum);
}


void CC_initStack(GC_state s, ConcurrentPackage cp) {
  // don't re-initialize
  if(cp->rootList!=NULL) {
    return;
  }

  CC_stack* temp  = (struct CC_stack*) malloc(sizeof(struct CC_stack));
  CC_stack_init(s, temp);
  cp->rootList = temp;
}

void CC_addToStack (GC_state s, ConcurrentPackage cp, pointer p) {
  if(cp->rootList==NULL) {
    // Its NULL because we won't collect this heap. so no need to snapshot
    return;
    // LOG(LM_HH_COLLECTION, LL_FORCE, "Concurrent Stack is not initialised\n");
    // CC_initStack(cp);
  }
  CC_stack_push(s, cp->rootList, (void*)p);
}

void CC_clearStack(GC_state s, ConcurrentPackage cp) {
  if(cp->rootList!=NULL) {
    CC_stack_clear(s, cp->rootList);
  }
}

void CC_freeStack(GC_state s, ConcurrentPackage cp) {
  if(cp->rootList!=NULL) {
    CC_stack_free(s, cp->rootList);
    free(cp->rootList);
    cp->rootList = NULL;
  }
}

bool CC_closeStack(ConcurrentPackage cp, HM_chunkList removed) {
  return CC_stack_try_close(cp->rootList, removed);
}

bool CC_isPointerMarked (pointer p) {
  return ((MARK_MASK & getHeader (p)) == MARK_MASK);
}

bool chunkIsInList(HM_chunk chunk, HM_chunkList list) {
  for (HM_chunk cursor = list->firstChunk;
       cursor != NULL;
       cursor = cursor->nextChunk)
  {
    if (cursor == chunk)
      return TRUE;
  }
  return FALSE;
}

bool isChunkInFromSpace(HM_chunk chunk, ConcurrentCollectArgs* args) {
  return chunk->parentHeapId == args->parentHeapId && chunk->live == FALSE;
}

bool isChunkInToSpace(HM_chunk chunk, ConcurrentCollectArgs* args) {
  return chunk->parentHeapId == args->parentHeapId && chunk->live == TRUE;
}

// JATIN_NOTE: this function should be called only for in scope objects.
// for out of scope objects the assertion and sanctity of *p is uncertain
// This is because of local collection. In general, CC assumes that the access to chunk level info
// about p is safe since it does not depend on (*p). Once it is established that p is in scope,
// we can call getTransitivePtr on it.
// Only after the transitive pointer is reached can we begin to inspect
// (*p). Otherwise, all bets are off because of races
pointer getTransitivePtr(pointer p, ConcurrentCollectArgs* args) {
  objptr op;

  assert(isChunkInFromSpace(HM_getChunkOf(p), args) ||
          isChunkInToSpace(HM_getChunkOf(p), args));

  assert(!hasFwdPtr(p));

  while (hasFwdPtr(p)) {
    fprintf(stderr,
      "getTransitivePtr: "FMTPTR" has forwarding ptr",
      (uintptr_t)p);

    HM_chunk chunk = HM_getChunkOf(p);
    if(chunk->tmpHeap == args->fromHead){
      saveChunk(chunk, args);
    }
    op = getFwdPtr(p);
    p = objptrToPointer(op, NULL);
  }
  return p;
}

void markObj(pointer p) {
  __sync_fetch_and_xor(getHeaderp(p), MARK_MASK);
  // GC_header* headerp = getHeaderp(p);
  // GC_header header = *headerp;
  // header ^= MARK_MASK;
  // *headerp = header;
}

// V1: make marking the object safe using compare and swap
// void markObj(pointer p) {
//   GC_header header = getHeader(p);
//   GC_header oldValue;
//   GC_header newValue;
//
//   do {
//     oldValue = header;                     // Read the current value of the header
//     if (oldValue & MARK_MASK) {            // If already marked, no action is needed
//       return;
//     }
//     newValue = oldValue | MARK_MASK;       // Prepare the new value with the MARK_MASK bit set
//   } while (!__sync_bool_compare_and_swap(getHeaderp(p), oldValue, newValue)); // CAS loop
// }


// This function is exactly the same as in chunk.c.
// The only difference is, it doesn't NULL the levelHead of the unlinking chunk.
// TODO: replace with HM_unlinkChunkPreserveLevelHead (see chunk.c)
void CC_HM_unlinkChunk(HM_chunkList list, HM_chunk chunk) {
  // assert(chunkIsInList(chunk, list));

  if (NULL == chunk->prevChunk) {
    assert(list->firstChunk == chunk);
    list->firstChunk = chunk->nextChunk;
  } else {
    assert(list->firstChunk != chunk);
    chunk->prevChunk->nextChunk = chunk->nextChunk;
  }

  if (NULL == chunk->nextChunk) {
    assert(list->lastChunk == chunk);
    list->lastChunk = chunk->prevChunk;
  } else {
    assert(list->lastChunk != chunk);
    chunk->nextChunk->prevChunk = chunk->prevChunk;
  }

  list->size -= HM_getChunkSize(chunk);
  list->usedSize -= HM_getChunkUsedSize(chunk);

  chunk->prevChunk = NULL;
  chunk->nextChunk = NULL;
}

void saveChunk(HM_chunk chunk, ConcurrentCollectArgs* args) {
  assert(chunk->live == FALSE);
  chunk->live = TRUE;
}

bool saveNoForward(
  __attribute__((unused)) GC_state s,
  pointer p,
  void* rawArgs)
{
  ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;

  HM_chunk cand_chunk = HM_getChunkOf(p);
  bool chunkSaved = isChunkInToSpace(cand_chunk, args);
  bool chunkOrig  = (chunkSaved)?TRUE:isChunkInFromSpace(cand_chunk, args);

  if(chunkOrig && !chunkSaved) {
    assert(isChunkInFromSpace(cand_chunk, args));
    assert(getTransitivePtr(p, rawArgs) == p);
    saveChunk(cand_chunk, args);
  }

  bool result = (chunkSaved || chunkOrig);

  assert((!result) || isChunkInToSpace(cand_chunk, args));
  return result;
}

#if 0
void markAndScan(GC_state s, pointer p, void* rawArgs) {
  ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;

  if(!CC_isPointerMarked(p)) {
    markObj(p);
    args->bytesSaved += sizeofObject(s, p);
    assert(CC_isPointerMarked(p));

    struct GC_foreachObjptrClosure forwardPtrClosure =
    {.fun = forwardPtrChunk, .env = rawArgs};

    foreachObjptrInObject(s, p, &trueObjptrPredicateClosure,
            &forwardPtrClosure, FALSE);
  }
}
#endif

// some debugging functions
void printObjPtrInScopeFunction(
  __attribute__((unused)) GC_state s,
  objptr* opp,
  void* rawArgs)
{
  objptr op = *opp;
  assert(isObjptr(op));
  pointer p = objptrToPointer (op, NULL);
  if (isChunkInFromSpace(HM_getChunkOf(p), rawArgs)
     && !isChunkInToSpace(HM_getChunkOf(p), rawArgs)) {
    printf("%p \n", (void *) *opp);
  }
}

void printObjPtrFunction(
  __attribute__((unused)) GC_state s,
  objptr* opp,
  __attribute__((unused)) void* rawArgs)
{
  printf("\t %p", (void*) *opp);
}

void printDownPtrs(
  __attribute__((unused)) GC_state s,
  objptr dst,
  objptr* field,
  objptr src,
  __attribute__((unused)) void* rawArgs)
{
  printf("(%p, %p, %p) ", (void*)dst, (void*)field, (void*)src);
}

void printChunkListSize(HM_chunkList list) {
  printf("sizes: ");
  for(HM_chunk chunk = list->firstChunk; chunk!=NULL; chunk = chunk->nextChunk) {
    printf("%zu ", HM_getChunkSize(chunk));
  }
  printf("\n");
}

bool isChunkInList(HM_chunk chunk, HM_chunkList list) {
  for(HM_chunk c = list->firstChunk; c!=NULL; c=c->nextChunk) {
    if(c == chunk)
      return TRUE;
  }
  return FALSE;
}

void tryMarkAndAddToWorkList(
  GC_state s,
  __attribute__((unused)) objptr *opp,
  objptr op,
  void* rawArgs)
{
  ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;
  assert(isObjptr(op));
  pointer p = objptrToPointer (op, NULL);

  bool isInScope = saveNoForward(s, p, rawArgs);

  if (!isInScope)
    return;

  if (!CC_isPointerMarked(p)) {
    markObj(p);
    args->bytesSaved += sizeofObject(s, p);
    args->numObjectsMarked++;
    assert(CC_isPointerMarked(p));
    CC_workList_push(s, &(args->worklist), op);
  }
}

void tryUnmarkAndAddToWorkList(
  GC_state s,
  __attribute__((unused)) objptr *opp,
  objptr op,
  void* rawArgs)
{
  ConcurrentCollectArgs* args = (ConcurrentCollectArgs*)rawArgs;
  assert(isObjptr(op));
  pointer p = objptrToPointer (op, NULL);
  HM_chunk chunk = HM_getChunkOf(p);

  if (!isChunkInToSpace(chunk, rawArgs)) {
    return;
  }

  if (CC_isPointerMarked(p)) {
    assert(isChunkInToSpace(chunk, args));
    markObj(p);
    assert(!CC_isPointerMarked(p));
    CC_workList_push(s, &(args->worklist), op);
  }
}

void markLoop(GC_state s, ConcurrentCollectArgs* args) {
  struct GC_foreachObjptrClosure markAddClosure =
    {.fun = tryMarkAndAddToWorkList, .env = (void*)args};

  CC_workList worklist = &(args->worklist);

  objptr* current = CC_workList_pop(s, worklist);
  while (NULL != current) {
    callIfIsObjptr(s, &markAddClosure, current);
    current = CC_workList_pop(s, worklist);
  }

  assert(CC_workList_isEmpty(s, worklist));
}

void unmarkLoop(GC_state s, ConcurrentCollectArgs* args) {
  struct GC_foreachObjptrClosure unmarkAddClosure =
    {.fun = tryUnmarkAndAddToWorkList, .env = (void*)args};

  CC_workList worklist = &(args->worklist);

  objptr* current = CC_workList_pop(s, worklist);
  while (NULL != current) {
    callIfIsObjptr(s, &unmarkAddClosure, current);
    current = CC_workList_pop(s, worklist);
  }

  assert(CC_workList_isEmpty(s, worklist));
}

void tryMarkAndMarkLoop(GC_state s, objptr *opp, objptr op, void* rawArgs) {
  tryMarkAndAddToWorkList(s, opp, op, rawArgs);
  markLoop(s, rawArgs);
}

void tryUnmarkAndUnmarkLoop(GC_state s, objptr *opp, objptr op, void* rawArgs) {
  tryUnmarkAndAddToWorkList(s, opp, op, rawArgs);
  unmarkLoop(s, rawArgs);
}

#if 0
void forwardPtrChunk (GC_state s, objptr *opp, void* rawArgs) {
  objptr op = *opp;
  assert(isObjptr(op));
  pointer p = objptrToPointer (op, NULL);

  // SAM_NOTE: can just remove this???
  if (isChunkInFromSpace(HM_getChunkOf(p), rawArgs)
     || isChunkInToSpace(HM_getChunkOf(p), rawArgs)) {
    p = getTransitivePtr(p, rawArgs);
  }

  bool saved = saveNoForward(s, p, rawArgs);

  if(saved) {
    markAndScan(s, p, rawArgs);
  }
}
#endif

void forwardPinned(GC_state s, HM_remembered remElem, void* rawArgs) {
  objptr src = remElem->object;
  tryMarkAndMarkLoop(s, &src, src, rawArgs);
  if (remElem->from != BOGUS_OBJPTR) {
    tryMarkAndMarkLoop(s, &(remElem->from), remElem->from, rawArgs);
  }

#if 0
#if ASSERT
  HM_chunk fromChunk = HM_getChunkOf(objptrToPointer(remElem->from, NULL));
  HM_chunk objChunk = HM_getChunkOf(objptrToPointer(remElem->object, NULL));
  assert(!isChunkInFromSpace(fromChunk, rawArgs));
  assert(!isChunkInToSpace(fromChunk, rawArgs));
  assert(
    HM_HH_getDepth(HM_getLevelHead(fromChunk))
    <=
    HM_HH_getDepth(HM_getLevelHead(objChunk))
  );
#endif
#endif

  // the runtime needs dst to be saved in case it is in the scope of collection.
  // can potentially remove the downPointer, but there are some race issues with the write Barrier
  // forwardPtrChunk(s, &dst, rawArgs);
}

#if 0
void unmarkPtrChunk(GC_state s, objptr* opp, void* rawArgs) {

#if ASSERT
  ConcurrentCollectArgs *args = (ConcurrentCollectArgs*)rawArgs;
#endif

  objptr op = *opp;
  assert(isObjptr(op));

  pointer p = objptrToPointer (op, NULL);
  HM_chunk chunk = HM_getChunkOf(p);

  // check that getTransitivePtr can be called.
  if (!isChunkInToSpace(chunk, rawArgs)) {
    return;
  }

  p = getTransitivePtr(p, rawArgs);

  assert(!isChunkInFromSpace(chunk, args));

  if(CC_isPointerMarked (p)) {
    assert(isChunkInToSpace(chunk, args));
    // assert(chunk->tmpHeap == ((ConcurrentCollectArgs*)rawArgs)->toHead);
    markObj(p);

    assert(!CC_isPointerMarked(p));

    struct GC_foreachObjptrClosure unmarkPtrClosure =
    {.fun = unmarkPtrChunk, .env = rawArgs};
    foreachObjptrInObject(s, p, &trueObjptrPredicateClosure,
            &unmarkPtrClosure, FALSE);
  }
}
#endif

// void checkRemEntry(
//   __attribute__((unused)) GC_state s,
//   ARG_USED_FOR_ASSERT HM_remembered remElem,
//   ARG_USED_FOR_ASSERT void* rawArgs)
// {
// #if ASSERT
//   ConcurrentCollectArgs *args = (ConcurrentCollectArgs*)rawArgs;
//   assert(
//     isChunkInList(
//       HM_getChunkOf(objptrToPointer(remElem->object, NULL)),
//       args->origList)
//   );
// #endif
// }

void unmarkPinned(
  GC_state s,
  HM_remembered remElem,
  void* rawArgs)
{
  objptr src = remElem->object;
  assert(!(HM_getChunkOf(objptrToPointer(src, NULL))->pinnedDuringCollection));
  tryUnmarkAndUnmarkLoop(s, &src, src, rawArgs);
  if (remElem->from != BOGUS_OBJPTR) {
    tryUnmarkAndUnmarkLoop(s, &(remElem->from), remElem->from, rawArgs);
  }
  // unmarkPtrChunk(s, &src, rawArgs);
  // unmarkPtrChunk(s, &(remElem->from), rawArgs);

#if 0
#if ASSERT
  HM_chunk fromChunk = HM_getChunkOf(objptrToPointer(remElem->from, NULL));
  HM_chunk objChunk = HM_getChunkOf(objptrToPointer(remElem->object, NULL));
  assert(!isChunkInFromSpace(fromChunk, rawArgs));
  assert(!isChunkInToSpace(fromChunk, rawArgs));
  assert(
    HM_HH_getDepth(HM_getLevelHead(fromChunk))
    <=
    HM_HH_getDepth(HM_getLevelHead(objChunk))
  );
#endif
#endif

  // TODO: SAM_NOTE: check this??
  // unmarkPtrChunk(s, &dst, rawArgs);
}

// This function does more than forwardPtrChunk.
// It scans the object pointed by the pointer even if its not in scope.
// Recursively however it only calls forwardPtrChunk and not itself
void forceForward(GC_state s, objptr *opp, void* rawArgs) {
  ConcurrentCollectArgs *args = (ConcurrentCollectArgs*)rawArgs;
  objptr op = *opp;
  pointer p = objptrToPointer(op, NULL);

  if (!isObjptr(op)) {
    return;
  }

  assert(isObjptr(op));

  bool saved = saveNoForward(s, p, rawArgs);

  if(saved && !CC_isPointerMarked(p)) {
    assert(getTransitivePtr(p, rawArgs) == p);
    markObj(p);
    assert(CC_isPointerMarked(p));
    args->bytesSaved += sizeofObject(s, p);
    args->numObjectsMarked++;
  }

  CC_workList_push(s, &(args->worklist), op);
  markLoop(s, rawArgs);
}

void forceUnmark (GC_state s, objptr* opp, void* rawArgs) {
  ConcurrentCollectArgs *args = (ConcurrentCollectArgs*)rawArgs;
  objptr op = *opp;
  pointer p = objptrToPointer(op, NULL);

  if (!isObjptr(op)) {
    return;
  }

  assert(isObjptr(op));

  if(CC_isPointerMarked(p)){
    assert(getTransitivePtr(p, rawArgs) == p);
    markObj(p);
    assert(!CC_isPointerMarked(p));
  }

  CC_workList_push(s, &(args->worklist), op);
  unmarkLoop(s, rawArgs);
}

void ensureCallSanity(
  __attribute__((unused)) GC_state s,
  ARG_USED_FOR_ASSERT HM_HierarchicalHeap targetHH,
  ARG_USED_FOR_ASSERT ConcurrentPackage args)
{
  assert(targetHH!=NULL);
  assert(args!=NULL);
  assert(args->snapLeft!=BOGUS_OBJPTR);
  assert(args->snapRight!=BOGUS_OBJPTR);
}


bool checkLocalScheduler (GC_state s) {
  return !( NONE == s->controls->collectionType ||
           s->wsQueueTop == BOGUS_OBJPTR ||
           s->wsQueueBot == BOGUS_OBJPTR
         );
}

HM_HierarchicalHeap findHeap (GC_thread thread, uint32_t depth) {

  HM_HierarchicalHeap currentHeap = thread->hierarchicalHeap;
  while(currentHeap!=NULL && HM_HH_getDepth (currentHeap) != depth) {
    currentHeap = currentHeap->nextAncestor;
  }

  if(currentHeap==NULL)
    return NULL;

  assert(HM_HH_getDepth(currentHeap) == depth);
  return currentHeap;
}

bool readyforCollection(ConcurrentPackage cp) {
  bool ready =  (cp != NULL && cp->ccstate == CC_REG);
  if (ready) {
    assert(cp->rootList!=NULL);
    assert(cp->snapLeft != BOGUS_OBJPTR);
    assert(cp->snapRight != BOGUS_OBJPTR);
    assert(cp->stack != BOGUS_OBJPTR);
  }
  return ready;
}

// returns true if claim succeeds
bool claimHeap(HM_HierarchicalHeap heap) {
  if (heap == NULL) {
    return FALSE;
  }

  ConcurrentPackage cp = HM_HH_getConcurrentPack(heap);
  if(!readyforCollection(cp)) {
    return FALSE;
  }
  else if (casCC (&(cp->ccstate), CC_REG, CC_COLLECTING) != CC_REG) {
    printf("\t %s\n", "returning because someone else claimed collection");
    return FALSE;
  }
  assert(HM_HH_getConcurrentPack(heap)->ccstate == CC_COLLECTING);
  return TRUE;
}

void CC_collectAtRoot(pointer threadp, pointer hhp) {
  GC_state s = pthread_getspecific (gcstate_key);
  GC_thread thread = threadObjptrToStruct(s, pointerToObjptr(threadp, NULL));
  HM_HierarchicalHeap heap = (HM_HierarchicalHeap)hhp;

  if (!checkLocalScheduler(s) || thread->currentDepth<=0) {
    return;
  }

  if (!claimHeap(heap)) {
    return;
  }

  // for exiting even if CC is going on.
  assert(NULL == s->currentCCTargetHH);
  s->currentCCTargetHH = heap;
  // assert(!s->amInCC);
  // s->amInCC = TRUE;

#if ASSERT
  for (int other = 0; other < (int)s->numberOfProcs; other++) {
    if (other != s->procNumber)
      assert(heap != s->procStates[other].currentCCTargetHH);
  }
#endif

  size_t beforeSize = HM_getChunkListSize(HM_HH_getChunkList(heap));
  size_t live = 0;
  size_t numObjectsMarked = 0;
  CC_collectWithRoots(s, heap, thread, &live, &numObjectsMarked);
  size_t afterSize = HM_getChunkListSize(HM_HH_getChunkList(heap));

  size_t diff = beforeSize > afterSize ? beforeSize - afterSize : 0;

  LOG(LM_CC_COLLECTION, LL_INFO,
    "finished at depth %u. before: %zu after: %zu (-%.01lf%%) live: %zu (%.01lf%% fragmented) objects: %zu",
    heap->depth,
    beforeSize,
    afterSize,
    100.0 * ((double)diff / (double)beforeSize),
    live,
    100.0 * (1.0 - (double)live / (double)afterSize),
    numObjectsMarked);

  // HM_HH_getConcurrentPack(heap)->ccstate = CC_UNREG;
  __atomic_store_n(&(HM_HH_getConcurrentPack(heap)->ccstate), CC_DONE, __ATOMIC_SEQ_CST);
  // s->amInCC = FALSE;
  s->currentCCTargetHH = NULL;
}

uint32_t minPrivateLevel(GC_state s) {
  uint64_t topval = *(uint64_t*)objptrToPointer(s->wsQueueTop, NULL);
  uint32_t shallowestPrivateLevel = UNPACK_IDX(topval);
  uint32_t level = (shallowestPrivateLevel>0)?(shallowestPrivateLevel-1):0;
  return level;
}

void CC_collectAtPublicLevel(GC_state s, GC_thread thread, uint32_t depth) {
  checkLocalScheduler(s);
  if (thread->currentDepth <= 1
    || depth <= 0
    || depth >= thread->currentDepth
    // Don't collect heaps that are private
    || depth > minPrivateLevel(s)
    ) {
    return;
  }

  HM_HierarchicalHeap heap = findHeap(thread, depth);
  if(!claimHeap(heap)){
    return;
  }

  // collect only if the heap is above a threshold size
  if (HM_getChunkListSize(&(heap->chunkList)) >= 2 * HM_BLOCK_SIZE) {
    assert(getThreadCurrent(s) == thread);
    CC_collectWithRoots(s, heap, thread, NULL, NULL);
  }

  // Mark that collection is complete
  __atomic_store_n(&(HM_HH_getConcurrentPack(heap)->ccstate), CC_DONE, __ATOMIC_SEQ_CST);
  // HM_HH_getConcurrentPack(heap)->ccstate = CC_UNREG;
}

/* ========================================================================= */

struct CC_tryUnpinOrKeepPinnedArgs {
  HM_remSet newRemSet;
  HM_HierarchicalHeap tgtHeap;
  uint32_t parentHeapId;
};

void CC_tryUnpinOrKeepPinned(
    __attribute__((unused)) GC_state s,
    HM_remembered remElem,
    void *rawArgs)
{
  struct CC_tryUnpinOrKeepPinnedArgs* args =
    (struct CC_tryUnpinOrKeepPinnedArgs *)rawArgs;
  HM_chunk chunk = HM_getChunkOf(objptrToPointer(remElem->object, NULL));

#if ASSERT
  assert(isPinned(remElem->object));
  // KKG_TODO_F: visit this and see if this assert makes sense
  //assert(chunk->tmpHeap != args->toSpaceMarker);
  assert(chunk->parentHeapId != args->parentHeapId || chunk->live != TRUE);
#endif

#if 0
  if (chunk->tmpHeap != args->fromSpaceMarker)
  {
    /** outside scope of CC. must be entangled? just keep it remembered and
      * move on.
      */
    assert(s->controls->manageEntanglement);
    HM_remember(args->newRemSet, remElem);
    return;
  }
#endif

  // KKG_TODO_F: check if the markers can be updated according to the new logic of fromSpace and toSpace
  // i.e., consider parent heap id and liveness, not just liveness
  // so this below if condition is not correct
  if (chunk->parentHeapId != args->parentHeapId || chunk->live != FALSE) {
  // if (chunk->tmpHeap != args->fromSpaceMarker) {
    /** It's possible to have a remset entry for an object elsewhere in the
      * chain. (When adding an remset entry for object at ancestor, this
      * object might live in the chain rather than the primary heap. Recall,
      * all remset entries are redirected to the primary heap. When CC is later
      * spawned for the primary heap, it's not guaranteed that all chain CCs
      * will have completed and been merged in the meantime.)
      *
      * The correct thing to do here is therefore to just keep the remset
      * entry. It will be merged and handled properly later.
      */

    HM_remember(args->newRemSet, remElem, false);
    return;
  }

  assert(isChunkInList(chunk, HM_HH_getChunkList(args->tgtHeap)));
  // KKG_TODO_F_DOUBT: same as before, this marker usage needs to be carefully checked
  // V1: this doesn't make sense, args->fromSpaceMarker is not modified in HM_remember function
  assert(chunk->parentHeapId == args->parentHeapId && chunk->live == FALSE);
  // assert(chunk->tmpHeap == args->fromSpaceMarker);
  assert(HM_getLevelHead(chunk) == args->tgtHeap);

  // pointer p = objptrToPointer(remElem->object, NULL);
  // GC_header header = getHeader(p);
  // enum PinType pt = pinType(header);
  // uint32_t unpinDepth = unpinDepthOf(remElem->object);

  // assert (pt != PIN_NONE);

  if (remElem->from != BOGUS_OBJPTR)
  {

    // uint32_t opDepth = HM_HH_getDepth(args->tgtHeap);
    // if (unpinDepth > opDepth && pt == PIN_ANY) {
      // tryPinDec(remElem->object, opDepth);
    // }

    HM_chunk fromChunk = HM_getChunkOf(objptrToPointer(remElem->from, NULL));
    // KKG_TODO_F: more of this
    // assert(fromChunk->tmpHeap != args->toSpaceMarker);
    assert(fromChunk->parentHeapId != args->parentHeapId || fromChunk->live != TRUE);

    // if (fromChunk->tmpHeap == args->fromSpaceMarker) {
    // KKG_TODO_F: more of this
    if (fromChunk->parentHeapId == args->parentHeapId && fromChunk->live == FALSE) {
      assert(isChunkInList(fromChunk, HM_HH_getChunkList(args->tgtHeap)));
      return;
    }

    /* otherwise, object stays pinned, and we have to keep this remembered
    * entry into the toSpace. */
  } else {
    GC_header header = getHeader(objptrToPointer(remElem->object, NULL));
    if (pinType(header) == PIN_ANY &&
      unpinDepthOfH(header) >= HM_HH_getDepth(args->tgtHeap)) {
      return;
    }
  }

  HM_remember(args->newRemSet, remElem, false);

  /** SAM_NOTE: The goal of the following was to filter remset entries
    * to only keep the "shallowest" entries. But this is really tricky,
    * because all three of the unpinDepth, opDepth, and fromDepth can
    * concurrently change (due to joins, concurrent pins, etc.)
    *
    * Perhaps the concurrency really isn't that bad, because we know that
    * all three values can only decrease due to concurrent operations. But
    * for now, I don't feel like working through all the cases.
    */
#if 0
  uint32_t unpinDepth = unpinDepthOf(op);
  uint32_t fromDepth = HM_HH_getDepth(HM_getLevelHead(fromChunk));
  if (fromDepth > unpinDepth) {
    /** Can forget any down-pointer that came from shallower than the
      * shallowest from-object.
      */
    return;
  }
  assert(opDepth < unpinDepth || fromDepth == unpinDepth);
#endif

  assert(isChunkInList(chunk, HM_HH_getChunkList(args->tgtHeap)));
  assert(HM_getLevelHead(chunk) == args->tgtHeap);
}


void CC_filterPinned(
  GC_state s,
  uint32_t initialDepth,
  HM_HierarchicalHeap hh,
  uint32_t parentHeapId)
{
  HM_remSet oldRemSet = HM_HH_getRemSet(hh);
  struct HM_remSet newRemSet;
  HM_initRemSet(&newRemSet);

  LOG(LM_CC_COLLECTION, LL_INFO,
    "num pinned initially: %zu",
    HM_numRemembered(HM_HH_getRemSet(hh)));

  // KKG_TODO_F: do you want to retain these markers?
  struct CC_tryUnpinOrKeepPinnedArgs args =
    { .newRemSet = &newRemSet
    , .tgtHeap = hh
    , .parentHeapId = parentHeapId
    };

  struct HM_foreachDownptrClosure closure =
    { .fun = CC_tryUnpinOrKeepPinned
    , .env = (void*)&args
    };

  /** Save "valid" entries to newRemSet, throw away old entries, and store
    * valid entries back into the main remembered set.
    */
  HM_foreachRemembered(s, oldRemSet, &closure, false);

  struct CC_chunkInfo info =
    {.initialDepth = initialDepth,
     .finalDepth = HM_HH_getDepth(hh),
     .procNum = s->procNumber,
     .freedType = CC_FREED_REMSET_CHUNK};
  struct writeFreedBlockInfoFnClosure infoc =
    {.fun = CC_writeFreeChunkInfo, .env = &info};

  // HM_freeRemSetWithInfo(s, oldRemSet, &infoc);
  // this reintializes the private remset
  HM_freeChunksInListWithInfo(s, &(oldRemSet->private), &infoc, BLOCK_FOR_REMEMBERED_SET);
  assert (newRemSet.public.firstChunk == NULL);
  // this moves all data into remset of hh
  HM_appendRemSet(oldRemSet, &newRemSet);

  // assert(HM_HH_getRemSet(hh)->firstChunk == newRemSet.firstChunk);
  // assert(HM_HH_getRemSet(hh)->lastChunk == newRemSet.lastChunk);
  // assert(HM_HH_getRemSet(hh)->size == newRemSet.size);
  // assert(HM_HH_getRemSet(hh)->usedSize == newRemSet.usedSize);

  LOG(LM_CC_COLLECTION, LL_INFO,
    "num pinned after filter: %zu",
    HM_numRemembered(HM_HH_getRemSet(hh)));
}

/* ========================================================================= */

#if 0
void CC_filterDownPointers(GC_state s, HM_chunkList x, HM_HierarchicalHeap hh){
  /** There is no race here, because for truly concurrent GC (depth 1), the
    * hh has been split.
    */

  DIE(
    "TODO: CC_filterDownPointers: need to handle pinning. "
    "Fix this function as well as HM_foreachRemembered calls in "
    "CC_collectWithRoots"
  );

  HM_chunkList y = HM_HH_getRemSet(hh);
  struct HM_foreachDownptrClosure bucketIfValidAtListClosure =
  {.fun = bucketIfValidAtList, .env = (void*)x};

  /** Save "valid" entries to x, throw away old entries in y, before storing
    * valid entries back in y.
    */
  HM_foreachRemembered(s, y, &bucketIfValidAtListClosure);
  HM_freeChunksInList(s, y);
  *y = *x;
}
#endif


CGC_process* initializeCGC(GC_state s,
  HM_HierarchicalHeap targetHH,
  __attribute__((unused)) GC_thread thread) {

  CGC_process* cgc_process = malloc(sizeof(CGC_process));

  getStackCurrent(s)->used = sizeofGCStateCurrentStackUsed(s);
  getThreadCurrent(s)->exnStack = s->exnStack;
  HM_HH_updateValues(getThreadCurrent(s), s->frontier);

  Trace0(EVENT_CGC_ENTER);
  timespec_now(&cgc_process->startTime);

  LOG(LM_CC_COLLECTION, LL_INFO,
    "CC collecting heap %p at depth %u",
    (void*)targetHH,
    HM_HH_getDepth(targetHH));

  ConcurrentPackage cp = HM_HH_getConcurrentPack(targetHH);
  cgc_process->concurrent_package = cp;
  HM_assertChunkListInvariants(HM_HH_getChunkList(targetHH));
  ensureCallSanity(s, targetHH, cp);
  // At the end of collection, repList will contain all the chunks that have
  // some object that is reachable from the roots. origList will contain the
  // chunks in which all objects are garbage. Before exiting, chunks in
  // origList are added to the free list.

  uint32_t initialDepth = HM_HH_getDepth(targetHH);
  cgc_process->initialDepth = initialDepth;

  // V1: removing origList and repList as no longer needed, reusing origList in the sense of initial chunkList from targetHH
  // struct HM_chunkList _repList;
  // HM_chunkList repList = malloc(sizeof(struct HM_chunkList));
  // HM_initChunkList(repList);
  HM_chunkList origList = malloc(sizeof(struct HM_chunkList));
  // orig list remains a chunklist
  origList = HM_HH_getChunkList(targetHH);
  HM_assertChunkListInvariants(origList);

  // ConcurrentCollectArgs lists = {
  //   .origList = origList,
  //   .repList  = repList,
  //   .toHead = (void*)repList,
  //   .fromHead = (void*) &(origList),
  //   .bytesSaved = 0,
  //   .numObjectsMarked = 0
  // };

  // might overflow with uint32_t
  // KKG_TODO: push the id creation logic into a function and call it here - overflow check can be done inside this
  const uint32_t parentHeapId = (s->procNumber+s->cumulativeStatistics->numCCs) * (s->procNumber+s->cumulativeStatistics->numCCs+1)/2 + s->cumulativeStatistics->numCCs;

  ConcurrentCollectArgs *lists = malloc(sizeof(ConcurrentCollectArgs));
  if (lists) {
    // V1: Removing usage of lists
    lists->origList = origList;
    // lists->repList = repList;
    // lists->toHead = (void*)repList;
    // lists->fromHead = (void*) &(origList);
    lists->bytesSaved = 0;
    lists->numObjectsMarked = 0;
    lists->parentHeapId = parentHeapId;
  }
  else {
    DIE("lists malloc failed\n");
  }
  CC_workList_init(s, &(lists->worklist));
  cgc_process->lists = lists;

  HH_EBR_enterQuiescentState(s);

  // JATIN_NOTE: Some HM_hierarchical objects in origList
  // might not be reachable from the mutator roots.
  // So, I assign the levelHead of all chunks in the list to targetHH.
  // Then I explicitly preserve the chunk that contains targetHH.
  for(HM_chunk T = HM_getChunkListFirstChunk(origList); T!=NULL; T = T->nextChunk) {
#if ASSERT2
    // there were some issues with stack chunks
    // preserving for future debugging
      HM_chunk stackChunk =  HM_getChunkOf(thread->stack);
      if (T!=stackChunk && HM_getLevelHead(T) != targetHH) {
        printf("%s\n", "this is failing");
        assert(0);
      }
#endif

    assert(T->tmpHeap == NULL);
    // T->tmpHeap = lists->fromHead; // setting every node in origList to fromHead flag
    // V1 : setting live field to false initially
    T->live = FALSE;
    // V1: setting the parentHeapId to a unique number using the cantor pairing function
    T->parentHeapId = (s->procNumber+s->cumulativeStatistics->numCCs) * (s->procNumber+s->cumulativeStatistics->numCCs+1)/2 + s->cumulativeStatistics->numCCs;
    T->levelHead = HM_HH_getUFNode(targetHH);
    assert(T->levelHead->representative == NULL);
    assert(T->levelHead->payload == targetHH);
  }

  // forward down pointers
  // struct HM_chunkList downPtrs;
  // HM_initChunkList(&downPtrs);
  // CC_filterDownPointers(s, &downPtrs, targetHH);

  // struct HM_chunkList pinnedChunks;
  // HM_initChunkList(&pinnedChunks);
  CC_filterPinned(s, initialDepth, targetHH, lists->parentHeapId);

  struct HM_foreachDownptrClosure forwardPinnedClosure =
    {.fun = forwardPinned, .env = (void*)lists};
  HM_foreachRemembered(s, HM_HH_getRemSet(targetHH), &forwardPinnedClosure, false);

  // forward closures, stack and deque?
  // snap - additional heap objects registered as garbage collection roots in addition to the already exisiting roots
  // multiple start nodes - identify all reachable nodes
  forceForward(s, &(cp->snapLeft), lists);
  forceForward(s, &(cp->snapRight), lists);
  forceForward(s, &(cp->snapTemp), lists);
  // forceForward(s, &(s->wsQueue), lists);
  forceForward(s, &(cp->stack), lists);
  forceForward(s, &(cp->additionalStack), lists);

  return cgc_process;
}

bool isDone(GC_state s, CGC_process* cgc_process) {
  CC_workList worklist = &(cgc_process->lists->worklist);
  return CC_workList_isEmpty(s, worklist);
}

void finalizeCC(GC_state s,
  HM_HierarchicalHeap targetHH,
  __attribute__((unused)) GC_thread thread,
  size_t *outputBytesSaved,
  size_t *outputNumObjectsMarked,
  CGC_process *cgc_process) {

  // JATIN_NOTE: This is important because the stack object of the thread we are collecting
  // often changes the level it is at. So it might in fact be at depth = 1.
  // It is important that we only mark the stack and not scan it.
  // Scanning the stack races with the thread using it.


  struct HM_chunkList removedFromCCBag_;
  HM_chunkList removedFromCCBag = &removedFromCCBag_;
  HM_initChunkList(removedFromCCBag);

  struct HM_chunkList tempRemovedFromCCBag_;
  HM_chunkList tempRemovedFromCCBag = &tempRemovedFromCCBag_;
  HM_initChunkList(tempRemovedFromCCBag);

  ConcurrentPackage cp = cgc_process -> concurrent_package;
  ConcurrentCollectArgs *lists = cgc_process -> lists;

  // this is where we are checking if rootList is empty or not
  // if not empty, keep doing marking until empty
  while (!CC_closeStack(cp, tempRemovedFromCCBag)) {
    forEachObjptrInCCStackBag(
      s,
      tempRemovedFromCCBag,
      tryMarkAndMarkLoop, // V1: this needs to be fixed - several places where marking is called - handle parallelism in all cases
      lists);
    HM_appendChunkList(removedFromCCBag, tempRemovedFromCCBag);
    HM_initChunkList(tempRemovedFromCCBag);

    markLoop(s, lists);
  }

  assert(CC_workList_isEmpty(s, &(lists->worklist))); // --> all of marking is done
  assert(NULL == tempRemovedFromCCBag->firstChunk);

  // saveNoForward(s, (void*)(thread->stack), &lists);
  // saveNoForward(s, (void*)thread, &lists);

  // forEachObjptrinStack(s, cp->rootList, forwardPtrChunk, &lists);

// #if ASSERT
//   if (HM_HH_getDepth(targetHH) != 1){
//     struct GC_foreachObjptrClosure printObjPtrInScopeClosure =
//     {.fun = printObjPtrInScopeFunction, .env =  &lists};
//     foreachObjptrInObject(
//       s,
//       objptrToPointer(thread->stack, NULL),
//       &trueObjptrPredicateClosure,
//       &printObjPtrInScopeClosure,
//       FALSE);
//   }
// #endif

  struct HM_foreachDownptrClosure unmarkPinnedClosure =
    {.fun = unmarkPinned, .env = lists};
  HM_foreachRemembered(s, HM_HH_getRemSet(targetHH), &unmarkPinnedClosure, false);

  forceUnmark(s, &(cp->snapLeft), lists);
  forceUnmark(s, &(cp->snapRight), lists);
  forceUnmark(s, &(cp->snapTemp), lists);
  // forceUnmark(s, &(s->wsQueue), lists);
  forceUnmark(s, &(cp->stack), lists);
  forceUnmark(s, &(cp->additionalStack), lists);

  unmarkLoop(s, lists);

  // forEachObjptrinStack(s, cp->rootList, unmarkPtrChunk, lists);
  forEachObjptrInCCStackBag(s, removedFromCCBag, tryUnmarkAndUnmarkLoop, lists);
  unmarkLoop(s, lists);

  HM_freeChunksInListWithInfo(s, removedFromCCBag, NULL, BLOCK_FOR_FORGOTTEN_SET);

  assert(CC_workList_isEmpty(s, &(lists->worklist)));
  CC_workList_free(s, &(lists->worklist));

#if ASSERT2 // just contains code that is sometimes useful for debugging.
  HM_assertChunkListInvariants(origList);
  HM_assertChunkListInvariants(repList);

  int lenFree = 0;
  int lenRep = 0;
  HM_chunk Q = origList->firstChunk;
  // assert(Q==NULL);
  while(Q!=NULL) {
    lenFree++;
    Q = Q->nextChunk;
  }

  Q = repList->firstChunk;
  while(Q!=NULL){
    lenRep++;
    assert(Q->tmpHeap == lists.toHead);
    assert(HM_getLevelHead(Q) == targetHH);
    Q = Q->nextChunk;
  }
  assert(lenRep+lenFree == lenOrig);
  printf("%s %d \n", "Chunks Collected = ", lenFree);

  printf("%s", "collected chunks: ");
  for(HM_chunk chunk = origList->firstChunk; chunk!=NULL; chunk = chunk->nextChunk){
    printf("%p", chunk);
    assert(chunk->tmpHeap == lists.fromHead);
    assert(HM_getLevelHead(chunk) == targetHH);
  }
  printf("\n");
  // safety code
  int countStopGapChunks = 0;
  int stopGapMem = 0;
  HM_chunk chunk = origList->firstChunk;
  while (chunk!=NULL)  {
    HM_chunk tChunk = chunk->nextChunk;
    assert(chunk->tmpHeap == lists.fromHead);
    if(chunk->startGap != 0) {
      saveChunk(chunk, &lists);
      countStopGapChunks++;
      stopGapMem+=(HM_getChunkSize(chunk));
    }
    chunk=tChunk;
  }
#endif

  // V1: is it required to scan through the entire list and fetch this information?
  // uint64_t bytesSaved =  HM_getChunkListUsedSize(cgc_process->lists->repList);
  // uint64_t bytesScanned =  HM_getChunkListUsedSize(cgc_process->lists->repList)
  //                         + HM_getChunkListUsedSize(cgc_process->lists->origList);
  uint64_t bytesSaved = HM_getChunkListUsedSizeWithFilter(cgc_process->lists->origList, TRUE);
  uint64_t bytesScanned = HM_getChunkListUsedSize(cgc_process->lists->origList);

  cp->bytesSurvivedLastCollection = bytesSaved;
  cp->bytesAllocatedSinceLastCollection = 0;

  // struct HM_chunkList _deleteList;
  // HM_chunkList deleteList = &(_deleteList);
  // HM_initChunkList(deleteList);

  // HM_chunk chunk = HM_getChunkListFirstChunk(cgc_process->lists->origList);
  // while (chunk!=NULL) {
  //   HM_chunk tChunk = chunk->nextChunk;
  //   chunk->levelHead = NULL;
  //   chunk->tmpHeap  = NULL;
  //   if(HM_getChunkSize(chunk) > 2 * (HM_BLOCK_SIZE)) {
  //     HM_unlinkChunk(cgc_process->lists->origList, chunk);
  //     HM_appendChunk(deleteList, chunk);
  //   }
  //   chunk = tChunk;
  // }

  uint32_t finalDepth = HM_HH_getDepth(targetHH);

  struct CC_chunkInfo info =
    {.initialDepth = cgc_process->initialDepth,
     .finalDepth = finalDepth,
     .procNum = s->procNumber,
     .freedType = CC_FREED_NORMAL_CHUNK};
  struct writeFreedBlockInfoFnClosure infoc =
    {.fun = CC_writeFreeChunkInfo, .env = &info};

  /** SAM_NOTE: TODO: deleteList no longer needed, because
    * block allocator handles that.
    */

  // move non-live objects off the origlist to a replist, and then free one of the lists, similar to how it was done before
  HM_freeNonLiveChunksInListWithInfo(s, cgc_process->lists->origList, &infoc, BLOCK_FOR_HEAP_CHUNK);
  // HM_freeChunksInListWithInfo(s, deleteList, &infoc, BLOCK_FOR_HEAP_CHUNK);

  // HM_appendChunkList(repList, origList);
  // *(cgc_process->lists->origList) = *(cgc_process->lists->repList);

  // V1: what do we do about this section of code? because there is some unlinking that is happening from the origList
  // also no more freeing after this at all, so why do this in the first place?
  HM_chunk stackChunk = HM_getChunkOf(objptrToPointer(cp->stack, NULL));
  assert(!(stackChunk->mightContainMultipleObjects));
  assert(HM_HH_getChunkList(HM_getLevelHead(stackChunk)) == cgc_process->lists->origList);
  assert(isChunkInList(stackChunk, cgc_process->lists->origList));
  assert(bytesSaved >= HM_getChunkUsedSize(stackChunk));
  bytesSaved -= HM_getChunkUsedSize(stackChunk);
  HM_unlinkChunk(cgc_process->lists->origList, stackChunk);
  info.freedType = CC_FREED_STACK_CHUNK;
  HM_freeChunkWithInfo(s, stackChunk, &infoc, BLOCK_FOR_HEAP_CHUNK);
  info.freedType = CC_FREED_NORMAL_CHUNK;
  cp->stack = BOGUS_OBJPTR;

  if (cp->additionalStack != BOGUS_OBJPTR) {
    HM_chunk aStackChunk =
      HM_getChunkOf(objptrToPointer(cp->additionalStack, NULL));
    assert(!(aStackChunk->mightContainMultipleObjects));
    assert(HM_HH_getChunkList(HM_getLevelHead(aStackChunk)) == cgc_process->lists->origList);
    assert(isChunkInList(aStackChunk, cgc_process->lists->origList));
    HM_unlinkChunk(cgc_process->lists->origList, aStackChunk);
    info.freedType = CC_FREED_STACK_CHUNK;
    HM_freeChunkWithInfo(s, aStackChunk, &infoc, BLOCK_FOR_HEAP_CHUNK);
    info.freedType = CC_FREED_NORMAL_CHUNK;
    cp->additionalStack = BOGUS_OBJPTR;
  }

// #if ASSERT
//   struct HM_foreachDownptrClosure checkRemEntryClosure =
//     {.fun = checkRemEntry, .env = &lists};
//   HM_foreachRemembered(s, HM_HH_getRemSet(targetHH), &checkRemEntryClosure);
// #endif

  HH_EBR_leaveQuiescentState(s);

  /** This is safe (no race with any zips), because `targetHH` is split off
    * from the main spine of the program.
    */
  HM_HH_freeAllDependants(s, targetHH, TRUE);

  HM_assertChunkListInvariants(cgc_process->lists->origList);

  timespec_now(&cgc_process->stopTime);
  timespec_sub(&cgc_process->stopTime, &cgc_process->startTime);
  timespec_add(&(s->cumulativeStatistics->timeCC), &cgc_process->stopTime);
  s->cumulativeStatistics->numCCs++; // V1: use as second field in id
  assert(bytesScanned >= bytesSaved);
  uintmax_t bytesReclaimed = bytesScanned-bytesSaved;
  s->cumulativeStatistics->bytesInScopeForCC += bytesScanned;
  s->cumulativeStatistics->bytesReclaimedByCC += bytesReclaimed;

  if (outputBytesSaved != NULL) {
    *outputBytesSaved = lists->bytesSaved;
  }

  if (outputNumObjectsMarked != NULL) {
    *outputNumObjectsMarked = lists->numObjectsMarked;
  }

  Trace0(EVENT_CGC_LEAVE);
}

bool isSplittable(CGC_process *cgc_process) {
  if(HM_getNumberOfChunksInChunkList(&cgc_process->lists->worklist.storage) > SPLIT_SIZE) {
    return TRUE;
  }
  return FALSE;
}

CGC_process* splitWork(CGC_process *cgc_process) {
  CGC_process* newProcess = malloc(sizeof(CGC_process));
  ConcurrentCollectArgs* newLists = malloc(sizeof(ConcurrentCollectArgs));
  if(newLists) {
    newLists->origList = cgc_process->lists->origList; // new origList should point back to the old origList
    newLists->bytesSaved = cgc_process->lists->bytesSaved;
    newLists->numObjectsMarked = cgc_process->lists->numObjectsMarked;
    newLists->parentHeapId = cgc_process->lists->parentHeapId;
  }
  else {
    DIE("lists malloc failed\n");
  }
  newLists->worklist = *HM_splitChunkList(&cgc_process->lists->worklist);
  newProcess->lists = newLists;

  return newProcess;
}

// test
void runCGC(GC_state s, CGC_process* cgc_process) {
  while(true) {
    if(isDone(s, cgc_process)) {
      return;
    }
    if(isSplittable(cgc_process)) {
      CGC_process* forked_cgc_process = splitWork(cgc_process);
      runCGC(s, cgc_process);
      runCGC(s, forked_cgc_process);
      return;
    }
    markLoop(s, cgc_process->lists);
  }
}

// refactoring this function
void CC_collectWithRoots(
  GC_state s,
  HM_HierarchicalHeap targetHH,
  __attribute__((unused)) GC_thread thread,
  size_t *outputBytesSaved,
  size_t *outputNumObjectsMarked)
{

  printf("start initializeCGC\n");
  CGC_process* cgc_process = initializeCGC(
    s, targetHH, thread);
  printf("end initializeCGC\n");

  printf("start runCGC\n");
  runCGC(s, cgc_process);
  printf("end runCGC\n");

  printf("start finalizeCC\n");
  finalizeCC(s, targetHH, thread, outputBytesSaved, outputNumObjectsMarked, cgc_process);
  printf("end finalizeCC\n");
}

#endif
