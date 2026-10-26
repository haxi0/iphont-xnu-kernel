# Third-party notices for the prebuilt kernel

`prebuilt/mach.development.bcm2837` is built from Apple's XNU (APSL 2.0) with the changes in `xnu/`, and it
also contains code of the following projects, linked in at build time (see README.md, "Licenses").

## ath11k (Linux, drivers/net/wireless/ath/ath11k)

Built by `tools/mk-ath11k.sh` from Linux 7.0.14, unmodified. License: BSD-3-Clause-Clear (one header of
`drivers/net/wireless/ath`, `spectral_common.h`, is ISC). The copyright lines of the files it is built from:

```
Copyright (c) 2013 Qualcomm Atheros, Inc.
Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
Copyright (c) 2018-2020 The Linux Foundation. All rights reserved.
Copyright (c) 2019 The Linux Foundation. All rights reserved.
Copyright (c) 2019-2020 The Linux Foundation. All rights reserved.
Copyright (c) 2019-2021 The Linux Foundation. All rights reserved.
Copyright (c) 2020 The Linux Foundation. All rights reserved.
Copyright (c) 2020-2021 The Linux Foundation. All rights reserved.
Copyright (c) 2021 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021, 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2022 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2022, 2025 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2022, Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2023, 2025 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2024 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2021-2025 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022, 2024 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022, 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
```

### BSD-3-Clause-Clear

```
The Clear BSD License

Redistribution and use in source and binary forms, with or without modification, are permitted (subject to the
limitations in the disclaimer below) provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this list of conditions and the
   following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
   following disclaimer in the documentation and/or other materials provided with the distribution.
 * Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
   products derived from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE. THIS SOFTWARE IS
PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### spectral_common.h (ISC)

```
Copyright (c) 2013 Qualcomm Atheros, Inc.
Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.
THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## FreeBSD LinuxKPI and kernel headers

From FreeBSD 15.1-RELEASE (`src.txz`), patched with `wifi/lx/patches/`. BSD-2-Clause. FreeBSD's `COPYRIGHT`:

```

The compilation of software known as FreeBSD is distributed under the
following terms:

Copyright (c) 1992-2025 The FreeBSD Project.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.

The 4.4BSD and 4.4BSD-Lite software is distributed under the following
terms:

All of the documentation and software included in the 4.4BSD and 4.4BSD-Lite
Releases is copyrighted by The Regents of the University of California.

Copyright 1979, 1980, 1983, 1986, 1988, 1989, 1991, 1992, 1993, 1994
	The Regents of the University of California.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. All advertising materials mentioning features or use of this software
   must display the following acknowledgement:
This product includes software developed by the University of
California, Berkeley and its contributors.
4. Neither the name of the University nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.

The Institute of Electrical and Electronics Engineers and the American
National Standards Committee X3, on Information Processing Systems have
given us permission to reprint portions of their documentation.

In the following statement, the phrase ``this text'' refers to portions
of the system documentation.

Portions of this text are reprinted and reproduced in electronic form in
the second BSD Networking Software Release, from IEEE Std 1003.1-1988, IEEE
Standard Portable Operating System Interface for Computer Environments
(POSIX), copyright C 1988 by the Institute of Electrical and Electronics
Engineers, Inc.  In the event of any discrepancy between these versions
and the original IEEE Standard, the original IEEE Standard is the referee
document.

In the following statement, the phrase ``This material'' refers to portions
of the system documentation.

This material is reproduced with permission from American National
Standards Committee X3, on Information Processing Systems.  Computer and
Business Equipment Manufacturers Association (CBEMA), 311 First St., NW,
Suite 500, Washington, DC 20001-2178.  The developmental work of
Programming Language C was completed by the X3J11 Technical Committee.

The views and conclusions contained in the software and documentation are
those of the authors and should not be interpreted as representing official
policies, either expressed or implied, of the Regents of the University
of California.


NOTE: The copyright of UC Berkeley's Berkeley Software Distribution ("BSD")
source has been updated.  The copyright addendum may be found at
ftp://ftp.cs.berkeley.edu/pub/4bsd/README.Impt.License.Change and is
included below.

July 22, 1999

To All Licensees, Distributors of Any Version of BSD:

As you know, certain of the Berkeley Software Distribution ("BSD") source
code files require that further distributions of products containing all or
portions of the software, acknowledge within their advertising materials
that such products contain software developed by UC Berkeley and its
contributors.

Specifically, the provision reads:

"     * 3. All advertising materials mentioning features or use of this software
      *    must display the following acknowledgement:
      *    This product includes software developed by the University of
      *    California, Berkeley and its contributors."

Effective immediately, licensees and distributors are no longer required to
include the acknowledgement within advertising materials.  Accordingly, the
foregoing paragraph of those BSD Unix files containing it is hereby deleted
in its entirety.

William Hoskins
Director, Office of Technology Licensing
University of California, Berkeley
```

## Fetched at build time, not in this repository (listed for completeness)

- **Apple HFS+** (`hfs-627.100.6`, commit `31d5ec238dfab485e78a9a30b8b92d13e8914f03`, APSL 2.0): fetched by `xnu/hfs-stage.sh` into `bsd/hk_hfs/` and built into the kernel for the OnePlus 8's data volume. Apple's copyright notices are in the fetched files.
- **libdeflate 1.24** (Eric Biggers, MIT, `xnu/files/bsd/miscfs/apfs/libdeflate/COPYING`) and **libzbitmap** (Corellium LLC, MIT here) are in the APFS driver (see the README "Licenses").
