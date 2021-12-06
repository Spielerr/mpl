#include "concurrent-stack.h"

#include <pthread.h>

#if (defined (MLTON_GC_INTERNAL_FUNCS))

#define MAX(A,B) (((A) > (B)) ? (A) : (B))

// static const size_t MINIMUM_CAPACITY = 10;

void CC_stack_init(GC_state s, CC_stack* stack) {

  // very important that this is equal to number of processors
  stack->numStacks = (size_t)s->numberOfProcs;
  // capacity = MAX(capacity, MINIMUM_CAPACITY);

  stack->stacks = malloc(sizeof(struct CC_stack_data) * stack->numStacks);
  for (size_t i = 0; i < stack->numStacks; i++) {
    // stack->stacks[i].size = 0;
    // stack->stacks[i].capacity = capacity;
    // stack->stacks[i].storage = NULL;
    HM_initChunkList(&(stack->stacks[i].storage));
    stack->stacks[i].isClosed = FALSE;
    pthread_mutex_init(&(stack->stacks[i].mutex), NULL);
  }
}

#if 0
// assumes that the mutex is held
bool increaseCapacity(CC_stack_data* stack, int factor){
    assert(stack->storage!=NULL);
    void** new_store = (void**) realloc(stack->storage,
                                    sizeof(void*) * (factor*stack->capacity));

    if(new_store!=NULL){
        stack->storage = new_store;
        stack->capacity*=factor;
        return TRUE;
    }
    else {
        return FALSE;
    }
}
#endif

// return false if the push failed. true if push succeeds
bool CC_stack_data_push(CC_stack_data* stack, void* datum){
    if (stack->isClosed) {
        return FALSE;
    }

    pthread_mutex_lock(&stack->mutex);

#if 0
    if (NULL == stack->storage) {
        stack->storage = malloc(sizeof(void*) * stack->capacity);
    }

    if (stack->size == stack->capacity){
        bool capIncrease = increaseCapacity(stack, 2);
        if(!capIncrease){
            DIE("Ran out of space for CC_stack!\n");
            assert(0);
        }
    }

    stack->storage[stack->size++] = datum;
#endif

    HM_chunkList storage = &(stack->storage);

    HM_chunk chunk = HM_getChunkListLastChunk(storage);
    if (NULL == chunk
        || HM_getChunkSizePastFrontier(chunk) < sizeof(void*))
    {
      chunk = HM_allocateChunk(storage, sizeof(void*));
    }

    assert(NULL != chunk);
    assert(HM_getChunkSizePastFrontier(chunk) >= sizeof(void*));
    pointer frontier = HM_getChunkFrontier(chunk);

    HM_updateChunkFrontierInList(
      storage,
      chunk,
      frontier + sizeof(void*));

    void** r = (void**)frontier;
    *r = datum;

    pthread_mutex_unlock(&stack->mutex);
    return TRUE;
}

bool CC_stack_push(GC_state s, CC_stack* stack, void* datum) {
  return CC_stack_data_push(&(stack->stacks[s->procNumber]), datum);
}

#if 0
// two threads can't pop the same stack
void* CC_stack_data_pop(CC_stack_data* stack){
    void* ret;
    pthread_mutex_lock(&stack->mutex);

    if (stack->size == 0){
        LOG(LM_HH_COLLECTION, LL_FORCE, "assertion that two different threads don't pop failed");
        assert(0);
    }
    ret = stack->storage[stack->size - 1];
    stack->size--;

    pthread_mutex_unlock(&stack->mutex);

    return ret;
}

void* CC_stack_data_top(CC_stack_data* stack){
    void* ret;
    pthread_mutex_lock(&stack->mutex);

    if (stack->size == 0){
        LOG(LM_HH_COLLECTION, LL_FORCE, "top query demands a size query");
        assert(0);
    }

    ret = stack->storage[stack->size - 1];

    // pthread_cond_signal(&stack->full_condition_variable);
    pthread_mutex_unlock(&stack->mutex);

    return ret;
}

size_t CC_stack_data_size(CC_stack_data* stack){
    size_t size;
    // Not sure if we need mutex here
    pthread_mutex_lock(&stack->mutex);
    size = stack->size;
    pthread_mutex_unlock(&stack->mutex);

    return size;
}

