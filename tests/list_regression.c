/* libxutils: bidirectional links, detached ownership and ring cleanup. */
#include "test.h"
#include "list.h"

static void XTest_Clear(void *pContext, void *pData)
{
    (void)pData;
    (*(int*)pContext)++;
}

static int XTest_Find(void *pContext, xlist_t *pNode)
{
    return pNode->data.pData == pContext;
}

static int XTest_links(void)
{
    int values[4] = {0, 1, 2, 3}, nCleared = 0;
    xlist_t *pHead = XList_New(&values[0], 0, XTest_Clear, &nCleared);
    CHECK(pHead != NULL, "Allocate list head");
    for (int i = 1; i < 4; i++) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "Append node");
    xlist_t *pTail = XList_GetTail(pHead), *pNode = pHead;
    for (int i = 0; i < 4; i++, pNode = pNode->pNext)
    {
        CHECK(pNode && pNode->data.pData == &values[i], "Forward traversal retains order");
        CHECK(!i || pNode->pPrev->pNext == pNode, "Backward link agrees with forward link");
    }
    CHECK(pNode == NULL && XList_GetHead(pTail) == pHead, "Locate both ends from either direction");
    pNode = XList_Search(pTail, &values[2], XTest_Find);
    CHECK(pNode != NULL, "Search from the tail visits the whole list");
    XList_Detach(pNode);
    CHECK(!pNode->pNext && !pNode->pPrev && nCleared == 0, "Detach transfers ownership without cleanup");
    XList_Free(pNode);
    CHECK(nCleared == 1 && pHead->pNext->pNext == pTail, "Detached node cleanup leaves neighbors linked");
    pHead = XList_RemoveHead(pHead);
    CHECK(nCleared == 2 && pHead->data.pData == &values[1], "Remove head returns the surviving list");
    XList_Clear(pHead);
    CHECK(nCleared == 4, "Clear visits each remaining node once");
    return 0;
}

static int XTest_rings(void)
{
    for (int nCount = 1; nCount <= 8; nCount++)
    {
        int values[8] = {0}, nCleared = 0;
        xlist_t *pHead = XList_New(&values[0], 0, XTest_Clear, &nCleared);
        CHECK(pHead != NULL, "Allocate ring head");
        for (int i = 1; i < nCount; i++) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "Append ring node");
        CHECK(!XList_IsRing(pHead), "A linear list is not a ring");
        CHECK(XList_MakeRing(pHead) == pHead && XList_IsRing(pHead), "Join both ends into a ring");
        XList_Clear(pHead);
        CHECK(nCleared == nCount, "Ring cleanup must terminate and free each node exactly once");
    }
    return 0;
}

static int XTest_stack_head(void)
{
    int value = 1, nCleared = 0;
    xlist_t head;
    XList_Init(&head, &value, 0, XTest_Clear, &nCleared);
    CHECK(XList_PushNext(&head, &value, 0) != NULL, "Mix a stack head with heap nodes");
    XList_Clear(&head);
    CHECK(nCleared == 2 && !head.pNext && !head.pPrev && !head.data.pData, "Clear resets but never frees a stack head");
    XList_Clear(&head);
    CHECK(nCleared == 2, "Repeated clear is harmless");
    return 0;
}


/* Matches a node whose payload pointer is the one being looked for. */
static int XTest_MatchPtr(void *pUserPtr, xlist_t *pNode)
{
    return pNode->data.pData == pUserPtr;
}

