# MIT License

## EfiNtfs — Native NTFS read+write driver for UEFI

**Copyright (c) 2026 Marek Wesolowski (WESMAR)**

---

## License Grant

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

## Attribution Requirement

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

## Disclaimer of Warranty

**THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.**

---

## Project Information

- **Project:** EfiNtfs — Native NTFS read+write driver for UEFI
- **Author:** Marek Wesolowski (WESMAR)
- **Contact:** marek@wesolowski.eu.org
- **Platform:** Windows 10/11 x64 (build environment); UEFI x64 (runtime)
- **Language:** C (UEFI/EDK2 calling conventions, no CRT)

---

## Third-party components

The prebuilt EDK2 static libraries under `lib/` are derived from the
[EDK II project](https://github.com/tianocore/edk2) maintained by the Tianocore community,
licensed under BSD-2-Clause-Patent. The EDK2 public headers under `include/edk2/` are
distributed under the same license.

---

## Responsible Use

This software is a low-level UEFI filesystem driver for educational and research purposes.
Use it only on systems you own or have explicit written permission to modify.

- **Authorization required** — only use on disks you own or have explicit permission to access.
- **Local laws** — comply with computer fraud and abuse laws in your jurisdiction.
- **Test on VHDs first** — always validate on a virtual disk before touching real data.

---

*Copyright (c) 2026 Marek Wesolowski (WESMAR). All rights reserved under MIT License terms.*