size_t CC_stack_data_capacity(CC_stack_data* stack){
    return stack->capacity;
}


void CC_stack_data_free(CC_stack_data* stack){
    if (NULL != stack->storage) {
      free(stack->storage);
      stack->storage = NULL;
    }
    pthread_mutex_destroy(&stack->mutex);
}
#endif

void CC_stack_data_free(GC_state s, CC_stack_data* stack) {
  HM_freeChunksInList(s, &(stack->storage));
}

void CC_stack_free(GC_state s, CC_stack* stack) {
  for (size_t i = 0; i < stack->numStacks; i++) {
    CC_stack_data_free(s, &(stack->stacks[i]));
  }
  free(stack->stacks);
  stack->stacks = NULL;
}

void CC_stack_data_clear(GC_state s, CC_stack_data* stack){
    pthread_mutex_lock(&stack->mutex);
    HM_freeChunksInList(s, &(stack->storage));
    pthread_mutex_unlock(&stack->mutex);
}


void CC_stack_clear(GC_state s, CC_stack* stack) {
  for (size_t i = 0; i < stack->numStacks; i++) {
    CC_stack_data_clear(s, &(stack->stacks[i]));
  }
}


void CC_stack_close(CC_stack* stack) {
  for (size_t i = 0; i < stack->numStacks; i++) {
    pthread_mutex_lock(&(stack->stacks[i].mutex));
    stack->stacks[i].isClosed = TRUE;
    pthread_mutex_unlock(&(stack->stacks[i].mutex));
  }
}


#if 0
void forEachObjptrInStackData(
  GC_state s,
  CC_stack_data* stack,
  GC_foreachObjptrFun f,
  void* rawArgs)
{
    if (stack == NULL || stack->storage == NULL)
      return;
    // size can only increase while iterating.
    // CC_stack_size function requires mutex, therefore it is more efficient to have two loops
    // the inner loop does not query size and once it's done, we can re-check size
    // There should be a mutex here. It is present in CC_stack_size, so its okay for now. (not good though)

    // temporary assurance changes ==> haven't proved to be useful so far.
    pthread_mutex_lock(&stack->mutex);
    size_t i = 0, size = stack->size;
    // size_t i = 0, size = CC_stack_size(stack);
    struct GC_foreachObjptrClosure fObjptrClosure =
    {.fun = f, .env = rawArgs};

    while(i!=size){

        while(i!=size){
            objptr* opp = (objptr*)(&(stack->storage[i]));
            callIfIsObjptr(s, &fObjptrClosure, opp);
            i++;
        }

        // unsure about this code too.
        // #if ASSERT
        //     size_t size_new = CC_stack_size(stack);
        //     assert(size_new>=size);
        //     size = size_new;
        // #else
        //     size = CC_stack_size(stack);
        // #endif
    }
    pthread_mutex_unlock(&stack->mutex);
}
#endif


void forEachObjptrInStackData(
  GC_state s,
  CC_stack_data* stack,
  GC_foreachObjptrFun f,
  void* rawArgs)
{
  if (stack == NULL)
    return;

  struct GC_foreachObjptrClosure fObjptrClosure =
  {.fun = f, .env = rawArgs};

  pthread_mutex_lock(&stack->mutex);

  HM_chunkList storage = &(stack->storage);
  HM_chunk chunk = HM_getChunkListFirstChunk(storage);
  while (chunk != NULL) {
    pointer p = HM_getChunkStart(chunk);
    pointer frontier = HM_getChunkFrontier(chunk);
    while (p < frontier) {
      // objptr* opp = (objptr*)p;
      callIfIsObjptr(s, &fObjptrClosure, (objptr*)p);
      p += sizeof(void*);
    }
    chunk = chunk->nextChunk;
  }

  pthread_mutex_unlock(&stack->mutex);
}


void forEachObjptrinStack(
  GC_state s,
  CC_stack* stack,
  GC_foreachObjptrFun f,
  void* rawArgs)
{
  for (size_t i = 0; i < stack->numStacks; i++) {
    forEachObjptrInStackData(s, &(stack->stacks[i]), f, rawArgs);
  }
}

#endif