static int XTest_push(void)
{
    /* Pushing to the front and the back builds the list in both
     * directions from one anchor node. */
    int values[5] = {0, 1, 2, 3, 4};
    xlist_t *pAnchor = XList_New(&values[2], 0, NULL, NULL);
    CHECK(pAnchor != NULL, "The anchor node is created");

    CHECK(XList_PushPrev(pAnchor, &values[1], 0) != NULL, "A node is pushed before the anchor");
    CHECK(XList_PushNext(pAnchor, &values[3], 0) != NULL, "A node is pushed after the anchor");

    xlist_t *pHead = XList_GetHead(pAnchor);
    xlist_t *pTail = XList_GetTail(pAnchor);
    CHECK(pHead != NULL && pHead->data.pData == &values[1], "The head is the node pushed before");
    CHECK(pTail != NULL && pTail->data.pData == &values[3], "The tail is the node pushed after");

    CHECK(XList_PushFront(pAnchor, &values[0], 0) != NULL, "A node is pushed to the front");
    CHECK(XList_PushBack(pAnchor, &values[4], 0) != NULL, "A node is pushed to the back");

    pHead = XList_GetHead(pAnchor);
    pTail = XList_GetTail(pAnchor);
    CHECK(pHead->data.pData == &values[0], "The front push reached the head");
    CHECK(pTail->data.pData == &values[4], "The back push reached the tail");

    /* Walking forward sees every value in order. */
    int nSeen = 0;
    for (xlist_t *pNode = pHead; pNode != NULL; pNode = pNode->pNext)
    {
        CHECK(pNode->data.pData == &values[nSeen], "Each node holds the value pushed into its position");
        nSeen++;
    }
    CHECK(nSeen == 5, "Every pushed node is on the list");

    /* Walking backward sees the same values in reverse. */
    nSeen = 5;
    for (xlist_t *pNode = pTail; pNode != NULL; pNode = pNode->pPrev)
    {
        nSeen--;
        CHECK(pNode->data.pData == &values[nSeen], "The reverse walk sees the same order backwards");
    }
    CHECK(nSeen == 0, "The reverse walk reaches the head");

    XList_Clear(pHead);
    return 0;
}

static int XTest_removal(void)
{
    /* Unlink both detaches and frees the node, and returns a neighbour to
     * carry on from. RemoveHead, RemoveTail and Remove are all built on it,
     * so none of them hands the caller a node to free. */
    int values[4] = {10, 20, 30, 40};
    int nCleared = 0;

    xlist_t *pHead = XList_New(&values[0], sizeof(int), XTest_Clear, &nCleared);
    CHECK(pHead != NULL, "The list is created");
    for (int i = 1; i < 4; i++)
        CHECK(XList_PushBack(pHead, &values[i], sizeof(int)) != NULL, "A node is appended");

    CHECK(XList_GetHead(pHead)->data.pData == &values[0], "The head is the first node pushed");

    xlist_t *pRest = XList_RemoveHead(pHead);
    CHECK(pRest != NULL, "Removing the head leaves a list behind");
    CHECK(nCleared == 1, "The removed head's payload was released once");
    CHECK(XList_GetHead(pRest)->data.pData == &values[1], "The second node became the head");
    CHECK(XList_Search(XList_GetHead(pRest), &values[0], XTest_MatchPtr) == NULL,
        "The removed head is off the list");

    CHECK(XList_GetTail(pRest)->data.pData == &values[3], "The tail is the last node pushed");

    pRest = XList_RemoveTail(pRest);
    CHECK(pRest != NULL, "Removing the tail leaves a list behind");
    CHECK(nCleared == 2, "The removed tail's payload was released once");
    CHECK(XList_GetTail(XList_GetHead(pRest))->data.pData == &values[2], "The list ends one node earlier");

    XList_Clear(XList_GetHead(pRest));
    CHECK(nCleared == 4, "Clearing released every remaining payload exactly once");

    /* Unlinking a middle node closes the gap around it. */
    int trio[3] = {1, 2, 3};
    xlist_t *pFirst = XList_New(&trio[0], 0, NULL, NULL);
    CHECK(pFirst != NULL, "A three node list is created");
    for (int i = 1; i < 3; i++) CHECK(XList_PushBack(pFirst, &trio[i], 0) != NULL, "A node is appended");

    xlist_t *pMiddle = XList_Search(pFirst, &trio[1], XTest_MatchPtr);
    CHECK(pMiddle != NULL, "The middle node is found");

    xlist_t *pNeighbour = XList_Unlink(pMiddle);
    CHECK(pNeighbour != NULL, "Unlinking reports a neighbour to carry on from");

    xlist_t *pRemaining = XList_GetHead(pNeighbour);
    CHECK(XList_Search(pRemaining, &trio[1], XTest_MatchPtr) == NULL, "The unlinked node is off the list");
    CHECK(XList_Search(pRemaining, &trio[0], XTest_MatchPtr) != NULL, "The node before it stayed");
    CHECK(XList_Search(pRemaining, &trio[2], XTest_MatchPtr) != NULL, "The node after it stayed");

    /* Removing by predicate takes the matching node off the list. */
    CHECK(XList_Remove(pRemaining, &trio[2], XTest_MatchPtr) != NULL, "The predicate removal reports a neighbour");

    /* A predicate that matches nothing removes nothing. */
    int nAbsent = 999;
    pRemaining = XList_GetHead(pFirst);
    CHECK(XList_Remove(pRemaining, &nAbsent, XTest_MatchPtr) == NULL, "A predicate matching nothing removes nothing");
    CHECK(XList_Search(pRemaining, &nAbsent, XTest_MatchPtr) == NULL, "A predicate matching nothing finds nothing");

    XList_Clear(pRemaining);
    return 0;
}

