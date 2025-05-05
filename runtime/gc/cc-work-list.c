#include "cc-work-list.h"

#if (defined (MLTON_GC_INTERNAL_FUNCS))


void CC_workList_init(
  __attribute__((unused)) GC_state s,
  CC_workList w)
{
  HM_chunkList c = &(w->storage);
  HM_initChunkList(c);
  // arbitrary, just need an initial chunk
  w->currentChunk = HM_allocateChunkWithPurpose(
    c,
    sizeof(struct CC_workList_elem),
    BLOCK_FOR_GC_WORKLIST);
}

void CC_workList_free(
    __attribute__((unused)) GC_state s,
    CC_workList w)
{
  HM_chunkList c = &(w->storage);
  HM_freeChunksInListWithInfo(s, c, NULL, BLOCK_FOR_GC_WORKLIST);
  w->currentChunk = NULL;
}

bool CC_workList_isEmpty(
  __attribute__((unused)) GC_state s,
  CC_workList w)
{
  HM_chunkList list = &(w->storage);
  HM_chunk curr = w->currentChunk;

  return
    (list->firstChunk == curr)
    &&
    ((list->lastChunk == curr) || (list->lastChunk == curr->nextChunk))
    &&
    (HM_getChunkFrontier(curr) == HM_getChunkStart(curr))
    &&
    ( curr->nextChunk == NULL
      || HM_getChunkFrontier(curr->nextChunk) == HM_getChunkStart(curr->nextChunk)
    );
}


pointer findStackNonEmptyFrameTop(GC_state s, pointer bottom, pointer top) {
  assert(bottom <= top);

  while (top > bottom) {
    /* Invariant: top points just past a "return address". */
    GC_returnAddress returnAddress =
      *((GC_returnAddress*)(top - GC_RETURNADDRESS_SIZE));
    GC_frameInfo frameInfo = getFrameInfoFromReturnAddress(s, returnAddress);
    // index zero of this array is size
    GC_frameOffsets frameOffsets = frameInfo->offsets;

    if (frameOffsets[0] > 0) {
      return top;
    }

    top -= frameInfo->size;
  }

  return NULL;
}


// returns FALSE if object has no objptrs and doesn't need to be traced
bool makeInitialElem(GC_state s, objptr op, CC_workList_elem result) {
  GC_header header;
  uint16_t numObjptrs;
  GC_objectTypeTag tag;
  header = getHeader(objptrToPointer(op, NULL));
  splitHeader(s, header, &tag, NULL, NULL, &numObjptrs);

  if (NORMAL_TAG == tag) {
    if (0 == numObjptrs) return FALSE;

    result->op = op;
    result->data.normal.objptrIdx = 0;
    return TRUE;
  }

  if (SEQUENCE_TAG == tag) {
    if (0 == numObjptrs) return FALSE;
    if (0 == getSequenceLength(objptrToPointer(op, NULL))) return FALSE;

    result->op = op;
    result->data.sequence.cellIdx = 0;
    result->data.sequence.objptrIdx = 0;
    return TRUE;
  }

  if (STACK_TAG == tag) {
    // printf("makeInitialElem stack\n");

    GC_stack stack = (GC_stack)objptrToPointer(op, NULL);
    assert (stack->used <= stack->reserved);
    pointer bottom = getStackBottom(s, stack);
    pointer top = getStackTop(s, stack);
    pointer firstTop = findStackNonEmptyFrameTop(s, bottom, top);

    if (NULL == firstTop) {
      // printf("makeInitialElem: skipping stack "FMTOBJPTR"\n", op);
      return FALSE;
    }

    assert(firstTop > bottom);

    result->op = op;
    result->data.stack.topCursor = firstTop;
    result->data.stack.frameOffsetsIdx = 0;

    // printf("makeInitialElem: push stack "FMTOBJPTR"\n", op);
    return TRUE;
  }

  DIE("makeInitialElem: cannot handle tag %u", tag);
  return FALSE;
}


void checkWorkList(CC_workList workList) {
  assert(workList != NULL);
  assert(workList->currentChunk != NULL);
  HM_assertChunkListInvariants2(&workList->storage);
  if (workList->currentChunk->nextChunk != NULL) {
    assert(HM_getChunkFrontier(workList->currentChunk->nextChunk) == HM_getChunkStart(workList->currentChunk->nextChunk));
    assert(workList->currentChunk->nextChunk->nextChunk == NULL);
  }
}

