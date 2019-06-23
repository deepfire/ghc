/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 2001
 * Author: Sungwoo Park
 *
 * Retainer profiling.
 *
 * ---------------------------------------------------------------------------*/

#if defined(PROFILING)

// Turn off inlining when debugging - it obfuscates things
#if defined(DEBUG)
#define INLINE
#else
#define INLINE inline
#endif

#include "PosixSource.h"
#include "Rts.h"

#include "RtsUtils.h"
#include "RetainerProfile.h"
#include "RetainerSet.h"
#include "Schedule.h"
#include "Printer.h"
#include "Weak.h"
#include "sm/Sanity.h"
#include "Profiling.h"
#include "Stats.h"
#include "ProfHeap.h"
#include "Apply.h"
#include "StablePtr.h" /* markStablePtrTable */
#include "StableName.h" /* rememberOldStableNameAddresses */
#include "sm/Storage.h" // for END_OF_STATIC_LIST

/* Note [What is a retainer?]
   ~~~~~~~~~~~~~~~~~~~~~~~~~~
Retainer profiling is a profiling technique that gives information why
objects can't be freed and lists the consumers that hold pointers to
the heap objects. It does not list all the objects that keep references
to the other, because then we would keep too much information that will
make the report unusable, for example the cons element of the list would keep
all the tail cells. As a result we are keeping only the objects of the
certain types, see 'isRetainer()' function for more discussion.

More formal definition of the retainer can be given the following way.

An object p is a retainer object of the object l, if all requirements
hold:

  1. p can be a retainer (see `isRetainer()`)
  2. l is reachable from p
  3. There are no other retainers on the path from p to l.

Exact algorithm and additional information can be found the historical
document 'docs/storage-mgt/rp.tex'. Details that are related to the
RTS implementation may be out of date, but the general
information about the retainers is still applicable.
*/


/*
  Note: what to change in order to plug-in a new retainer profiling scheme?
    (1) type retainer in ../includes/StgRetainerProf.h
    (2) retainer function R(), i.e., getRetainerFrom()
    (3) the two hashing functions, hashKeySingleton() and hashKeyAddElement(),
        in RetainerSet.h, if needed.
    (4) printRetainer() and printRetainerSetShort() in RetainerSet.c.
 */

// TODO: Change references to c_child_r in comments to 'data'.

/* -----------------------------------------------------------------------------
 * Declarations...
 * -------------------------------------------------------------------------- */

static uint32_t retainerGeneration;  // generation

static uint32_t numObjectVisited;    // total number of objects visited
static uint32_t timesAnyObjectVisited;  // number of times any objects are
                                        // visited

/** Note [Profiling heap traversal visited bit]
 *
 * If the RTS is compiled with profiling enabled StgProfHeader can be used by
 * profiling code to store per-heap object information.
 *
 * When using the generic heap traversal code we use this field to store
 * profiler specific information. However we reserve the LSB of the *entire*
 * 'trav' union (which will overlap with the other fields) for the generic
 * traversal code. We use the bit to decide whether we've already visited this
 * closure in this pass or not. We do this as the heap may contain cyclic
 * references, it being a graph and all, so we would likely just infinite loop
 * if we didn't.
 *
 * We assume that at least the LSB of the largest field in the corresponding
 * union is insignificant. This is true at least for the word aligned pointers
 * which the retainer profiler currently stores there and should be maintained
 * by new users of the 'trav' union.
 *
 * Now the way the traversal works is that the interpretation of the "visited?"
 * bit depends on the value of the global 'flip' variable. We don't want to have
 * to do another pass over the heap just to reset the bit to zero so instead on
 * each traversal (i.e. each run of the profiling code) we invert the value of
 * the global 'flip' variable. We interpret this as resetting all the "visited?"
 * flags on the heap.
 *
 * There is one exception to this rule, namely: static objects. There we do just
 * go over the heap and reset the bit manually. See
 * 'resetStaticObjectForProfiling'.
 */
StgWord flip = 0;     // flip bit
                      // must be 0 if DEBUG_RETAINER is on (for static closures)

#define setTravDataToZero(c) \
  (c)->header.prof.hp.trav.lsb = flip

/* -----------------------------------------------------------------------------
 * Retainer stack - header
 *   Note:
 *     Although the retainer stack implementation could be separated *
 *     from the retainer profiling engine, there does not seem to be
 *     any advantage in doing that; retainer stack is an integral part
 *     of retainer profiling engine and cannot be use elsewhere at
 *     all.
 * -------------------------------------------------------------------------- */

typedef enum {
    // Object with fixed layout. Keeps an information about that
    // element was processed. (stackPos.next.step)
    posTypeStep,
    // Description of the pointers-first heap object. Keeps information
    // about layout. (stackPos.next.ptrs)
    posTypePtrs,
    // Keeps SRT bitmap (stackPos.next.srt)
    posTypeSRT,
    // Keeps a new object that was not inspected yet. Keeps a parent
    // element (stackPos.next.parent)
    posTypeFresh
} nextPosType;

typedef union {
    // fixed layout or layout specified by a field in the closure
    StgWord step;

    // layout.payload
    struct {
        // See StgClosureInfo in InfoTables.h
        StgHalfWord pos;
        StgHalfWord ptrs;
        StgPtr payload;
    } ptrs;

    // SRT
    struct {
        StgClosure *srt;
    } srt;
} nextPos;

// Tagged stack element, that keeps information how to process
// the next element in the traverse stack.
typedef struct {
    nextPosType type;
    nextPos next;
} stackPos;

typedef union {
     /**
      * Most recent retainer for the corresponding closure on the stack.
      */
    retainer c_child_r;
} stackData;

// Element in the traverse stack, keeps the element, information
// how to continue processing the element, and it's retainer set.
typedef struct {
    stackPos info;
    StgClosure *c;
    StgClosure *cp; // parent of 'c'
    stackData data;
} stackElement;

typedef struct {
/*
  Invariants:
    firstStack points to the first block group.
    currentStack points to the block group currently being used.
    currentStack->free == stackLimit.
    stackTop points to the topmost byte in the stack of currentStack.
    Unless the whole stack is empty, stackTop must point to the topmost
    object (or byte) in the whole stack. Thus, it is only when the whole stack
    is empty that stackTop == stackLimit (not during the execution of push()
    and pop()).
    stackBottom == currentStack->start.
    stackLimit == currentStack->start + BLOCK_SIZE_W * currentStack->blocks.
  Note:
    When a current stack becomes empty, stackTop is set to point to
    the topmost element on the previous block group so as to satisfy
    the invariants described above.
 */
    bdescr *firstStack;
    bdescr *currentStack;
    stackElement *stackBottom, *stackTop, *stackLimit;

/*
  currentStackBoundary is used to mark the current stack chunk.
  If stackTop == currentStackBoundary, it means that the current stack chunk
  is empty. It is the responsibility of the user to keep currentStackBoundary
  valid all the time if it is to be employed.
 */
    stackElement *currentStackBoundary;

#if defined(DEBUG_RETAINER)
/*
  stackSize records the current size of the stack.
  maxStackSize records its high water mark.
  Invariants:
    stackSize <= maxStackSize
  Note:
    stackSize is just an estimate measure of the depth of the graph. The reason
    is that some heap objects have only a single child and may not result
    in a new element being pushed onto the stack. Therefore, at the end of
    retainer profiling, maxStackSize is some value no greater
    than the actual depth of the graph.
 */
    int stackSize, maxStackSize;
#endif
} traverseState;

/* Callback called when heap traversal visits a closure.
 *
 * Before this callback is called the profiling header of the visited closure
 * 'c' is zero'd with 'setTravDataToZero' if this closure hasn't been visited in
 * this run yet. See Note [Profiling heap traversal visited bit].
 *
 * Return 'true' when this is not the first visit to this element. The generic
 * traversal code will then skip traversing the children.
 */