static int XTest_single_node(void)
{
    /* A list of one is its own head and tail, and removing from it leaves
     * nothing to carry on from. */
    int value = 7;
    xlist_t *pOnly = XList_New(&value, 0, NULL, NULL);
    CHECK(pOnly != NULL, "A single node list is created");
    CHECK(XList_GetHead(pOnly) == pOnly, "The only node is the head");
    CHECK(XList_GetTail(pOnly) == pOnly, "The only node is the tail");
    CHECK(pOnly->pNext == NULL && pOnly->pPrev == NULL, "The only node has no neighbours");
    CHECK(XList_IsRing(pOnly) == 0, "A single node is not a ring by itself");

    CHECK(XList_Search(pOnly, &value, XTest_MatchPtr) == pOnly, "The only node is findable");
    CHECK(XList_Unlink(pOnly) == NULL, "Unlinking the only node reports no neighbour and frees it");

    /* Detaching leaves the node standing alone without freeing it. */
    int pair[2] = {1, 2};
    xlist_t *pFirst = XList_New(&pair[0], 0, NULL, NULL);
    CHECK(pFirst != NULL, "A two node list is created");
    xlist_t *pSecond = XList_PushBack(pFirst, &pair[1], 0);
    CHECK(pSecond != NULL, "The second node is appended");

    XList_Detach(pSecond);
    CHECK(pSecond->pNext == NULL && pSecond->pPrev == NULL, "The detached node has no neighbours");
    CHECK(pFirst->pNext == NULL, "The list it came from no longer points at it");
    XList_Free(pSecond);
    XList_Free(pFirst);
    return 0;
}

static int XTest_insert_nodes(void)
{
    /* The node-taking inserts place an existing node rather than making
     * one, so the caller keeps whatever payload it set up. */
    int values[4] = {0, 1, 2, 3};
    xlist_t *pAnchor = XList_New(&values[1], 0, NULL, NULL);
    CHECK(pAnchor != NULL, "The anchor node is created");

    xlist_t *pBefore = XList_New(&values[0], 0, NULL, NULL);
    xlist_t *pAfter = XList_New(&values[2], 0, NULL, NULL);
    CHECK(pBefore != NULL && pAfter != NULL, "The nodes to insert are created");

    CHECK(XList_InsertPrev(pAnchor, pBefore) == pBefore, "The node is inserted before the anchor");
    CHECK(XList_InsertNext(pAnchor, pAfter) == pAfter, "The node is inserted after the anchor");
    CHECK(pAnchor->pPrev == pBefore && pAnchor->pNext == pAfter, "The anchor sits between the two");

    xlist_t *pNewHead = XList_New(&values[3], 0, NULL, NULL);
    CHECK(pNewHead != NULL, "The head node is created");
    CHECK(XList_InsertHead(pAnchor, pNewHead) == pNewHead, "The node is inserted at the head");
    CHECK(XList_GetHead(pAnchor) == pNewHead, "The inserted node became the head");

    xlist_t *pNewTail = XList_New(&values[3], 0, NULL, NULL);
    CHECK(pNewTail != NULL, "The tail node is created");
    CHECK(XList_InsertTail(pAnchor, pNewTail) == pNewTail, "The node is inserted at the tail");
    CHECK(XList_GetTail(pAnchor) == pNewTail, "The inserted node became the tail");

    XList_Clear(XList_GetHead(pAnchor));
    return 0;
}

