/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        network/router.c
 * PURPOSE:     IP routing subsystem
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 * NOTES:
 *   This file holds authoritative routing information.
 *   Information queries on the route table should be handled here.
 *   This information should always override the route cache info.
 * REVISIONS:
 *   CSH 01/08-2000 Created
 */

#include "precomp.h"


LIST_ENTRY FIBListHead;
KSPIN_LOCK FIBLock;


VOID FreeFIB(
    PVOID Object)
/*
 * FUNCTION: Frees an forward information base object
 * ARGUMENTS:
 *     Object = Pointer to an forward information base structure
 */
{
    PoolFreeBuffer(Object);
}


VOID DestroyFIBE(
    PFIB_ENTRY FIBE)
/*
 * FUNCTION: Destroys an forward information base entry
 * ARGUMENTS:
 *     FIBE = Pointer to FIB entry
 * NOTES:
 *     The forward information base lock must be held when called
 */
{
    TI_DbgPrint(DEBUG_ROUTER, ("Called. FIBE (0x%X).\n", FIBE));

    /* Unlink the FIB entry from the list */
    RemoveEntryList(&FIBE->ListEntry);

    /* And free the FIB entry */
    FreeFIB(FIBE);
}


VOID DestroyFIBEs(
    VOID)
/*
 * FUNCTION: Destroys all forward information base entries
 * NOTES:
 *     The forward information base lock must be held when called
 */
{
    PLIST_ENTRY CurrentEntry;
    PLIST_ENTRY NextEntry;
    PFIB_ENTRY Current;

    /* Search the list and remove every FIB entry we find */
    CurrentEntry = FIBListHead.Flink;
    while (CurrentEntry != &FIBListHead) {
        NextEntry = CurrentEntry->Flink;
	Current = CONTAINING_RECORD(CurrentEntry, FIB_ENTRY, ListEntry);
        /* Destroy the FIB entry */
        DestroyFIBE(Current);
        CurrentEntry = NextEntry;
    }
}


UINT CountFIBs() {
    UINT FibCount = 0;
    PLIST_ENTRY CurrentEntry;
    PLIST_ENTRY NextEntry;

    /* Search the list and remove every FIB entry we find */
    CurrentEntry = FIBListHead.Flink;
    while (CurrentEntry != &FIBListHead) {
        NextEntry = CurrentEntry->Flink;
        CurrentEntry = NextEntry;
	FibCount++;
    }

    return FibCount;
}


UINT CopyFIBs( PFIB_ENTRY Target ) {
    UINT FibCount = 0;
    PLIST_ENTRY CurrentEntry;
    PLIST_ENTRY NextEntry;
    PFIB_ENTRY Current;

    /* Search the list and remove every FIB entry we find */
    CurrentEntry = FIBListHead.Flink;
    while (CurrentEntry != &FIBListHead) {
        NextEntry = CurrentEntry->Flink;
	Current = CONTAINING_RECORD(CurrentEntry, FIB_ENTRY, ListEntry);
	Target[FibCount] = *Current;
        CurrentEntry = NextEntry;
	FibCount++;
    }

    return FibCount;    
}


UINT CommonPrefixLength(
    PIP_ADDRESS Address1,
    PIP_ADDRESS Address2)
/*
 * FUNCTION: Computes the length of the longest prefix common to two addresses
 * ARGUMENTS:
 *     Address1 = Pointer to first address
 *     Address2 = Pointer to second address
 * NOTES:
 *     The two addresses must be of the same type
 * RETURNS:
 *     Length of longest common prefix
 */
{
    PUCHAR Addr1, Addr2;
    UINT Size;
    UINT i, j;
    UINT Bitmask;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. Address1 (0x%X)  Address2 (0x%X).\n", Address1, Address2));

    /*TI_DbgPrint(DEBUG_ROUTER, ("Target  (%s) \n", A2S(Address1)));*/
    /*TI_DbgPrint(DEBUG_ROUTER, ("Adapter (%s).\n", A2S(Address2)));*/

    if (Address1->Type == IP_ADDRESS_V4)
        Size = sizeof(IPv4_RAW_ADDRESS);
    else
        Size = sizeof(IPv6_RAW_ADDRESS);

    Addr1 = (PUCHAR)&Address1->Address.IPv4Address;
    Addr2 = (PUCHAR)&Address2->Address.IPv4Address;

    /* Find first non-matching byte */
    for (i = 0; i < Size && Addr1[i] == Addr2[i]; i++);
    if( i == Size ) return 8 * i;

    /* Find first non-matching bit */
    Bitmask = 0x80;
    for (j = 0; (Addr1[i] & Bitmask) != (Addr2[i] & Bitmask); j++)
        Bitmask >>= 1;

    return 8 * i + j;
}