typedef bool (*visitClosure_cb) (
    const StgClosure *c,
    const StgClosure *cp,
    const stackData data,
    stackData *child_data);

traverseState g_retainerTraverseState;


static void traverseStack(traverseState *, StgClosure *, stackData, StgPtr, StgPtr);
static void traverseClosure(traverseState *, StgClosure *, StgClosure *, retainer);
static void traversePushClosure(traverseState *, StgClosure *, StgClosure *, stackData);


// number of blocks allocated for one stack
#define BLOCKS_IN_STACK 1

/* -----------------------------------------------------------------------------
 * Add a new block group to the stack.
 * Invariants:
 *  currentStack->link == s.
 * -------------------------------------------------------------------------- */
static INLINE void
newStackBlock( traverseState *ts, bdescr *bd )
{
    ts->currentStack = bd;
    ts->stackTop     = (stackElement *)(bd->start + BLOCK_SIZE_W * bd->blocks);
    ts->stackBottom  = (stackElement *)bd->start;
    ts->stackLimit   = (stackElement *)ts->stackTop;
    bd->free     = (StgPtr)ts->stackLimit;
}

/* -----------------------------------------------------------------------------
 * Return to the previous block group.
 * Invariants:
 *   s->link == currentStack.
 * -------------------------------------------------------------------------- */
static INLINE void
returnToOldStack( traverseState *ts, bdescr *bd )
{
    ts->currentStack = bd;
    ts->stackTop = (stackElement *)bd->free;
    ts->stackBottom = (stackElement *)bd->start;
    ts->stackLimit = (stackElement *)(bd->start + BLOCK_SIZE_W * bd->blocks);
    bd->free = (StgPtr)ts->stackLimit;
}

/* -----------------------------------------------------------------------------
 *  Initializes the traverse stack.
 * -------------------------------------------------------------------------- */
static void
initializeTraverseStack( traverseState *ts )
{
    if (ts->firstStack != NULL) {
        freeChain(ts->firstStack);
    }

    ts->firstStack = allocGroup(BLOCKS_IN_STACK);
    ts->firstStack->link = NULL;
    ts->firstStack->u.back = NULL;

    newStackBlock(ts, ts->firstStack);
}

/* -----------------------------------------------------------------------------
 * Frees all the block groups in the traverse stack.
 * Invariants:
 *   firstStack != NULL
 * -------------------------------------------------------------------------- */
static void
closeTraverseStack( traverseState *ts )
{
    freeChain(ts->firstStack);
    ts->firstStack = NULL;
}

/* -----------------------------------------------------------------------------
 * Returns true if the whole stack is empty.
 * -------------------------------------------------------------------------- */
static INLINE bool
isEmptyWorkStack( traverseState *ts )
{
    return (ts->firstStack == ts->currentStack) && ts->stackTop == ts->stackLimit;
}

/* -----------------------------------------------------------------------------
 * Returns size of stack
 * -------------------------------------------------------------------------- */
W_
traverseWorkStackBlocks(traverseState *ts)
{
    bdescr* bd;
    W_ res = 0;

    for (bd = ts->firstStack; bd != NULL; bd = bd->link)
      res += bd->blocks;

    return res;
}

W_
retainerStackBlocks(void)
{
    return traverseWorkStackBlocks(&g_retainerTraverseState);
}

/* -----------------------------------------------------------------------------
 * Returns true if stackTop is at the stack boundary of the current stack,
 * i.e., if the current stack chunk is empty.
 * -------------------------------------------------------------------------- */
static INLINE bool
isOnBoundary( traverseState *ts )
{
    return ts->stackTop == ts->currentStackBoundary;
}

/* -----------------------------------------------------------------------------
 * Initializes *info from ptrs and payload.
 * Invariants:
 *   payload[] begins with ptrs pointers followed by non-pointers.
 * -------------------------------------------------------------------------- */
static INLINE void
init_ptrs( stackPos *info, uint32_t ptrs, StgPtr payload )
{
    info->type              = posTypePtrs;
    info->next.ptrs.pos     = 0;
    info->next.ptrs.ptrs    = ptrs;
    info->next.ptrs.payload = payload;
}

/* -----------------------------------------------------------------------------
 * Find the next object from *info.
 * -------------------------------------------------------------------------- */
static INLINE StgClosure *
find_ptrs( stackPos *info )
{
    if (info->next.ptrs.pos < info->next.ptrs.ptrs) {
        return (StgClosure *)info->next.ptrs.payload[info->next.ptrs.pos++];
    } else {
        return NULL;
    }
}

/* -----------------------------------------------------------------------------
 *  Initializes *info from SRT information stored in *infoTable.
 * -------------------------------------------------------------------------- */
static INLINE void
init_srt_fun( stackPos *info, const StgFunInfoTable *infoTable )
{
    info->type = posTypeSRT;
    if (infoTable->i.srt) {
        info->next.srt.srt = (StgClosure*)GET_FUN_SRT(infoTable);
    } else {
        info->next.srt.srt = NULL;
    }
}

static INLINE void
init_srt_thunk( stackPos *info, const StgThunkInfoTable *infoTable )
{
    info->type = posTypeSRT;
    if (infoTable->i.srt) {
        info->next.srt.srt = (StgClosure*)GET_SRT(infoTable);
    } else {
        info->next.srt.srt = NULL;
    }
}

/* -----------------------------------------------------------------------------
 * Find the next object from *info.
 * -------------------------------------------------------------------------- */
static INLINE StgClosure *
find_srt( stackPos *info )
{
    StgClosure *c;
    if (info->type == posTypeSRT) {
        c = info->next.srt.srt;
        info->next.srt.srt = NULL;
        return c;
    }
}

/* -----------------------------------------------------------------------------
 * Pushes an element onto traverse stack
 * -------------------------------------------------------------------------- */
static void
pushStackElement(traverseState *ts, stackElement *se)
{
    bdescr *nbd;      // Next Block Descriptor
    if (ts->stackTop - 1 < ts->stackBottom) {
#if defined(DEBUG_RETAINER)
        // debugBelch("push() to the next stack.\n");
#endif
        // currentStack->free is updated when the active stack is switched
        // to the next stack.
        ts->currentStack->free = (StgPtr)ts->stackTop;

        if (ts->currentStack->link == NULL) {
            nbd = allocGroup(BLOCKS_IN_STACK);
            nbd->link = NULL;
            nbd->u.back = ts->currentStack;
            ts->currentStack->link = nbd;
        } else
            nbd = ts->currentStack->link;

        newStackBlock(ts, nbd);
    }

    // adjust stackTop (acutal push)
    ts->stackTop--;
    // If the size of stackElement was huge, we would better replace the
    // following statement by either a memcpy() call or a switch statement
    // on the type of the element. Currently, the size of stackElement is
    // small enough (5 words) that this direct assignment seems to be enough.
    *ts->stackTop = *se;

#if defined(DEBUG_RETAINER)
    ts->stackSize++;
    if (ts->stackSize > ts->maxStackSize) ts->maxStackSize = ts->stackSize;
    ASSERT(ts->stackSize >= 0);
    debugBelch("stackSize = %d\n", ts->stackSize);
#endif

}

/* Push an object onto traverse stack. This method can be used anytime
 * instead of calling retainClosure(), it exists in order to use an
 * explicit stack instead of direct recursion.
 *
 *  *cp - object's parent
 *  *c - closure
 *  c_child_r - closure retainer.
 */
static INLINE void
traversePushClosure(traverseState *ts, StgClosure *c, StgClosure *cp, stackData data) {
    stackElement se;

    se.c = c;
    se.cp = cp;
    se.data = data;
    se.info.type = posTypeFresh;

    pushStackElement(ts, &se);
};