static int XTest_ring_walk(void)
{
    /* A ring has no end, so the walk has to stop when it comes back round
     * rather than on a null pointer. */
    int values[4] = {0, 1, 2, 3};
    xlist_t *pHead = XList_New(&values[0], 0, NULL, NULL);
    CHECK(pHead != NULL, "The list is created");
    for (int i = 1; i < 4; i++) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "A node is appended");

    CHECK(XList_IsRing(pHead) == 0, "A plain list is not a ring");
    CHECK(XList_MakeRing(pHead) != NULL, "The list is closed into a ring");
    CHECK(XList_IsRing(pHead) != 0, "The closed list reports itself a ring");

    /* Every node in the ring is reachable from every other. */
    int nSteps = 0;
    xlist_t *pNode = pHead;
    do
    {
        CHECK(pNode != NULL, "A ring never walks off the end");
        CHECK(pNode->data.pData == &values[nSteps], "The ring keeps its insertion order");
        pNode = pNode->pNext;
        nSteps++;
    }
    while (pNode != pHead && nSteps < 10);
    CHECK(nSteps == 4, "The ring comes back round after exactly its length");

    /* A ring has neither a head nor a tail, so both walks have to stop when
     * they come back round rather than circling forever. */
    CHECK(XList_GetHead(pHead) == pHead, "The head walk terminates on a ring at its starting node");
    CHECK(XList_GetTail(pHead) == pHead, "The tail walk terminates on a ring at its starting node");

    xlist_t *pThird = pHead->pNext->pNext;
    CHECK(XList_GetHead(pThird) == pThird, "The head walk terminates from any node in the ring");
    CHECK(XList_GetTail(pThird) == pThird, "The tail walk terminates from any node in the ring");

    XList_Clear(pHead);
    return 0;
}

/* Counts the nodes a forward walk meets, stopping a ring where it began. */
static int XTest_Count(xlist_t *pStart)
{
    int nCount = 0;
    xlist_t *pNode = pStart;
    while (pNode != NULL && nCount < 100)
    {
        nCount++;
        pNode = pNode->pNext;
        if (pNode == pStart) break;
    }
    return nCount;
}

/* Every link has to agree with the one pointing back at it. */
static int XTest_Consistent(xlist_t *pStart)
{
    xlist_t *pNode = pStart;
    for (int i = 0; pNode != NULL && i < 100; i++)
    {
        if (pNode->pNext != NULL && pNode->pNext->pPrev != pNode) return 0;
        if (pNode->pPrev != NULL && pNode->pPrev->pNext != pNode) return 0;
        pNode = pNode->pNext;
        if (pNode == pStart) break;
    }
    return 1;
}

static int XTest_Never(void *pContext, xlist_t *pNode)
{
    (void)pNode;
    (*(int*)pContext)++;
    return 0;
}

static int XTest_ring_search(void)
{
    /* A search that matched nothing went round a ring forever. */
    int values[5] = {0, 1, 2, 3, 4}, nVisited = 0;
    xlist_t *pHead = XList_New(&values[0], 0, NULL, NULL);
    CHECK(pHead != NULL, "The list is created");
    for (int i = 1; i < 5; i++) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "A node is appended");
    CHECK(XList_MakeRing(pHead) == pHead, "The list is closed into a ring");

    CHECK(XList_Search(pHead, &nVisited, XTest_Never) == NULL, "A search of a ring that matches nothing ends");
    CHECK(nVisited == 5, "And visits every node of the ring once");

    xlist_t *pThird = pHead->pNext->pNext;
    nVisited = 0;
    CHECK(XList_Search(pThird, &nVisited, XTest_Never) == NULL && nVisited == 5, "From any node of the ring");
    CHECK(XList_Search(pThird, &values[1], XTest_MatchPtr) == pHead->pNext, "A match behind the start is found");

    /* A ring of one node is its own predecessor */
    xlist_t *pOnly = XList_New(&values[0], 0, NULL, NULL);
    CHECK(pOnly != NULL && XList_MakeRing(pOnly) == pOnly && pOnly->pNext == pOnly, "A ring of one node");
    nVisited = 0;
    CHECK(XList_Search(pOnly, &nVisited, XTest_Never) == NULL && nVisited == 1, "Is searched once");

    XList_Clear(pOnly);
    XList_Clear(pHead);
    return 0;
}

