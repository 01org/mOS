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

  $Id: core_main.c,v 1.7 2004/02/25 01:14:52 roland Exp $
*/

#include "core_priv.h"

#include "ts_kernel_trace.h"
#include "ts_kernel_services.h"

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>

#include <linux/init.h>
#include <linux/errno.h>

MODULE_AUTHOR("Roland Dreier");
MODULE_DESCRIPTION("core kernel IB API");
MODULE_LICENSE("Dual BSD/GPL");

static int __init _tsIbCoreInitModule(
                                     void
                                     ) {
  int ret;

  TS_REPORT_INIT(MOD_KERNEL_IB,
                 "Initializing core IB layer");

  ret = tsIbCreateProcDir();
  if (ret) {
    TS_REPORT_WARN(MOD_KERNEL_IB, "Couldn't create IB core proc directory");
    return ret;
  }

  TS_REPORT_INIT(MOD_KERNEL_IB,
                 "core IB layer initialized");

  tsIbDeviceInitModule();

  return 0;
}

static void __exit _tsIbCoreCleanupModule(
                                          void
                                          ) {
  TS_REPORT_CLEANUP(MOD_KERNEL_IB,
                    "Unloading core IB layer");

  tsIbRemoveProcDir();

  TS_REPORT_CLEANUP(MOD_KERNEL_IB,
                    "core IB layer unloaded");
}

module_init(_tsIbCoreInitModule);
module_exit(_tsIbCoreCleanupModule);