/* -----------------------------------------------------------------------------
 *  push() pushes a stackElement representing the next child of *c
 *  onto the traverse stack. If *c has no child, *first_child is set
 *  to NULL and nothing is pushed onto the stack. If *c has only one
 *  child, *c_child is set to that child and nothing is pushed onto
 *  the stack.  If *c has more than two children, *first_child is set
 *  to the first child and a stackElement representing the second
 *  child is pushed onto the stack.

 *  Invariants:
 *     *c_child_r is the most recent retainer of *c's children.
 *     *c is not any of TSO, AP, PAP, AP_STACK, which means that
 *        there cannot be any stack objects.
 *  Note: SRTs are considered to  be children as well.
 * -------------------------------------------------------------------------- */
static INLINE void
traversePushChildren(traverseState *ts, StgClosure *c, stackData data, StgClosure **first_child)
{
    stackElement se;
    bdescr *nbd;      // Next Block Descriptor

#if defined(DEBUG_RETAINER)
    debugBelch("push(): stackTop = 0x%x, currentStackBoundary = 0x%x\n", ts->stackTop, ts->currentStackBoundary);
#endif

    ASSERT(get_itbl(c)->type != TSO);
    ASSERT(get_itbl(c)->type != AP_STACK);

    //
    // fill in se
    //

    se.c = c;
    se.data = data;
    // Note: se.cp ommitted on purpose, only retainPushClosure uses that.

    // fill in se.info
    switch (get_itbl(c)->type) {
        // no child, no SRT
    case CONSTR_0_1:
    case CONSTR_0_2:
    case ARR_WORDS:
    case COMPACT_NFDATA:
        *first_child = NULL;
        return;

        // one child (fixed), no SRT
    case MUT_VAR_CLEAN:
    case MUT_VAR_DIRTY:
        *first_child = ((StgMutVar *)c)->var;
        return;
    case THUNK_SELECTOR:
        *first_child = ((StgSelector *)c)->selectee;
        return;
    case BLACKHOLE:
        *first_child = ((StgInd *)c)->indirectee;
        return;
    case CONSTR_1_0:
    case CONSTR_1_1:
        *first_child = c->payload[0];
        return;

        // For CONSTR_2_0 and MVAR, we use se.info.step to record the position
        // of the next child. We do not write a separate initialization code.
        // Also we do not have to initialize info.type;

        // two children (fixed), no SRT
        // need to push a stackElement, but nothing to store in se.info
    case CONSTR_2_0:
        *first_child = c->payload[0];         // return the first pointer
        se.info.type = posTypeStep;
        se.info.next.step = 2;            // 2 = second
        break;

        // three children (fixed), no SRT
        // need to push a stackElement
    case MVAR_CLEAN:
    case MVAR_DIRTY:
        // head must be TSO and the head of a linked list of TSOs.
        // Shoule it be a child? Seems to be yes.
        *first_child = (StgClosure *)((StgMVar *)c)->head;
        se.info.type = posTypeStep;
        se.info.next.step = 2;            // 2 = second
        break;

        // three children (fixed), no SRT
    case WEAK:
        *first_child = ((StgWeak *)c)->key;
        se.info.type = posTypeStep;
        se.info.next.step = 2;
        break;

        // layout.payload.ptrs, no SRT
    case TVAR:
    case CONSTR:
    case CONSTR_NOCAF:
    case PRIM:
    case MUT_PRIM:
    case BCO:
        init_ptrs(&se.info, get_itbl(c)->layout.payload.ptrs,
                  (StgPtr)c->payload);
        *first_child = find_ptrs(&se.info);
        if (*first_child == NULL)
            return;   // no child
        break;

        // StgMutArrPtr.ptrs, no SRT
    case MUT_ARR_PTRS_CLEAN:
    case MUT_ARR_PTRS_DIRTY:
    case MUT_ARR_PTRS_FROZEN_CLEAN:
    case MUT_ARR_PTRS_FROZEN_DIRTY:
        init_ptrs(&se.info, ((StgMutArrPtrs *)c)->ptrs,
                  (StgPtr)(((StgMutArrPtrs *)c)->payload));
        *first_child = find_ptrs(&se.info);
        if (*first_child == NULL)
            return;
        break;

        // StgMutArrPtr.ptrs, no SRT
    case SMALL_MUT_ARR_PTRS_CLEAN:
    case SMALL_MUT_ARR_PTRS_DIRTY:
    case SMALL_MUT_ARR_PTRS_FROZEN_CLEAN:
    case SMALL_MUT_ARR_PTRS_FROZEN_DIRTY:
        init_ptrs(&se.info, ((StgSmallMutArrPtrs *)c)->ptrs,
                  (StgPtr)(((StgSmallMutArrPtrs *)c)->payload));
        *first_child = find_ptrs(&se.info);
        if (*first_child == NULL)
            return;
        break;

    // layout.payload.ptrs, SRT
    case FUN_STATIC:
    case FUN:           // *c is a heap object.
    case FUN_2_0:
        init_ptrs(&se.info, get_itbl(c)->layout.payload.ptrs, (StgPtr)c->payload);
        *first_child = find_ptrs(&se.info);
        if (*first_child == NULL)
            // no child from ptrs, so check SRT
            goto fun_srt_only;
        break;

    case THUNK:
    case THUNK_2_0:
        init_ptrs(&se.info, get_itbl(c)->layout.payload.ptrs,
                  (StgPtr)((StgThunk *)c)->payload);
        *first_child = find_ptrs(&se.info);
        if (*first_child == NULL)
            // no child from ptrs, so check SRT
            goto thunk_srt_only;
        break;

        // 1 fixed child, SRT
    case FUN_1_0:
    case FUN_1_1:
        *first_child = c->payload[0];
        ASSERT(*first_child != NULL);
        init_srt_fun(&se.info, get_fun_itbl(c));
        break;

    case THUNK_1_0:
    case THUNK_1_1:
        *first_child = ((StgThunk *)c)->payload[0];
        ASSERT(*first_child != NULL);
        init_srt_thunk(&se.info, get_thunk_itbl(c));
        break;

    case FUN_0_1:      // *c is a heap object.
    case FUN_0_2:
    fun_srt_only:
        init_srt_fun(&se.info, get_fun_itbl(c));
        *first_child = find_srt(&se.info);
        if (*first_child == NULL)
            return;     // no child
        break;

    // SRT only
    case THUNK_STATIC:
        ASSERT(get_itbl(c)->srt != 0);
    case THUNK_0_1:
    case THUNK_0_2:
    thunk_srt_only:
        init_srt_thunk(&se.info, get_thunk_itbl(c));
        *first_child = find_srt(&se.info);
        if (*first_child == NULL)
            return;     // no child
        break;

    case TREC_CHUNK:
        *first_child = (StgClosure *)((StgTRecChunk *)c)->prev_chunk;
        se.info.type = posTypeStep;
        se.info.next.step = 0;  // entry no.
        break;

        // cannot appear
    case PAP:
    case AP:
    case AP_STACK:
    case TSO:
    case STACK:
    case IND_STATIC:
        // stack objects
    case UPDATE_FRAME:
    case CATCH_FRAME:
    case UNDERFLOW_FRAME:
    case STOP_FRAME:
    case RET_BCO:
    case RET_SMALL:
    case RET_BIG:
        // invalid objects
    case IND:
    case INVALID_OBJECT:
    default:
        barf("Invalid object *c in push(): %d", get_itbl(c)->type);
        return;
    }

    // se.cp has to be initialized when type==posTypeFresh. We don't do that
    // here though. So type must be !=posTypeFresh.
    ASSERT(se.info.type != posTypeFresh);

    pushStackElement(ts, &se);
}

/* -----------------------------------------------------------------------------
 *  popOff() and popOffReal(): Pop a stackElement off the traverse stack.
 *  Invariants:
 *    stackTop cannot be equal to stackLimit unless the whole stack is
 *    empty, in which case popOff() is not allowed.
 *  Note:
 *    You can think of popOffReal() as a part of popOff() which is
 *    executed at the end of popOff() in necessary. Since popOff() is
 *    likely to be executed quite often while popOffReal() is not, we
 *    separate popOffReal() from popOff(), which is declared as an
 *    INLINE function (for the sake of execution speed).  popOffReal()
 *    is called only within popOff() and nowhere else.
 * -------------------------------------------------------------------------- */