static int XTest_ring_insert(void)
{
    /* Inserting at the head or the tail of a ring cut it open on one side,
       and the next walk from the new node never came back to it. */
    int values[8] = {0}, nCleared = 0;
    xlist_t *pHead = XList_New(&values[0], 0, XTest_Clear, &nCleared);
    CHECK(pHead != NULL, "The list is created");
    for (int i = 1; i < 3; i++) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "A node is appended");
    CHECK(XList_MakeRing(pHead) == pHead, "The list is closed into a ring");

    /* Closing a ring again joined its head to itself and lost the rest */
    CHECK(XList_MakeRing(pHead->pNext) == pHead->pNext, "Closing a ring again reports where it started");
    CHECK(XTest_Count(pHead) == 3 && XTest_Consistent(pHead), "And leaves the ring as it was");

    xlist_t *pFront = XList_PushFront(pHead, &values[3], 0);
    CHECK(pFront != NULL && pFront->data.pData == &values[3], "A node is pushed to the front of a ring");
    xlist_t *pBack = XList_PushBack(pHead, &values[4], 0);
    CHECK(pBack != NULL && pBack->data.pData == &values[4], "A node is pushed to the back of a ring");

    xlist_t *pNode = XList_New(&values[5], 0, NULL, NULL);
    CHECK(XList_InsertHead(pHead, pNode) == pNode, "A node is inserted at the head of a ring");
    pNode = XList_New(&values[6], 0, NULL, NULL);
    CHECK(XList_InsertTail(pHead, pNode) == pNode, "A node is inserted at the tail of a ring");

    CHECK(XList_IsRing(pHead) && XList_IsRing(pFront) && XList_IsRing(pNode), "The ring stays closed");
    CHECK(XTest_Count(pHead) == 7 && XTest_Count(pNode) == 7, "Every node is on the ring once");
    CHECK(XTest_Consistent(pHead), "Every link agrees with the one pointing back");
    CHECK(XList_GetHead(pFront) == pFront && XList_GetTail(pBack) == pBack, "Both walks end from the new nodes");

    XList_Clear(pHead);
    CHECK(nCleared == 7, "Every node of the ring is released once");
    return 0;
}

static int XTest_relink(void)
{
    /* A node inserted while still linked elsewhere is moved. Its old
       neighbours kept pointing at it, and inserting a node next to itself
       linked it to itself. */
    int values[4] = {0, 1, 2, 3};
    xlist_t *pHead = XList_New(&values[0], 0, NULL, NULL);
    CHECK(pHead != NULL, "The list is created");
    xlist_t *pNodes[4] = {pHead};
    for (int i = 1; i < 4; i++)
    {
        CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "A node is appended");
        pNodes[i] = XList_GetTail(pHead);
    }

    CHECK(XList_InsertNext(pNodes[0], pNodes[3]) == pNodes[1], "The tail is moved after the head");
    CHECK(pHead->pNext == pNodes[3] && pNodes[3]->pNext == pNodes[1] && pNodes[2]->pNext == NULL, "In its new place");
    CHECK(XTest_Count(pHead) == 4 && XTest_Consistent(pHead), "Nothing is lost or linked twice");

    CHECK(XList_InsertPrev(pNodes[0], pNodes[2]) == pNodes[2], "The tail is moved in front of the head");
    CHECK(XList_GetHead(pHead) == pNodes[2] && XList_GetTail(pHead) == pNodes[1], "The ends follow the move");
    CHECK(XTest_Count(pNodes[2]) == 4 && XTest_Consistent(pNodes[2]), "Nothing is lost or linked twice");

    CHECK(XList_InsertTail(pNodes[3], pNodes[2]) == pNodes[2], "The head is moved to the tail");
    CHECK(XList_InsertHead(pNodes[1], pNodes[0]) == pNodes[0], "A node already at the head stays there");
    CHECK(XList_GetHead(pNodes[3]) == pNodes[0] && XList_GetTail(pNodes[3]) == pNodes[2], "The ends follow the moves");
    CHECK(XTest_Count(pNodes[0]) == 4 && XTest_Consistent(pNodes[0]), "Nothing is lost or linked twice");

    CHECK(XList_InsertPrev(pNodes[1], pNodes[1]) == NULL && XList_InsertNext(pNodes[1], pNodes[1]) == NULL,
        "A node is not inserted next to itself");
    CHECK(XList_InsertHead(pNodes[1], pNodes[1]) == NULL && XList_InsertTail(pNodes[1], pNodes[1]) == NULL,
        "Nor at an end of its own list relative to itself");
    CHECK(XTest_Count(pNodes[0]) == 4 && XTest_Consistent(pNodes[0]), "A refused insertion changes nothing");

    XList_Clear(pNodes[0]);
    return 0;
}