BOOLEAN HasPrefix(
    PIP_ADDRESS Address,
    PIP_ADDRESS Prefix,
    UINT Length)
/*
 * FUNCTION: Determines wether an address has an given prefix
 * ARGUMENTS:
 *     Address = Pointer to address to use
 *     Prefix  = Pointer to prefix to check for
 *     Length  = Length of prefix
 * RETURNS:
 *     TRUE if the address has the prefix, FALSE if not
 * NOTES:
 *     The two addresses must be of the same type
 */
{
    PUCHAR pAddress = (PUCHAR)&Address->Address;
    PUCHAR pPrefix  = (PUCHAR)&Prefix->Address;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. Address (0x%X)  Prefix (0x%X)  Length (%d).\n", Address, Prefix, Length));

#if 0
    TI_DbgPrint(DEBUG_ROUTER, ("Address (%s)  Prefix (%s).\n",
        A2S(Address), A2S(Prefix)));
#endif

    /* Check that initial integral bytes match */
    while (Length > 8) {
        if (*pAddress++ != *pPrefix++)
            return FALSE;
        Length -= 8;
    } 

    /* Check any remaining bits */
    if ((Length > 0) && ((*pAddress >> (8 - Length)) != (*pPrefix >> (8 - Length))))
        return FALSE;

    return TRUE;
}


PNET_TABLE_ENTRY RouterFindBestNTE(
    PIP_INTERFACE Interface,
    PIP_ADDRESS Destination)
/*
 * FUNCTION: Checks all on-link prefixes to find out if an address is on-link
 * ARGUMENTS:
 *     Interface   = Pointer to interface to use
 *     Destination = Pointer to destination address
 * NOTES:
 *     If found the NTE if referenced
 * RETURNS:
 *     Pointer to NTE if found, NULL if not
 */
{
    KIRQL OldIrql;
    PLIST_ENTRY CurrentEntry;
    PNET_TABLE_ENTRY Current;
    UINT Length, BestLength  = 0;
    PNET_TABLE_ENTRY BestNTE = NULL;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. Interface (0x%X)  Destination (0x%X).\n", Interface, Destination));

    TI_DbgPrint(DEBUG_ROUTER, ("Destination (%s).\n", A2S(Destination)));

    TcpipAcquireSpinLock(&Interface->Lock, &OldIrql);

    CurrentEntry = Interface->NTEListHead.Flink;
    while (CurrentEntry != &Interface->NTEListHead) {
	Current = CONTAINING_RECORD(CurrentEntry, NET_TABLE_ENTRY, IFListEntry);
	TI_DbgPrint(DEBUG_ROUTER, ("Looking at NTE %s\n", A2S(Current->Address)));

        Length = CommonPrefixLength(Destination, Current->Address);
        if (BestNTE) {
            if (Length > BestLength) {
                BestNTE    = Current;
                BestLength = Length;
            }
        } else {
            BestNTE    = Current;
            BestLength = Length;
        }
        CurrentEntry = CurrentEntry->Flink;
    }

    TcpipReleaseSpinLock(&Interface->Lock, OldIrql);

    return BestNTE;
}


PIP_INTERFACE RouterFindOnLinkInterface(
    PIP_ADDRESS Address,
    PNET_TABLE_ENTRY NTE)
/*
 * FUNCTION: Checks all on-link prefixes to find out if an address is on-link
 * ARGUMENTS:
 *     Address = Pointer to address to check
 *     NTE     = Pointer to NTE to check (NULL = check all interfaces)
 * RETURNS:
 *     Pointer to interface if address is on-link, NULL if not
 */
{
    PLIST_ENTRY CurrentEntry;
    PPREFIX_LIST_ENTRY Current;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. Address (0x%X)  NTE (0x%X).\n", Address, NTE));

    TI_DbgPrint(DEBUG_ROUTER, ("Address (%s)  NTE (%s).\n",
			       A2S(Address), NTE ? A2S(NTE->Address) : ""));

    CurrentEntry = PrefixListHead.Flink;
    while (CurrentEntry != &PrefixListHead) {
	    Current = CONTAINING_RECORD(CurrentEntry, PREFIX_LIST_ENTRY, ListEntry);

        if (HasPrefix(Address, Current->Prefix, Current->PrefixLength) &&
            ((!NTE) || (NTE->Interface == Current->Interface)))
            return Current->Interface;

        CurrentEntry = CurrentEntry->Flink;
    }

    return NULL;
}