static void
popStackElement(traverseState *ts) {
#if defined(DEBUG_RETAINER)
    debugBelch("popStackElement(): stackTop = 0x%x, currentStackBoundary = 0x%x\n", ts->stackTop, ts->currentStackBoundary);
#endif

    ASSERT(ts->stackTop != ts->stackLimit);
    ASSERT(!isEmptyWorkStack(ts));

    // <= (instead of <) is wrong!
    if (ts->stackTop + 1 < ts->stackLimit) {
        ts->stackTop++;
#if defined(DEBUG_RETAINER)
        ts->stackSize--;
        if (ts->stackSize > ts->maxStackSize) ts->maxStackSize = ts->stackSize;
        ASSERT(ts->stackSize >= 0);
        debugBelch("stackSize = (--) %d\n", ts->stackSize);
#endif
        return;
    }

    bdescr *pbd;    // Previous Block Descriptor

#if defined(DEBUG_RETAINER)
    debugBelch("pop() to the previous stack.\n");
#endif

    ASSERT(ts->stackTop + 1 == ts->stackLimit);
    ASSERT(ts->stackBottom == (stackElement *)ts->currentStack->start);

    if (ts->firstStack == ts->currentStack) {
        // The stack is completely empty.
        ts->stackTop++;
        ASSERT(ts->stackTop == ts->stackLimit);
#if defined(DEBUG_RETAINER)
        ts->stackSize--;
        if (ts->stackSize > ts->maxStackSize) ts->maxStackSize = ts->stackSize;
        ASSERT(ts->stackSize >= 0);
        debugBelch("stackSize = %d\n", ts->stackSize);
#endif
        return;
    }

    // currentStack->free is updated when the active stack is switched back
    // to the previous stack.
    ts->currentStack->free = (StgPtr)ts->stackLimit;

    // find the previous block descriptor
    pbd = ts->currentStack->u.back;
    ASSERT(pbd != NULL);

    returnToOldStack(ts, pbd);

#if defined(DEBUG_RETAINER)
    ts->stackSize--;
    if (ts->stackSize > ts->maxStackSize) ts->maxStackSize = ts->stackSize;
    ASSERT(ts->stackSize >= 0);
    debugBelch("stackSize = %d\n", ts->stackSize);
#endif
}

/* -----------------------------------------------------------------------------
 *  Finds the next object to be considered for retainer profiling and store
 *  its pointer to *c.
 *  If the unprocessed object was stored in the stack (posTypeFresh), the
 *  this object is returned as-is. Otherwise Test if the topmost stack
 *  element indicates that more objects are left,
 *  and if so, retrieve the first object and store its pointer to *c. Also,
 *  set *cp and *data appropriately, both of which are stored in the stack
 *  element.  The topmost stack element then is overwritten so as for it to now
 *  denote the next object.
 *  If the topmost stack element indicates no more objects are left, pop
 *  off the stack element until either an object can be retrieved or
 *  the current stack chunk becomes empty, indicated by true returned by
 *  isOnBoundary(), in which case *c is set to NULL.
 *  Note:
 *    It is okay to call this function even when the current stack chunk
 *    is empty.
 * -------------------------------------------------------------------------- */
static INLINE void
traversePop(traverseState *ts, StgClosure **c, StgClosure **cp, stackData *data)
{
    stackElement *se;

#if defined(DEBUG_RETAINER)
    debugBelch("pop(): stackTop = 0x%x, currentStackBoundary = 0x%x\n", ts->stackTop, ts->currentStackBoundary);
#endif

    // Is this the last internal element? If so instead of modifying the current
    // stackElement in place we actually remove it from the stack.
    bool last = false;

    do {
        if (isOnBoundary(ts)) {     // if the current stack chunk is depleted
            *c = NULL;
            return;
        }

        // Note: Below every `break`, where the loop condition is true, must be
        // accompanied by a popOff() otherwise this is an infinite loop.
        se = ts->stackTop;

        // If this is a top-level element, you should pop that out.
        if (se->info.type == posTypeFresh) {
            *cp = se->cp;
            *c = se->c;
            *data = se->data;
            popStackElement(ts);
            return;
        }

        // Note: The first ptr of all of these was already returned as
        // *fist_child in push(), so we always start with the second field.
        switch (get_itbl(se->c)->type) {
            // two children (fixed), no SRT
            // nothing in se.info
        case CONSTR_2_0:
            *c = se->c->payload[1];
            last = true;
            goto out;

            // three children (fixed), no SRT
            // need to push a stackElement
        case MVAR_CLEAN:
        case MVAR_DIRTY:
            if (se->info.next.step == 2) {
                *c = (StgClosure *)((StgMVar *)se->c)->tail;
                se->info.next.step++;             // move to the next step
                // no popOff
            } else {
                *c = ((StgMVar *)se->c)->value;
                last = true;
            }
            goto out;

            // three children (fixed), no SRT
        case WEAK:
            if (se->info.next.step == 2) {
                *c = ((StgWeak *)se->c)->value;
                se->info.next.step++;
                // no popOff
            } else {
                *c = ((StgWeak *)se->c)->finalizer;
                last = true;
            }
            goto out;

        case TREC_CHUNK: {
            // These are pretty complicated: we have N entries, each
            // of which contains 3 fields that we want to follow.  So
            // we divide the step counter: the 2 low bits indicate
            // which field, and the rest of the bits indicate the
            // entry number (starting from zero).
            TRecEntry *entry;
            uint32_t entry_no = se->info.next.step >> 2;
            uint32_t field_no = se->info.next.step & 3;
            if (entry_no == ((StgTRecChunk *)se->c)->next_entry_idx) {
                *c = NULL;
                popStackElement(ts);
                break; // this breaks out of the switch not the loop
            }
            entry = &((StgTRecChunk *)se->c)->entries[entry_no];
            if (field_no == 0) {
                *c = (StgClosure *)entry->tvar;
            } else if (field_no == 1) {
                *c = entry->expected_value;
            } else {
                *c = entry->new_value;
            }
            se->info.next.step++;
            goto out;
        }

        case TVAR:
        case CONSTR:
        case PRIM:
        case MUT_PRIM:
        case BCO:
            // StgMutArrPtr.ptrs, no SRT
        case MUT_ARR_PTRS_CLEAN:
        case MUT_ARR_PTRS_DIRTY:
        case MUT_ARR_PTRS_FROZEN_CLEAN:
        case MUT_ARR_PTRS_FROZEN_DIRTY:
        case SMALL_MUT_ARR_PTRS_CLEAN:
        case SMALL_MUT_ARR_PTRS_DIRTY:
        case SMALL_MUT_ARR_PTRS_FROZEN_CLEAN:
        case SMALL_MUT_ARR_PTRS_FROZEN_DIRTY:
            *c = find_ptrs(&se->info);
            if (*c == NULL) {
                popStackElement(ts);
                break; // this breaks out of the switch not the loop
            }
            goto out;

            // layout.payload.ptrs, SRT
        case FUN:         // always a heap object
        case FUN_STATIC:
        case FUN_2_0:
            if (se->info.type == posTypePtrs) {
                *c = find_ptrs(&se->info);
                if (*c != NULL) {
                    goto out;
                }
                init_srt_fun(&se->info, get_fun_itbl(se->c));
            }
            goto do_srt;

        case THUNK:
        case THUNK_2_0:
            if (se->info.type == posTypePtrs) {
                *c = find_ptrs(&se->info);
                if (*c != NULL) {
                    goto out;
                }
                init_srt_thunk(&se->info, get_thunk_itbl(se->c));
            }
            goto do_srt;

            // SRT
        do_srt:
        case THUNK_STATIC:
        case FUN_0_1:
        case FUN_0_2:
        case THUNK_0_1:
        case THUNK_0_2:
        case FUN_1_0:
        case FUN_1_1:
        case THUNK_1_0:
        case THUNK_1_1:
            *c = find_srt(&se->info);
            if(*c == NULL) {
                popStackElement(ts);
                break; // this breaks out of the switch not the loop
            }
            goto out;

            // no child (fixed), no SRT
        case CONSTR_0_1:
        case CONSTR_0_2:
        case ARR_WORDS:
            // one child (fixed), no SRT
        case MUT_VAR_CLEAN:
        case MUT_VAR_DIRTY:
        case THUNK_SELECTOR:
        case CONSTR_1_1:
            // cannot appear
        case PAP:
        case AP:
        case AP_STACK:
        case TSO:
        case STACK:
        case IND_STATIC:
        case CONSTR_NOCAF:
            // stack objects
        case UPDATE_FRAME:
        case CATCH_FRAME:
        case UNDERFLOW_FRAME:
        case STOP_FRAME:
        case RET_BCO:
        case RET_SMALL:
        case RET_BIG:
            // invalid objects
        case IND:
        case INVALID_OBJECT:
        default:
            barf("Invalid object *c in pop(): %d", get_itbl(se->c)->type);
            return;
        }
    } while (*c == NULL);

out:

    ASSERT(*c != NULL);

    *cp = se->c;
    *data = se->data;

    if(last)
        popStackElement(ts);

    return;

}