static int g_nClearA, g_nClearB;
static void XTest_ClearA(void *pContext, void *pData) { (void)pData; if (pContext == &g_nClearB) g_nClearA++; }
static void XTest_ClearB(void *pContext, void *pData) { (void)pData; if (pContext == &g_nClearB) g_nClearB++; }

static int XTest_merge_ctx(void)
{
    /* A node inherits whichever of the callback and its context it is missing
       from the node it is put next to, each one on its own */
    g_nClearA = g_nClearB = 0;
    int value = 1;
    xlist_t *pHead = XList_New(&value, 0, XTest_ClearB, &g_nClearB);
    CHECK(pHead != NULL, "The list is created");

    xlist_t *pNone = XList_New(&value, 0, NULL, NULL);
    xlist_t *pOwn = XList_New(&value, 0, XTest_ClearA, NULL);
    xlist_t *pCtxOnly = XList_New(&value, 0, NULL, &g_nClearB);
    CHECK(pOwn != NULL && pCtxOnly != NULL && pNone != NULL, "The nodes are created");

    CHECK(XList_InsertTail(pHead, pNone) == pNone, "A node with neither is inserted");
    CHECK(pNone->data.onClear == XTest_ClearB && pNone->data.pClearCtx == &g_nClearB, "It inherits both");
    CHECK(XList_InsertTail(pHead, pOwn) == pOwn, "A node with its own callback is inserted");
    CHECK(pOwn->data.onClear == XTest_ClearA && pOwn->data.pClearCtx == &g_nClearB, "It keeps its callback, takes the context");
    CHECK(XList_InsertTail(pHead, pCtxOnly) == pCtxOnly, "A node with only a context is inserted");
    CHECK(pCtxOnly->data.onClear == XTest_ClearA && pCtxOnly->data.pClearCtx == &g_nClearB, "It takes the callback");

    xlist_data_t data;
    XListData_Init(&data, NULL, 0, NULL, NULL);
    XListData_MergeCtx(&data, NULL);
    CHECK(data.onClear == NULL && data.pClearCtx == NULL, "There is nothing to inherit from no node");

    XList_Clear(pHead);
    CHECK(g_nClearA == 2 && g_nClearB == 2, "Each node was released through the callback it ended up with");
    return 0;
}

static int XTest_UnlinkEven(void *pContext, xlist_t *pNode)
{
    int *pValue = (int*)pNode->data.pData;
    if (*pValue % 2 == 0)
    {
        (*(int*)pContext)++;
        XList_Unlink(pNode);
    }
    return 0;
}

static int XTest_Stop(void *pContext, xlist_t *pNode)
{
    (void)pNode;
    (*(int*)pContext)++;
    return -1;
}

static int XTest_search_unlink(void)
{
    /* A callback may unlink the node it is given. The walk read the next
       link from that node after the callback had released it. */
    int values[6] = {0, 1, 2, 3, 4, 5}, nRemoved = 0;
    xlist_t *pHead = XList_New(&values[1], 0, NULL, NULL);
    CHECK(pHead != NULL, "The list is created");
    for (int i = 2; i < 6; i++) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "A node is appended");
    CHECK(XList_PushFront(pHead, &values[0], 0) != NULL, "A node is pushed to the front");

    CHECK(XList_Search(pHead, &nRemoved, XTest_UnlinkEven) == NULL && nRemoved == 3, "Every even node is unlinked");
    CHECK(XTest_Count(pHead) == 3 && XTest_Consistent(pHead), "The odd ones are still linked");

    /* And in a ring, where the walk ends at the node before its start */
    for (int i = 0; i < 6; i += 2) CHECK(XList_PushBack(pHead, &values[i], 0) != NULL, "An even node is appended");
    CHECK(XList_MakeRing(pHead) == pHead, "The list is closed into a ring");
    nRemoved = 0;
    CHECK(XList_Search(pHead, &nRemoved, XTest_UnlinkEven) == NULL && nRemoved == 3, "Every even node of the ring is unlinked");
    CHECK(XTest_Count(pHead) == 3 && XList_IsRing(pHead) && XTest_Consistent(pHead), "The rest of the ring is closed");

    /* A negative answer stops the walk at the node that gave it */
    int nVisited = 0;
    CHECK(XList_Search(pHead, &nVisited, XTest_Stop) == NULL && nVisited == 1, "A negative answer stops the search");
    CHECK(XList_Remove(pHead, &nVisited, XTest_Stop) == NULL && XTest_Count(pHead) == 3, "And removes nothing");

    XList_Clear(pHead);
    return 0;
}

