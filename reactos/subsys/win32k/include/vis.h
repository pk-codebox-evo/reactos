/* $Id: vis.h,v 1.2 2003/08/02 19:56:19 dwelch Exp $
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS Win32k subsystem
 * PURPOSE:          Visibility computations interface definition
 * FILE:             include/win32k/vis.h
 * PROGRAMMER:       Ge van Geldorp (ge@gse.nl)
 *
 */

#ifndef _WIN32K_VIS_H
#define _WIN32K_VIS_H

#include <internal/ex.h>
#include <include/window.h>

HRGN FASTCALL
VIS_ComputeVisibleRegion(PDESKTOP_OBJECT Desktop, PWINDOW_OBJECT Window,
                         BOOLEAN ClientArea, BOOLEAN ClipChildren,
                         BOOLEAN ClipSiblings);

VOID FASTCALL
VIS_WindowLayoutChanged(PDESKTOP_OBJECT Desktop, PWINDOW_OBJECT Window,
                        HRGN UncoveredRgn);
VOID FASTCALL
VIS_RepaintDesktop(HWND Desktop, HRGN RepaintRgn);

#endif /* ! defined(_WIN32K_VIS_H) */

/* EOF */

