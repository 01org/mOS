/*
  This software is available to you under a choice of one of two
  licenses.  You may choose to be licensed under the terms of the GNU
  General Public License (GPL) Version 2, available at
  <http://www.fsf.org/copyleft/gpl.html>, or the OpenIB.org BSD
  license, available in the LICENSE.TXT file accompanying this
  software.  These details are also available at
  <http://openib.org/license.html>.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

  Copyright (c) 2004 Topspin Communications.  All rights reserved.

  $Id: ts_ib_header.h,v 1.5 2004/02/25 00:32:32 roland Exp $
*/

#ifndef _TS_IB_HEADER_H
#define _TS_IB_HEADER_H

#if defined(MODVERSIONS) && !defined(__GENKSYMS__) && !defined(TS_KERNEL_2_6)
#  include "ts_kernel_version.h"
#  include TS_VER_FILE(../header,header_export.ver)
#endif

#include "ts_ib_header_types.h"

void tsIbUdHeaderInit(
                      int              payload_bytes,
                      int              grh_present,
                      tTS_IB_UD_HEADER header
                      );

int tsIbUdHeaderPack(
                     tTS_IB_UD_HEADER header,
                     void            *buf
                     );

int tsIbUdHeaderUnpack(
                       void            *buf,
                       tTS_IB_UD_HEADER header
                       );

#endif /* _TS_IB_HEADER_H */