void CC_workList_push(
  GC_state s,
  CC_workList w,
  objptr op)
{
  struct CC_workList_elem elem;
  if (!makeInitialElem(s, op, &elem))
    return;

  HM_chunkList list = &(w->storage);
  HM_chunk chunk = w->currentChunk;
  size_t elemSize = sizeof(struct CC_workList_elem);

  if (HM_getChunkSizePastFrontier(chunk) < elemSize) {
    if (chunk->nextChunk != NULL) {
      chunk = chunk->nextChunk; // this will be an empty chunk
    } else {
      // chunk = HM_allocateChunk(list, elemSize);
      chunk = HM_allocateChunkWithPurpose(
        list,
        elemSize,
        BLOCK_FOR_GC_WORKLIST);
    }
    w->currentChunk = chunk;
  }

  assert(NULL != chunk);
  assert(HM_getChunkSizePastFrontier(chunk) >= elemSize);
  assert(chunk == w->currentChunk);

  pointer frontier = HM_getChunkFrontier(chunk);
  HM_updateChunkFrontierInList(
    list,
    chunk,
    frontier + elemSize);

  *(CC_workList_elem)frontier = elem;
  return;
}

typedef struct {
  size_t totalNumObjPtrs;
  size_t numWorklistElems;
  size_t worklistElemObjPtrs[]; // Flexible array member
} WorklistChunkSize;

WorklistChunkSize* createWorklistChunk(size_t totalObjPtrs, size_t numElems) {
  // Allocate memory: size of struct + size of flexible array
  WorklistChunkSize* chunk = malloc(sizeof(WorklistChunkSize) + numElems * sizeof(size_t));
  if (!chunk) {
    DIE("Failed to allocate memory");
  }

  chunk->totalNumObjPtrs = totalObjPtrs;
  chunk->numWorklistElems = numElems;

  return chunk;
}

void populateWorklistChunk(WorklistChunkSize* chunk, size_t values[]) {
  for (size_t i = 0; i < chunk->numWorklistElems; i++) {
    chunk->worklistElemObjPtrs[i] = values[i];
  }
}

#define INITIAL_LIST_CAPACITY 4  // Initial capacity of list

typedef struct {
  WorklistChunkSize** chunks; // Array of WorklistChunkSize pointers
  size_t size;                   // Current number of elements
  size_t capacity;               // Max capacity before reallocation
} WorklistChunkList;

WorklistChunkList* createChunkList() {
  WorklistChunkList* list = malloc(sizeof(WorklistChunkList));
  list->size = 0;
  list->capacity = INITIAL_LIST_CAPACITY;
  list->chunks = malloc(list->capacity * sizeof(WorklistChunkSize*));
  return list;
}

void addChunkToList(WorklistChunkList* list, WorklistChunkSize* chunk) {
  if (list->size >= list->capacity) {
    list->capacity *= 2; // Double capacity
    list->chunks = realloc(list->chunks, list->capacity * sizeof(WorklistChunkSize*));
    if (!list->chunks) {
      DIE("Failed to reallocate memory");
    }
  }
  list->chunks[list->size++] = chunk;
}

/*
 * splitWorkList splits the worklist at the worklist_elem level - i.e., within a chunk between two worklist elems
 * first step - iterate through the worklist to count # of chunks and # of worklist elems within each chunk and # of obj ptrs within each worklist elem
 * second step - use this information to go the middle of the worklist and then split into two worklists
 *
 *
 */