PFIB_ENTRY RouterAddRoute(
    PIP_ADDRESS NetworkAddress,
    PIP_ADDRESS Netmask,
    PNEIGHBOR_CACHE_ENTRY Router,
    UINT Metric)
/*
 * FUNCTION: Adds a route to the Forward Information Base (FIB)
 * ARGUMENTS:
 *     NetworkAddress = Pointer to address of network
 *     Netmask        = Pointer to netmask of network
 *     Router         = Pointer to NCE of router to use
 *     Metric         = Cost of this route
 * RETURNS:
 *     Pointer to FIB entry if the route was added, NULL if not
 * NOTES:
 *     The FIB entry references the NetworkAddress, Netmask and
 *     the NCE of the router. The caller is responsible for providing
 *     these references
 */
{
    PFIB_ENTRY FIBE;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. NetworkAddress (0x%X)  Netmask (0x%X) "
        "Router (0x%X)  Metric (%d).\n", NetworkAddress, Netmask, Router, Metric));

    TI_DbgPrint(DEBUG_ROUTER, ("NetworkAddress (%s)  Netmask (%s)  Router (%s).\n",
			       A2S(NetworkAddress), 
			       A2S(Netmask), 
			       A2S(&Router->Address)));

    FIBE = PoolAllocateBuffer(sizeof(FIB_ENTRY));
    if (!FIBE) {
        TI_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
        return NULL;
    }

   INIT_TAG(Router, TAG('R','O','U','T'));

    FIBE->Free           = FreeFIB;
    FIBE->NetworkAddress = NetworkAddress;
    FIBE->Netmask        = Netmask;
    FIBE->Router         = Router;
    FIBE->Metric         = Metric;

    /* Add FIB to the forward information base */
    TcpipInterlockedInsertTailList(&FIBListHead, &FIBE->ListEntry, &FIBLock);

    return FIBE;
}


PNEIGHBOR_CACHE_ENTRY RouterGetRoute(
    PIP_ADDRESS Destination,
    PNET_TABLE_ENTRY NTE)
/*
 * FUNCTION: Finds a router to use to get to Destination
 * ARGUMENTS:
 *     Destination = Pointer to destination address (NULL means don't care)
 *     NTE         = Pointer to NTE describing net to send on
 *                   (NULL means don't care)
 * RETURNS:
 *     Pointer to NCE for router, NULL if none was found
 * NOTES:
 *     If found the NCE is referenced
 */
{
    KIRQL OldIrql;
    PLIST_ENTRY CurrentEntry;
    PLIST_ENTRY NextEntry;
    PFIB_ENTRY Current;
    UCHAR State, BestState = 0;
    UINT Length, BestLength = 0;
    PNEIGHBOR_CACHE_ENTRY NCE, BestNCE = NULL;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. Destination (0x%X)  NTE (0x%X).\n", Destination, NTE));

    TI_DbgPrint(DEBUG_ROUTER, ("Destination (%s)\n", A2S(Destination)));
    if( NTE )
	TI_DbgPrint(DEBUG_ROUTER, ("NTE (%s).\n", A2S(NTE->Address)));

    TcpipAcquireSpinLock(&FIBLock, &OldIrql);

    CurrentEntry = FIBListHead.Flink;
    while (CurrentEntry != &FIBListHead) {
        NextEntry = CurrentEntry->Flink;
	    Current = CONTAINING_RECORD(CurrentEntry, FIB_ENTRY, ListEntry);

        NCE   = Current->Router;
        State = NCE->State;

        if ((!NTE) || (NTE->Interface == NCE->Interface)) {
            if (Destination)
                Length = CommonPrefixLength(Destination, &NCE->Address);
            else
                Length = 0;

            if (BestNCE) {
                if ((State  > BestState)  || 
                    ((State == BestState) &&
                    (Length > BestLength))) {
                    /* This seems to be a better router */
                    BestNCE    = NCE;
                    BestLength = Length;
                    BestState  = State;
                }
            } else {
                /* First suitable router found, save it */
                BestNCE    = NCE;
                BestLength = Length;
                BestState  = State;
            }
        }
        CurrentEntry = NextEntry;
    }

    TcpipReleaseSpinLock(&FIBLock, OldIrql);

    return BestNCE;
}