static int XTest_api_guards(void)
{
    XListData_MergeCtx(NULL, NULL);
    XListData_Init(NULL, NULL, 0, NULL, NULL);
    XList_Init(NULL, NULL, 0, NULL, NULL);
    XList_Free(NULL);
    XList_Detach(NULL);
    CHECK(XList_Unlink(NULL) == NULL && XList_GetHead(NULL) == NULL && XList_GetTail(NULL) == NULL, "No list has no ends");
    CHECK(XList_MakeRing(NULL) == NULL && !XList_IsRing(NULL), "No list makes no ring");
    CHECK(XList_InsertPrev(NULL, NULL) == NULL && XList_InsertNext(NULL, NULL) == NULL, "No list takes no node");
    CHECK(XList_InsertHead(NULL, NULL) == NULL && XList_InsertTail(NULL, NULL) == NULL, "No list takes no node");
    CHECK(XList_PushPrev(NULL, NULL, 0) == NULL && XList_PushNext(NULL, NULL, 0) == NULL, "No list takes no data");
    CHECK(XList_PushFront(NULL, NULL, 0) == NULL && XList_PushBack(NULL, NULL, 0) == NULL, "No list takes no data");
    CHECK(XList_Search(NULL, NULL, XTest_MatchPtr) == NULL, "No list finds nothing");

    xlist_t head;
    XList_Init(&head, NULL, 0, NULL, NULL);
    CHECK(XList_InsertPrev(&head, NULL) == NULL && XList_InsertNext(&head, NULL) == NULL, "No node is inserted");
    CHECK(XList_InsertHead(&head, NULL) == NULL && XList_InsertTail(&head, NULL) == NULL, "No node is inserted");
    CHECK(XList_Search(&head, NULL, NULL) == NULL, "A search needs a comparator");

    /* Data with a size and no clear callback belongs to its node */
    char *pOwned = strdup("owned");
    xlist_t *pLast = XList_PushBack(&head, pOwned, strlen("owned") + 1);
    CHECK(pOwned != NULL && pLast != NULL && pLast->data.pData == pOwned, "Push owned data");

    /* A node put in front of one that has a predecessor goes between the two */
    xlist_t *pMiddle = XList_New(NULL, 0, NULL, NULL);
    CHECK(pMiddle != NULL && XList_InsertPrev(pLast, pMiddle) == &head, "The insertion reports the predecessor");
    CHECK(head.pNext == pMiddle && pMiddle->pPrev == &head, "The new node follows the predecessor");
    CHECK(pMiddle->pNext == pLast && pLast->pPrev == pMiddle, "And leads the node it was put in front of");

    XList_Clear(&head);
    CHECK(head.pNext == NULL && head.data.pData == NULL, "Clearing resets the head");
    return 0;
}

XTEST_MAIN(
    XTEST_CASE(api_guards),
    XTEST_CASE(links),
    XTEST_CASE(rings),
    XTEST_CASE(stack_head),
    XTEST_CASE(push),
    XTEST_CASE(removal),
    XTEST_CASE(single_node),
    XTEST_CASE(insert_nodes),
    XTEST_CASE(ring_walk),
    XTEST_CASE(ring_search),
    XTEST_CASE(ring_insert),
    XTEST_CASE(relink),
    XTEST_CASE(merge_ctx),
    XTEST_CASE(search_unlink)
)