CC_workList HM_splitWorkList(GC_state s, CC_workList workList) {
  checkWorkList(workList);
  HM_chunkList list = &workList->storage;
  size_t originalSize = list->size;
  size_t originalUsedSize = list->usedSize;
  HM_chunk lastChunk = list->lastChunk;
  HM_chunk worklistCurrentChunk = workList->currentChunk;

  size_t firstHalfSize = 0;
  size_t firstHalfUsedSize = 0;

  HM_chunk currentChunk = HM_getChunkListFirstChunk(list);
  size_t totalObjPtrs = 0;
  size_t totalWorklistElems = 0;
  WorklistChunkList* worklist_chunk_list = createChunkList();

  // part 1 - fill in the required metadata to find the size characteristics of the worklist
  while (currentChunk != worklistCurrentChunk->nextChunk) {
    printf("new chunk\n");
    pointer chunkFrontier = HM_getChunkFrontier(currentChunk);
    pointer chunkStart = HM_getChunkStart(currentChunk);
    pointer elemPtr = chunkFrontier - sizeof(struct CC_workList_elem);

    size_t totalNumObjPtrsBeforeChunk = totalObjPtrs;
    size_t numOfWorklistElems = (chunkFrontier - chunkStart)/sizeof(struct CC_workList_elem);
    totalWorklistElems += numOfWorklistElems;
    size_t numObjPtrsInWorklistElems[numOfWorklistElems];
    size_t i = numOfWorklistElems - 1;

    while (elemPtr >= chunkStart) {
      CC_workList_elem elem = (CC_workList_elem)elemPtr;

      pointer p = objptrToPointer(elem->op, NULL);

      // inspect the object
      GC_header header;
      uint16_t bytesNonObjptrs;
      uint16_t numObjptrs;
      GC_objectTypeTag tag;
      header = getHeader(p);
      splitHeader(s, header, &tag, NULL, &bytesNonObjptrs, &numObjptrs);

      totalObjPtrs += numObjptrs;
      numObjPtrsInWorklistElems[i--] = numObjptrs;

      elemPtr = elemPtr - sizeof(struct CC_workList_elem);
    }
    WorklistChunkSize* worklist_chunk_size =
      createWorklistChunk(totalObjPtrs - totalNumObjPtrsBeforeChunk,
        numOfWorklistElems);
    populateWorklistChunk(worklist_chunk_size, numObjPtrsInWorklistElems);
    addChunkToList(worklist_chunk_list, worklist_chunk_size);
    currentChunk = currentChunk->nextChunk;
  }

  size_t temp = 0;
  HM_chunk curr_chunk = list->firstChunk;
  pointer ptrToRemainingWorkListElems = NULL;
  size_t numOfRemainingWorkListElems = 0;
  HM_chunk nextChunkOfNewWorkList = NULL;

  // step 2 - find the point in the worklist where the split needs to happen and update a few things
  bool foundBreakpoint = false;
  for (size_t i = 0; i < worklist_chunk_list->size; i++) {
    WorklistChunkSize* worklist_chunk_size = worklist_chunk_list->chunks[i];
    if (temp + worklist_chunk_size -> totalNumObjPtrs > totalObjPtrs/2) {
      size_t countOfWorkListElemObjPtrs = 0;

      for (size_t j = 0; j < worklist_chunk_size->numWorklistElems; j++) {
        countOfWorkListElemObjPtrs += worklist_chunk_size->worklistElemObjPtrs[j];
        if (countOfWorkListElemObjPtrs + temp > totalObjPtrs/2 || j == worklist_chunk_size->numWorklistElems - 2) {

          if (j == 0 && worklist_chunk_size->numWorklistElems == 1) {
            curr_chunk->prevChunk->nextChunk = NULL;
            list->lastChunk = curr_chunk->prevChunk;
            list->size = firstHalfSize;
            list->usedSize = firstHalfUsedSize;
            workList->currentChunk = curr_chunk->prevChunk;

            nextChunkOfNewWorkList = curr_chunk;
            ptrToRemainingWorkListElems = NULL;
            numOfRemainingWorkListElems = 0;
            foundBreakpoint = true;
            break;
          }

          curr_chunk->frontier -= (worklist_chunk_size->numWorklistElems - j - 1) * sizeof(struct CC_workList_elem);
          curr_chunk->nextChunk = NULL;
          list->lastChunk = curr_chunk;
          firstHalfSize += HM_getChunkSize(curr_chunk);
          firstHalfUsedSize += HM_getChunkUsedSize(curr_chunk) - (worklist_chunk_size->numWorklistElems - j - 1) * sizeof(struct CC_workList_elem);
          list->size = firstHalfSize;
          list->usedSize = firstHalfUsedSize;
          workList->currentChunk = curr_chunk;

          nextChunkOfNewWorkList = curr_chunk->nextChunk;
          ptrToRemainingWorkListElems = curr_chunk->frontier;
          numOfRemainingWorkListElems = worklist_chunk_size->numWorklistElems - j - 1;
          foundBreakpoint = true;
          break;
        }
      }
      if (foundBreakpoint) {break;}
    }
    temp += worklist_chunk_size -> totalNumObjPtrs;
    firstHalfSize += HM_getChunkSize(curr_chunk);
    firstHalfUsedSize += HM_getChunkUsedSize(curr_chunk);
    curr_chunk = curr_chunk->nextChunk;
  }

  // step 3 - do the final split after finding the position to split at
  // let's make a new chunk to add - guaranteed to have at least one worklist elem
  // need a new chunklist for the new worklist
  HM_chunkList newChunkList = malloc((sizeof(struct HM_chunkList)));
  bool rhsEmpty = false;
  if (nextChunkOfNewWorkList != worklistCurrentChunk->nextChunk) {
    // there are chunks present on the RHS
    newChunkList->firstChunk = nextChunkOfNewWorkList;
    newChunkList->lastChunk = lastChunk;
    newChunkList->size = originalSize - firstHalfSize;
    newChunkList->usedSize = originalUsedSize - firstHalfUsedSize; // still doesn't include the size(s) of the pending worklist elem(s) to be added
    // this worklist currentchunk should be original current chunk
  }
  else {
    // no chunks on the right hand side - need to make a new chunklist and add new chunk into this
    rhsEmpty = true;
    HM_initChunkList(newChunkList);
  }

  if (numOfRemainingWorkListElems != 0) {
    size_t workListElemSize = sizeof(struct CC_workList_elem);
    HM_chunk newChunkForNewList = HM_allocateChunkWithPurposeAndAddToFront(
      newChunkList,
      workListElemSize * numOfRemainingWorkListElems,
      BLOCK_FOR_GC_WORKLIST);

    pointer currentWorkListElem = ptrToRemainingWorkListElems;
    for (size_t i = 0; i < numOfRemainingWorkListElems; i++) {
      struct CC_workList_elem elem = *((CC_workList_elem) currentWorkListElem);
      pointer frontier = HM_getChunkFrontier(newChunkForNewList);
      *(CC_workList_elem) frontier = elem;

      HM_updateChunkFrontierInList(
        newChunkList,
        newChunkForNewList, // TODO: make sure this is the new chunk you added previously
        frontier + workListElemSize); // also updates the newList sizes
      currentWorkListElem += workListElemSize;
    }
  }

  // time to make newWorkList, set the storage and the currentChunk and return it
  CC_workList newWorkList = malloc(sizeof(struct CC_workList));
  newWorkList->storage = *newChunkList;
  if (!rhsEmpty) {
    newWorkList->currentChunk = worklistCurrentChunk;
  } else {
    newWorkList->currentChunk = newChunkList->firstChunk;
  }

  checkWorkList(workList);
  checkWorkList(newWorkList);

  // invariants
  size_t rhs_wl_size = HM_getNumberOfObjPtrsInWorkList(s, newWorkList->currentChunk);
  size_t lhs_wl_size = HM_getNumberOfObjPtrsInWorkList(s, workList->currentChunk);

  size_t rhs_wlelems = HM_getNumberOfWorklistElemsInChunkList(newWorkList->currentChunk);
  size_t lhs_wlelems = HM_getNumberOfWorklistElemsInChunkList(workList->currentChunk);

  if (totalObjPtrs == rhs_wl_size + lhs_wl_size) {
    printf("SUCCESSFUL SPLIT**********************************\n\n\n");
  }

  if (totalObjPtrs != rhs_wl_size + lhs_wl_size) {
    printf("total obj ptrs before and after do not match\n");
    printf("before count: %lu\n", totalObjPtrs);
    printf("after count lhs: %lu\n\n", lhs_wl_size);
    printf("after count rhs: %lu\n\n", rhs_wl_size);

    DIE("objptr count failure");
  }

  return newWorkList;
}