/* -----------------------------------------------------------------------------
 * RETAINER PROFILING ENGINE
 * -------------------------------------------------------------------------- */

void
initRetainerProfiling( void )
{
    initializeAllRetainerSet();
    retainerGeneration = 0;
}

/* -----------------------------------------------------------------------------
 *  This function must be called before f-closing prof_file.
 * -------------------------------------------------------------------------- */
void
endRetainerProfiling( void )
{
    outputAllRetainerSet(prof_file);
}

/* -----------------------------------------------------------------------------
 *  Returns the actual pointer to the retainer set of the closure *c.
 *  It may adjust RSET(c) subject to flip.
 *  Side effects:
 *    RSET(c) is initialized to NULL if its current value does not
 *    conform to flip.
 *  Note:
 *    Even though this function has side effects, they CAN be ignored because
 *    subsequent calls to retainerSetOf() always result in the same return value
 *    and retainerSetOf() is the only way to retrieve retainerSet of a given
 *    closure.
 *    We have to perform an XOR (^) operation each time a closure is examined.
 *    The reason is that we do not know when a closure is visited last.
 * -------------------------------------------------------------------------- */
static INLINE void
traverseMaybeInitClosureData(StgClosure *c)
{
    if (!isTravDataValid(c)) {
        setTravDataToZero(c);
    }
}

/* -----------------------------------------------------------------------------
 * Returns true if *c is a retainer.
 * In general the retainers are the objects that may be the roots of the
 * collection. Basically this roots represents programmers threads
 * (TSO) with their stack and thunks.
 *
 * In addition we mark all mutable objects as a retainers, the reason for
 * that decision is lost in time.
 * -------------------------------------------------------------------------- */
static INLINE bool
isRetainer( StgClosure *c )
{
    switch (get_itbl(c)->type) {
        //
        //  True case
        //
        // TSOs MUST be retainers: they constitute the set of roots.
    case TSO:
    case STACK:

        // mutable objects
    case MUT_PRIM:
    case MVAR_CLEAN:
    case MVAR_DIRTY:
    case TVAR:
    case MUT_VAR_CLEAN:
    case MUT_VAR_DIRTY:
    case MUT_ARR_PTRS_CLEAN:
    case MUT_ARR_PTRS_DIRTY:
    case SMALL_MUT_ARR_PTRS_CLEAN:
    case SMALL_MUT_ARR_PTRS_DIRTY:
    case BLOCKING_QUEUE:

        // thunks are retainers.
    case THUNK:
    case THUNK_1_0:
    case THUNK_0_1:
    case THUNK_2_0:
    case THUNK_1_1:
    case THUNK_0_2:
    case THUNK_SELECTOR:
    case AP:
    case AP_STACK:

        // Static thunks, or CAFS, are obviously retainers.
    case THUNK_STATIC:

        // WEAK objects are roots; there is separate code in which traversing
        // begins from WEAK objects.
    case WEAK:
        return true;

        //
        // False case
        //

        // constructors
    case CONSTR:
    case CONSTR_NOCAF:
    case CONSTR_1_0:
    case CONSTR_0_1:
    case CONSTR_2_0:
    case CONSTR_1_1:
    case CONSTR_0_2:
        // functions
    case FUN:
    case FUN_1_0:
    case FUN_0_1:
    case FUN_2_0:
    case FUN_1_1:
    case FUN_0_2:
        // partial applications
    case PAP:
        // indirection
    // IND_STATIC used to be an error, but at the moment it can happen
    // as isAlive doesn't look through IND_STATIC as it ignores static
    // closures. See trac #3956 for a program that hit this error.
    case IND_STATIC:
    case BLACKHOLE:
    case WHITEHOLE:
        // static objects
    case FUN_STATIC:
        // misc
    case PRIM:
    case BCO:
    case ARR_WORDS:
    case COMPACT_NFDATA:
        // STM
    case TREC_CHUNK:
        // immutable arrays
    case MUT_ARR_PTRS_FROZEN_CLEAN:
    case MUT_ARR_PTRS_FROZEN_DIRTY:
    case SMALL_MUT_ARR_PTRS_FROZEN_CLEAN:
    case SMALL_MUT_ARR_PTRS_FROZEN_DIRTY:
        return false;

        //
        // Error case
        //
        // Stack objects are invalid because they are never treated as
        // legal objects during retainer profiling.
    case UPDATE_FRAME:
    case CATCH_FRAME:
    case CATCH_RETRY_FRAME:
    case CATCH_STM_FRAME:
    case UNDERFLOW_FRAME:
    case ATOMICALLY_FRAME:
    case STOP_FRAME:
    case RET_BCO:
    case RET_SMALL:
    case RET_BIG:
    case RET_FUN:
        // other cases
    case IND:
    case INVALID_OBJECT:
    default:
        barf("Invalid object in isRetainer(): %d", get_itbl(c)->type);
        return false;
    }
}

/* -----------------------------------------------------------------------------
 *  Returns the retainer function value for the closure *c, i.e., R(*c).
 *  This function does NOT return the retainer(s) of *c.
 *  Invariants:
 *    *c must be a retainer.
 * -------------------------------------------------------------------------- */
static INLINE retainer
getRetainerFrom( StgClosure *c )
{
    ASSERT(isRetainer(c));

    return c->header.prof.ccs;
}

/* -----------------------------------------------------------------------------
 *  Associates the retainer set *s with the closure *c, that is, *s becomes
 *  the retainer set of *c.
 *  Invariants:
 *    c != NULL
 *    s != NULL
 * -------------------------------------------------------------------------- */
static INLINE void
associate( StgClosure *c, RetainerSet *s )
{
    // StgWord has the same size as pointers, so the following type
    // casting is okay.
    RSET(c) = (RetainerSet *)((StgWord)s | flip);
}

/* -----------------------------------------------------------------------------
   Call retainPushClosure for each of the closures covered by a large bitmap.
   -------------------------------------------------------------------------- */

static void
traverseLargeBitmap (traverseState *ts, StgPtr p, StgLargeBitmap *large_bitmap,
                     uint32_t size, StgClosure *c, stackData data)
{
    uint32_t i, b;
    StgWord bitmap;

    b = 0;
    bitmap = large_bitmap->bitmap[b];
    for (i = 0; i < size; ) {
        if ((bitmap & 1) == 0) {
            traversePushClosure(ts, (StgClosure *)*p, c, data);
        }
        i++;
        p++;
        if (i % BITS_IN(W_) == 0) {
            b++;
            bitmap = large_bitmap->bitmap[b];
        } else {
            bitmap = bitmap >> 1;
        }
    }
}

