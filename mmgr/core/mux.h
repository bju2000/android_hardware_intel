/* Modem Manager - mux header file
**
** Copyright (C) Intel 2012
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
**
*/

#ifndef __MMGR_MUX_HEADER__
#define __MMGR_MUX_HEADER__

#include "tcs_mmgr.h"

e_mmgr_errors_t modem_handshake(int fd_tty, int timeout);
e_mmgr_errors_t configure_cmux_driver(int fd_tty, int max_frame_size);
e_mmgr_errors_t send_at_cmux(int fd_tty, const mux_t *mux);

#endif                          /* __MCDR_FILE_HEADER__ */