struct advanceOneFieldResult {
  objptr* field;
  bool objectDone;
};

void advanceOneField(
  GC_state s,
  CC_workList_elem elem,
  struct advanceOneFieldResult * result)
{
  pointer p = objptrToPointer(elem->op, NULL);

  // inspect the object
  GC_header header;
  uint16_t bytesNonObjptrs;
  uint16_t numObjptrs;
  GC_objectTypeTag tag;
  header = getHeader(p);
  splitHeader(s, header, &tag, NULL, &bytesNonObjptrs, &numObjptrs);

  // ======================== NORMAL OBJECTS ========================

  if (NORMAL_TAG == tag) {
    uint16_t objptrIdx = elem->data.normal.objptrIdx;
    assert(objptrIdx < numObjptrs);
    result->field = (objptr*)(p + bytesNonObjptrs + (objptrIdx * OBJPTR_SIZE));
    result->objectDone = (objptrIdx+1 == numObjptrs);
    elem->data.normal.objptrIdx++;
    return;
  }

  // ======================== SEQUENCE OBJECTS ========================

  if (SEQUENCE_TAG == tag) {
    GC_sequenceLength numCells = getSequenceLength(p);
    size_t bytesPerCell = bytesNonObjptrs + (numObjptrs * OBJPTR_SIZE);

    size_t cellIdx = elem->data.sequence.cellIdx;
    uint16_t objptrIdx = elem->data.sequence.objptrIdx;
    assert(cellIdx < numCells);
    assert(objptrIdx < numObjptrs);

    result->field =
      (objptr*)(
        p                            // object start
        + (cellIdx * bytesPerCell)   // cell offset
        + bytesNonObjptrs            // objptrs offset
        + (objptrIdx * OBJPTR_SIZE)  // current objptr offset
      );

    result->objectDone = FALSE;

    elem->data.sequence.objptrIdx++;
    if (objptrIdx+1 == numObjptrs) {
      elem->data.sequence.cellIdx++;
      elem->data.sequence.objptrIdx = 0;
      if (cellIdx+1 == numCells)
        result->objectDone = TRUE;
    }
    return;
  }