static INLINE StgPtr
traverseSmallBitmap (traverseState *ts, StgPtr p, uint32_t size, StgWord bitmap,
                     StgClosure *c, stackData data)
{
    while (size > 0) {
        if ((bitmap & 1) == 0) {
            traversePushClosure(ts, (StgClosure *)*p, c, data);
        }
        p++;
        bitmap = bitmap >> 1;
        size--;
    }
    return p;
}

/* -----------------------------------------------------------------------------
 *  Process all the objects in the stack chunk from stackStart to stackEnd
 *  with *c and *c_child_r being their parent and their most recent retainer,
 *  respectively. Treat stackOptionalFun as another child of *c if it is
 *  not NULL.
 *  Invariants:
 *    *c is one of the following: TSO, AP_STACK.
 *    If *c is TSO, c == c_child_r.
 *    stackStart < stackEnd.
 *    RSET(c) and RSET(c_child_r) are valid, i.e., their
 *    interpretation conforms to the current value of flip (even when they
 *    are interpreted to be NULL).
 *    If *c is TSO, its state is not ThreadComplete,or ThreadKilled,
 *    which means that its stack is ready to process.
 *  Note:
 *    This code was almost plagiarzied from GC.c! For each pointer,
 *    retainPushClosure() is invoked instead of evacuate().
 * -------------------------------------------------------------------------- */
static void
traversePushStack(traverseState *ts, StgClosure *cp, stackData data,
                  StgPtr stackStart, StgPtr stackEnd)
{
    stackElement *oldStackBoundary;
    StgPtr p;
    const StgRetInfoTable *info;
    StgWord bitmap;
    uint32_t size;

    /*
      Each invocation of retainStack() creates a new virtual
      stack. Since all such stacks share a single common stack, we
      record the current currentStackBoundary, which will be restored
      at the exit.
    */
    oldStackBoundary = ts->currentStackBoundary;
    ts->currentStackBoundary = ts->stackTop;

#if defined(DEBUG_RETAINER)
    debugBelch("retainStack() called: oldStackBoundary = 0x%x, currentStackBoundary = 0x%x\n",
        oldStackBoundary, ts->currentStackBoundary);
#endif

    ASSERT(get_itbl(cp)->type == STACK);

    p = stackStart;
    while (p < stackEnd) {
        info = get_ret_itbl((StgClosure *)p);

        switch(info->i.type) {

        case UPDATE_FRAME:
            traversePushClosure(ts, ((StgUpdateFrame *)p)->updatee, cp, data);
            p += sizeofW(StgUpdateFrame);
            continue;

        case UNDERFLOW_FRAME:
        case STOP_FRAME:
        case CATCH_FRAME:
        case CATCH_STM_FRAME:
        case CATCH_RETRY_FRAME:
        case ATOMICALLY_FRAME:
        case RET_SMALL:
            bitmap = BITMAP_BITS(info->i.layout.bitmap);
            size   = BITMAP_SIZE(info->i.layout.bitmap);
            p++;
            p = traverseSmallBitmap(ts, p, size, bitmap, cp, data);

        follow_srt:
            if (info->i.srt) {
                traversePushClosure(ts, GET_SRT(info), cp, data);
            }
            continue;

        case RET_BCO: {
            StgBCO *bco;

            p++;
            traversePushClosure(ts, (StgClosure*)*p, cp, data);
            bco = (StgBCO *)*p;
            p++;
            size = BCO_BITMAP_SIZE(bco);
            traverseLargeBitmap(ts, p, BCO_BITMAP(bco), size, cp, data);
            p += size;
            continue;
        }

            // large bitmap (> 32 entries, or > 64 on a 64-bit machine)
        case RET_BIG:
            size = GET_LARGE_BITMAP(&info->i)->size;
            p++;
            traverseLargeBitmap(ts, p, GET_LARGE_BITMAP(&info->i),
                                size, cp, data);
            p += size;
            // and don't forget to follow the SRT
            goto follow_srt;

        case RET_FUN: {
            StgRetFun *ret_fun = (StgRetFun *)p;
            const StgFunInfoTable *fun_info;

            traversePushClosure(ts, ret_fun->fun, cp, data);
            fun_info = get_fun_itbl(UNTAG_CONST_CLOSURE(ret_fun->fun));

            p = (P_)&ret_fun->payload;
            switch (fun_info->f.fun_type) {
            case ARG_GEN:
                bitmap = BITMAP_BITS(fun_info->f.b.bitmap);
                size = BITMAP_SIZE(fun_info->f.b.bitmap);
                p = traverseSmallBitmap(ts, p, size, bitmap, cp, data);
                break;
            case ARG_GEN_BIG:
                size = GET_FUN_LARGE_BITMAP(fun_info)->size;
                traverseLargeBitmap(ts, p, GET_FUN_LARGE_BITMAP(fun_info),
                                    size, cp, data);
                p += size;
                break;
            default:
                bitmap = BITMAP_BITS(stg_arg_bitmaps[fun_info->f.fun_type]);
                size = BITMAP_SIZE(stg_arg_bitmaps[fun_info->f.fun_type]);
                p = traverseSmallBitmap(ts, p, size, bitmap, cp, data);
                break;
            }
            goto follow_srt;
        }

        default:
            barf("Invalid object found in retainStack(): %d",
                 (int)(info->i.type));
        }
    }

    // restore currentStackBoundary
    ts->currentStackBoundary = oldStackBoundary;
#if defined(DEBUG_RETAINER)
    debugBelch("retainStack() finished: currentStackBoundary = 0x%x\n",
        ts->currentStackBoundary);
#endif
}

/* ----------------------------------------------------------------------------
 * Call retainPushClosure for each of the children of a PAP/AP
 * ------------------------------------------------------------------------- */

static INLINE StgPtr
traversePAP (traverseState *ts,
                    StgClosure *pap,    /* NOT tagged */
                    stackData data,
                    StgClosure *fun,    /* tagged */
                    StgClosure** payload, StgWord n_args)
{
    StgPtr p;
    StgWord bitmap;
    const StgFunInfoTable *fun_info;

    traversePushClosure(ts, fun, pap, data);
    fun = UNTAG_CLOSURE(fun);
    fun_info = get_fun_itbl(fun);
    ASSERT(fun_info->i.type != PAP);

    p = (StgPtr)payload;

    switch (fun_info->f.fun_type) {
    case ARG_GEN:
        bitmap = BITMAP_BITS(fun_info->f.b.bitmap);
        p = traverseSmallBitmap(ts, p, n_args, bitmap,
                                pap, data);
        break;
    case ARG_GEN_BIG:
        traverseLargeBitmap(ts, p, GET_FUN_LARGE_BITMAP(fun_info),
                            n_args, pap, data);
        p += n_args;
        break;
    case ARG_BCO:
        traverseLargeBitmap(ts, (StgPtr)payload, BCO_BITMAP(fun),
                            n_args, pap, data);
        p += n_args;
        break;
    default:
        bitmap = BITMAP_BITS(stg_arg_bitmaps[fun_info->f.fun_type]);
        p = traverseSmallBitmap(ts, p, n_args, bitmap, pap, data);
        break;
    }
    return p;
}