VOID RouterRemoveRoute(
    PFIB_ENTRY FIBE)
/*
 * FUNCTION: Removes a route from the Forward Information Base (FIB)
 * ARGUMENTS:
 *     FIBE = Pointer to FIB entry describing route
 */
{
    KIRQL OldIrql;

    TI_DbgPrint(DEBUG_ROUTER, ("Called. FIBE (0x%X).\n", FIBE));

    TI_DbgPrint(DEBUG_ROUTER, ("FIBE (%s).\n", A2S(FIBE->NetworkAddress)));

    TcpipAcquireSpinLock(&FIBLock, &OldIrql);
    DestroyFIBE(FIBE);
    TcpipReleaseSpinLock(&FIBLock, OldIrql);
}


PFIB_ENTRY RouterCreateRouteIPv4(
    IPv4_RAW_ADDRESS NetworkAddress,
    IPv4_RAW_ADDRESS Netmask,
    IPv4_RAW_ADDRESS RouterAddress,
    PNET_TABLE_ENTRY NTE,
    UINT Metric)
/*
 * FUNCTION: Creates a route with IPv4 addresses as parameters
 * ARGUMENTS:
 *     NetworkAddress = Address of network
 *     Netmask        = Netmask of network
 *     RouterAddress  = Address of router to use
 *     NTE            = Pointer to NTE to use
 *     Metric         = Cost of this route
 * RETURNS:
 *     Pointer to FIB entry if the route was created, NULL if not.
 *     The FIB entry references the NTE. The caller is responsible
 *     for providing this reference
 */
{
    PIP_ADDRESS pNetworkAddress;
    PIP_ADDRESS pNetmask;
    PIP_ADDRESS pRouterAddress;
    PNEIGHBOR_CACHE_ENTRY NCE;
    PFIB_ENTRY FIBE;

    pNetworkAddress = AddrBuildIPv4(NetworkAddress);
    if (!pNetworkAddress) {
        TI_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
        return NULL;
    }

    pNetmask = AddrBuildIPv4(Netmask);
    if (!pNetmask) {
        TI_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
        return NULL;
    }

    pRouterAddress = AddrBuildIPv4(RouterAddress);
    if (!pRouterAddress) {
        TI_DbgPrint(MIN_TRACE, ("Insufficient resources.\n"));
        return NULL;
    }

    /* The NCE references RouterAddress. The NCE is referenced for us */
    NCE = NBAddNeighbor(NTE->Interface,
                        pRouterAddress,
                        NULL,
                        NTE->Interface->AddressLength,
                        NUD_PROBE);
    if (!NCE) {
        /* Not enough free resources */
        return NULL;
    }

    FIBE = RouterAddRoute(pNetworkAddress, pNetmask, NCE, 1);
    if (!FIBE) {
        /* Not enough free resources */
        NBRemoveNeighbor(NCE);

        (pNetworkAddress->Free)(pNetworkAddress);
        (pNetmask->Free)(pNetmask);
        (pRouterAddress->Free)(pRouterAddress);
    }

    return FIBE;
}


NTSTATUS RouterStartup(
    VOID)
/*
 * FUNCTION: Initializes the routing subsystem
 * RETURNS:
 *     Status of operation
 */
{
    TI_DbgPrint(DEBUG_ROUTER, ("Called.\n"));

    /* Initialize the Forward Information Base */
    InitializeListHead(&FIBListHead);
    TcpipInitializeSpinLock(&FIBLock);

#if 0
    /* TEST: Create a test route */
    /* Network is 10.0.0.0  */
    /* Netmask is 255.0.0.0 */
    /* Router is 10.0.0.1   */
    RouterCreateRouteIPv4(0x0000000A, 0x000000FF, 0x0100000A, NTE?, 1);
#endif
    return STATUS_SUCCESS;
}


NTSTATUS RouterShutdown(
    VOID)
/*
 * FUNCTION: Shuts down the routing subsystem
 * RETURNS:
 *     Status of operation
 */
{
    KIRQL OldIrql;

    TI_DbgPrint(DEBUG_ROUTER, ("Called.\n"));

    /* Clear Forward Information Base */
    TcpipAcquireSpinLock(&FIBLock, &OldIrql);
    DestroyFIBEs();
    TcpipReleaseSpinLock(&FIBLock, OldIrql);

    return STATUS_SUCCESS;
}

/* EOF */