  // ======================== STACK OBJECTS ========================

  if (STACK_TAG == tag) {
    GC_stack stack = (GC_stack)p;
    pointer bottom = getStackBottom(s, stack);
    pointer top = elem->data.stack.topCursor;
    unsigned int i = elem->data.stack.frameOffsetsIdx;

    // printf(
    //   "advanceOneField: stack "FMTOBJPTR" top="FMTPTR" bottom="FMTPTR" i=%u\n",
    //   (uintptr_t)elem->op,
    //   (uintptr_t)top,
    //   (uintptr_t)bottom,
    //   i
    // );

    /* Invariant: top points just past a "return address". */
    GC_returnAddress returnAddress =
      *((GC_returnAddress*)(top - GC_RETURNADDRESS_SIZE));
    GC_frameInfo frameInfo = getFrameInfoFromReturnAddress(s, returnAddress);
    // index zero of this array is size
    GC_frameOffsets frameOffsets = frameInfo->offsets;
    pointer frameStart = top - frameInfo->size;

    assert(frameOffsets[0] > 0);
    assert(i < frameOffsets[0]);
    assert(frameStart >= bottom);

    result->field = (objptr*)(frameStart + frameOffsets[i+1]);
    elem->data.stack.frameOffsetsIdx++;

    result->objectDone = FALSE;

    if (i+1 == frameOffsets[0]) {
      /** walk backwards until we find a frame that has at least one objptr
        * or, until we find the end of the stack.
        */
      pointer newTop = findStackNonEmptyFrameTop(s, bottom, frameStart);

      if (NULL == newTop) {
        result->objectDone = TRUE;
      } else {
        elem->data.stack.topCursor = newTop;
        elem->data.stack.frameOffsetsIdx = 0;
      }
    }

    return;
  }

  // ======================== OTHERWISE... ========================

  DIE("advanceOneField: cannot handle tag %u", tag);
  return;
}


objptr* CC_workList_pop(
  GC_state s,
  CC_workList w)
{
  HM_chunkList list = &(w->storage);
  HM_chunk chunk = w->currentChunk;

  if (HM_getChunkFrontier(chunk) <= HM_getChunkStart(chunk)) {
    // chunk is empty; try to move backwards

    HM_chunk prevChunk = chunk->prevChunk;
    if (prevChunk == NULL) {
      // whole worklist is empty
      return NULL;
    }

    /** Otherwise, there is a chunk before us. It's now safe (for cost
      * amortization) to delete the chunk after us, if there is one.
      */
    if (NULL != chunk->nextChunk) {
      HM_chunk nextChunk = chunk->nextChunk;
      HM_unlinkChunk(list, nextChunk);
      HM_freeChunkWithInfo(s, nextChunk, NULL, BLOCK_FOR_GC_WORKLIST);
    }

    assert(NULL == chunk->nextChunk);
    assert(prevChunk == chunk->prevChunk);

    chunk = prevChunk;
    w->currentChunk = chunk;
  }

  assert(w->currentChunk == chunk);
  assert(HM_getChunkFrontier(chunk) >= HM_getChunkStart(chunk) + sizeof(struct CC_workList_elem));

  pointer frontier = HM_getChunkFrontier(chunk);
  pointer elemPtr = frontier - sizeof(struct CC_workList_elem);
  CC_workList_elem elem = (CC_workList_elem)elemPtr;

  struct advanceOneFieldResult r;
  advanceOneField(s, elem, &r);

  if (r.objectDone) {
    pointer newFrontier = elemPtr;
    HM_updateChunkFrontierInList(
      list,
      chunk,
      newFrontier);
  }

  assert(NULL != r.field);
  return r.field;
}

#endif /* MLTON_GC_INTERNAL_FUNCS */