static bool
retainVisitClosure( const StgClosure *c, const StgClosure *cp, const stackData data, stackData *out_data )
{
    retainer r = data.c_child_r;
    RetainerSet *s, *retainerSetOfc;
    retainerSetOfc = retainerSetOf(c);

    timesAnyObjectVisited++;

    // c  = current closure under consideration,
    // cp = current closure's parent,
    // r  = current closure's most recent retainer
    //
    // Loop invariants (on the meaning of c, cp, r, and their retainer sets):
    //   RSET(cp) and RSET(r) are valid.
    //   RSET(c) is valid only if c has been visited before.
    //
    // Loop invariants (on the relation between c, cp, and r)
    //   if cp is not a retainer, r belongs to RSET(cp).
    //   if cp is a retainer, r == cp.

    // Now compute s:
    //    isRetainer(cp) == true => s == NULL
    //    isRetainer(cp) == false => s == cp.retainer
    if (isRetainer(cp))
        s = NULL;
    else
        s = retainerSetOf(cp);

    // (c, cp, r, s) is available.

    // (c, cp, r, s, R_r) is available, so compute the retainer set for *c.
    if (retainerSetOfc == NULL) {
        // This is the first visit to *c.
        numObjectVisited++;

        if (s == NULL)
            associate(c, singleton(r));
        else
            // s is actually the retainer set of *c!
            associate(c, s);

        // compute c_child_r
        out_data->c_child_r = isRetainer(c) ? getRetainerFrom(c) : r;
    } else {
        // This is not the first visit to *c.
        if (isMember(r, retainerSetOfc))
            return 1;          // no need to process child

        if (s == NULL)
            associate(c, addElement(r, retainerSetOfc));
        else {
            // s is not NULL and cp is not a retainer. This means that
            // each time *cp is visited, so is *c. Thus, if s has
            // exactly one more element in its retainer set than c, s
            // is also the new retainer set for *c.
            if (s->num == retainerSetOfc->num + 1) {
                associate(c, s);
            }
            // Otherwise, just add R_r to the current retainer set of *c.
            else {
                associate(c, addElement(r, retainerSetOfc));
            }
        }

        if (isRetainer(c))
            return 1;          // no need to process child

        // compute c_child_r
        out_data->c_child_r = r;
    }

    // now, RSET() of all of *c, *cp, and *r is valid.
    // (c, c_child_r) are available.

    return 0;
}

/* -----------------------------------------------------------------------------
 *  Compute the retainer set of *c0 and all its desecents by traversing.
 *  *cp0 is the parent of *c0, and *r0 is the most recent retainer of *c0.
 *  Invariants:
 *    c0 = cp0 = r0 holds only for root objects.
 *    RSET(cp0) and RSET(r0) are valid, i.e., their
 *    interpretation conforms to the current value of flip (even when they
 *    are interpreted to be NULL).
 *    However, RSET(c0) may be corrupt, i.e., it may not conform to
 *    the current value of flip. If it does not, during the execution
 *    of this function, RSET(c0) must be initialized as well as all
 *    its descendants.
 *  Note:
 *    stackTop must be the same at the beginning and the exit of this function.
 *    *c0 can be TSO (as well as AP_STACK).
 * -------------------------------------------------------------------------- */
static void
traverseWorkStack(traverseState *ts, visitClosure_cb visit_cb)
{
    // first_child = first child of c
    StgClosure *c, *cp, *first_child;
    stackData data, child_data;
    StgWord typeOfc;

    // c = Current closure                           (possibly tagged)
    // cp = Current closure's Parent                 (NOT tagged)
    // data = current closures' associated data      (NOT tagged)
    // data_out = data to associate with current closure's children

loop:
    traversePop(ts, &c, &cp, &data);

    if (c == NULL) {
        return;
    }
inner_loop:
    c = UNTAG_CLOSURE(c);

    typeOfc = get_itbl(c)->type;

    // special cases
    switch (typeOfc) {
    case TSO:
        if (((StgTSO *)c)->what_next == ThreadComplete ||
            ((StgTSO *)c)->what_next == ThreadKilled) {
#if defined(DEBUG_RETAINER)
            debugBelch("ThreadComplete or ThreadKilled encountered in retainClosure()\n");
#endif
            goto loop;
        }
        break;

    case IND_STATIC:
        // We just skip IND_STATIC, so it's never visited.
        c = ((StgIndStatic *)c)->indirectee;
        goto inner_loop;

    case CONSTR_NOCAF:
        // static objects with no pointers out, so goto loop.

        // It is not just enough not to visit *c; it is
        // mandatory because CONSTR_NOCAF are not reachable from
        // scavenged_static_objects, the list from which is assumed to traverse
        // all static objects after major garbage collections.
        goto loop;

    case THUNK_STATIC:
        if (get_itbl(c)->srt == 0) {
            // No need to visit *c; no dynamic objects are reachable from it.
            //
            // Static objects: if we traverse all the live closures,
            // including static closures, during each heap census then
            // we will observe that some static closures appear and
            // disappear.  eg. a closure may contain a pointer to a
            // static function 'f' which is not otherwise reachable
            // (it doesn't indirectly point to any CAFs, so it doesn't
            // appear in any SRTs), so we would find 'f' during
            // traversal.  However on the next sweep there may be no
            // closures pointing to 'f'.
            //
            // We must therefore ignore static closures whose SRT is
            // empty, because these are exactly the closures that may
            // "appear".  A closure with a non-empty SRT, and which is
            // still required, will always be reachable.
            //
            // But what about CONSTR?  Surely these may be able
            // to appear, and they don't have SRTs, so we can't
            // check.  So for now, we're calling
            // resetStaticObjectForProfiling() from the
            // garbage collector to reset the retainer sets in all the
            // reachable static objects.
            goto loop;
        }

    case FUN_STATIC: {
        StgInfoTable *info = get_itbl(c);
        if (info->srt == 0 && info->layout.payload.ptrs == 0) {
            goto loop;
        } else {
            break;
        }
    }

    default:
        break;
    }

    // If this is the first visit to c, initialize its data.
    traverseMaybeInitClosureData(c);

    if(visit_cb(c, cp, data, (stackData*)&child_data))
        goto loop;

    // process child

    // Special case closures: we process these all in one go rather
    // than attempting to save the current position, because doing so
    // would be hard.
    switch (typeOfc) {
    case STACK:
        traversePushStack(ts, c, child_data,
                    ((StgStack *)c)->sp,
                    ((StgStack *)c)->stack + ((StgStack *)c)->stack_size);
        goto loop;

    case TSO:
    {
        StgTSO *tso = (StgTSO *)c;

        traversePushClosure(ts, (StgClosure *) tso->stackobj, c, child_data);
        traversePushClosure(ts, (StgClosure *) tso->blocked_exceptions, c, child_data);
        traversePushClosure(ts, (StgClosure *) tso->bq, c, child_data);
        traversePushClosure(ts, (StgClosure *) tso->trec, c, child_data);
        if (   tso->why_blocked == BlockedOnMVar
               || tso->why_blocked == BlockedOnMVarRead
               || tso->why_blocked == BlockedOnBlackHole
               || tso->why_blocked == BlockedOnMsgThrowTo
            ) {
            traversePushClosure(ts, tso->block_info.closure, c, child_data);
        }
        goto loop;
    }

    case BLOCKING_QUEUE:
    {
        StgBlockingQueue *bq = (StgBlockingQueue *)c;
        traversePushClosure(ts, (StgClosure *) bq->link,  c, child_data);
        traversePushClosure(ts, (StgClosure *) bq->bh,    c, child_data);
        traversePushClosure(ts, (StgClosure *) bq->owner, c, child_data);
        goto loop;
    }

    case PAP:
    {
        StgPAP *pap = (StgPAP *)c;
        traversePAP(ts, c, child_data, pap->fun, pap->payload, pap->n_args);
        goto loop;
    }

    case AP:
    {
        StgAP *ap = (StgAP *)c;
        traversePAP(ts, c, child_data, ap->fun, ap->payload, ap->n_args);
        goto loop;
    }

    case AP_STACK:
        traversePushClosure(ts, ((StgAP_STACK *)c)->fun, c, child_data);
        traversePushStack(ts, c, child_data,
                    (StgPtr)((StgAP_STACK *)c)->payload,
                    (StgPtr)((StgAP_STACK *)c)->payload +
                             ((StgAP_STACK *)c)->size);
        goto loop;
    }

    traversePushChildren(ts, c, child_data, &first_child);

    // If first_child is null, c has no child.
    // If first_child is not null, the top stack element points to the next
    // object. push() may or may not push a stackElement on the stack.
    if (first_child == NULL)
        goto loop;

    // (c, cp, data) = (first_child, c, child_data)
    data = child_data;
    cp = c;
    c = first_child;
    goto inner_loop;
}

