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

  $Id: sdp_inet.h,v 1.5 2004/02/24 23:48:43 roland Exp $
*/

#ifndef _TS_SDP_INET_H
#define _TS_SDP_INET_H

/* ------------------------------------------------------------------------- */
/* user space                                                                */
/* ------------------------------------------------------------------------- */
#ifndef __KERNEL__


#endif
/* ------------------------------------------------------------------------- */
/* both                                                                      */
/* ------------------------------------------------------------------------- */
/*
 * constants shared between user and kernel space.
 */
#define AF_INET_SDP 26             /* SDP socket protocol family */
#define AF_INET_STR "AF_INET_SDP"  /* SDP enabled enviroment variable */
#endif /* _TS_SDP_INET_H */