/* -----------------------------------------------------------------------------
 *  Compute the retainer set for every object reachable from *tl.
 * -------------------------------------------------------------------------- */
static void
retainRoot(void *user, StgClosure **tl)
{
    traverseState *ts = (traverseState*) user;
    StgClosure *c;

    // We no longer assume that only TSOs and WEAKs are roots; any closure can
    // be a root.

    ASSERT(isEmptyWorkStack(ts));
    ts->currentStackBoundary = ts->stackTop;

    c = UNTAG_CLOSURE(*tl);
    traverseMaybeInitClosureData(c);
    if (c != &stg_END_TSO_QUEUE_closure && isRetainer(c)) {
        traversePushClosure(ts, c, c, (stackData)getRetainerFrom(c));
    } else {
        traversePushClosure(ts, c, c, (stackData)CCS_SYSTEM);
    }
    traverseWorkStack(ts, &retainVisitClosure);

    // NOT TRUE: ASSERT(isMember(getRetainerFrom(*tl), retainerSetOf(*tl)));
    // *tl might be a TSO which is ThreadComplete, in which
    // case we ignore it for the purposes of retainer profiling.
}

/* -----------------------------------------------------------------------------
 *  Compute the retainer set for each of the objects in the heap.
 * -------------------------------------------------------------------------- */
static void
computeRetainerSet( traverseState *ts )
{
    StgWeak *weak;
    uint32_t g, n;
    StgPtr ml;
    bdescr *bd;

    markCapabilities(retainRoot, (void*)ts); // for scheduler roots

    // This function is called after a major GC, when key, value, and finalizer
    // all are guaranteed to be valid, or reachable.
    //
    // The following code assumes that WEAK objects are considered to be roots
    // for retainer profilng.
    for (n = 0; n < n_capabilities; n++) {
        // NB: after a GC, all nursery weak_ptr_lists have been migrated
        // to the global lists living in the generations
        ASSERT(capabilities[n]->weak_ptr_list_hd == NULL);
        ASSERT(capabilities[n]->weak_ptr_list_tl == NULL);
    }
    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
        for (weak = generations[g].weak_ptr_list; weak != NULL; weak = weak->link) {
            // retainRoot((StgClosure *)weak);
            retainRoot((void*)ts, (StgClosure **)&weak);
        }
    }

    // Consider roots from the stable ptr table.
    markStablePtrTable(retainRoot, (void*)ts);
    // Remember old stable name addresses.
    rememberOldStableNameAddresses ();

    // The following code resets the rs field of each unvisited mutable
    // object.
    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
        // NOT true: even G0 has a block on its mutable list
        // ASSERT(g != 0 || (generations[g].mut_list == NULL));

        // Traversing through mut_list is necessary
        // because we can find MUT_VAR objects which have not been
        // visited during retainer profiling.
        for (n = 0; n < n_capabilities; n++) {
          for (bd = capabilities[n]->mut_lists[g]; bd != NULL; bd = bd->link) {
            for (ml = bd->start; ml < bd->free; ml++) {

                traverseMaybeInitClosureData((StgClosure *)*ml);
            }
          }
        }
    }
}

/* -----------------------------------------------------------------------------
 *  Traverse all static objects for which we compute retainer sets,
 *  and reset their rs fields to NULL, which is accomplished by
 *  invoking traverseMaybeInitClosureData(). This function must be called
 *  before zeroing all objects reachable from scavenged_static_objects
 *  in the case of major garbage collections. See GarbageCollect() in
 *  GC.c.
 *  Note:
 *    The mut_once_list of the oldest generation must also be traversed?
 *    Why? Because if the evacuation of an object pointed to by a static
 *    indirection object fails, it is put back to the mut_once_list of
 *    the oldest generation.
 *    However, this is not necessary because any static indirection objects
 *    are just traversed through to reach dynamic objects. In other words,
 *    they are not taken into consideration in computing retainer sets.
 *
 * SDM (20/7/2011): I don't think this is doing anything sensible,
 * because it happens before retainerProfile() and at the beginning of
 * retainerProfil() we change the sense of 'flip'.  So all of the
 * calls to traverseMaybeInitClosureData() here are initialising retainer sets
 * with the wrong flip.  Also, I don't see why this is necessary.  I
 * added a traverseMaybeInitClosureData() call to retainRoot(), and that seems
 * to have fixed the assertion failure in retainerSetOf() I was
 * encountering.
 * -------------------------------------------------------------------------- */
void
resetStaticObjectForProfiling( StgClosure *static_objects )
{
#if defined(DEBUG_RETAINER)
    uint32_t count;
#endif
    StgClosure *p;

#if defined(DEBUG_RETAINER)
    count = 0;
#endif
    p = static_objects;
    while (p != END_OF_STATIC_OBJECT_LIST) {
        p = UNTAG_STATIC_LIST_PTR(p);
#if defined(DEBUG_RETAINER)
        count++;
#endif
        switch (get_itbl(p)->type) {
        case IND_STATIC:
            // Since we do not compute the retainer set of any
            // IND_STATIC object, we don't have to reset its retainer
            // field.
            p = (StgClosure*)*IND_STATIC_LINK(p);
            break;
        case THUNK_STATIC:
            traverseMaybeInitClosureData(p);
            p = (StgClosure*)*THUNK_STATIC_LINK(p);
            break;
        case FUN_STATIC:
        case CONSTR:
        case CONSTR_1_0:
        case CONSTR_2_0:
        case CONSTR_1_1:
        case CONSTR_NOCAF:
            traverseMaybeInitClosureData(p);
            p = (StgClosure*)*STATIC_LINK(get_itbl(p), p);
            break;
        default:
            barf("resetStaticObjectForProfiling: %p (%s)",
                 p, get_itbl(p)->type);
            break;
        }
    }
#if defined(DEBUG_RETAINER)
    debugBelch("count in scavenged_static_objects = %d\n", count);
#endif
}

/* -----------------------------------------------------------------------------
 * Perform retainer profiling.
 * N is the oldest generation being profilied, where the generations are
 * numbered starting at 0.
 * Invariants:
 * Note:
 *   This function should be called only immediately after major garbage
 *   collection.
 * ------------------------------------------------------------------------- */
void
retainerProfile(void)
{
  stat_startRP();

  // Now we flips flip.
  flip = flip ^ 1;

#if defined(DEBUG_RETAINER)
  g_retainerTraverseState.stackSize = 0;
  g_retainerTraverseState.maxStackSize = 0;
#endif
  numObjectVisited = 0;
  timesAnyObjectVisited = 0;

  /*
    We initialize the traverse stack each time the retainer profiling is
    performed (because the traverse stack size varies on each retainer profiling
    and this operation is not costly anyhow). However, we just refresh the
    retainer sets.
   */
  initializeTraverseStack(&g_retainerTraverseState);
  initializeAllRetainerSet();
  computeRetainerSet(&g_retainerTraverseState);

  // post-processing
  closeTraverseStack(&g_retainerTraverseState);
  retainerGeneration++;

  stat_endRP(
    retainerGeneration - 1,   // retainerGeneration has just been incremented!
#if defined(DEBUG_RETAINER)
    g_retainerTraverseState.maxStackSize,
#endif
    (double)timesAnyObjectVisited / numObjectVisited);
}

#endif /* PROFILING */
