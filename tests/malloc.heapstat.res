# **********************************************************
# Copyright (c) 2009-2010 VMware, Inc.  All rights reserved.
# **********************************************************
#
# Dr. Memory: the memory debugger
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; 
# version 2.1 of the License, and no later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
:::Dr.Heapstat:::       3 unique,     3 total,    155 byte(s) of leak(s)
:::Dr.Heapstat:::       1 unique,     1 total,     16 byte(s) of possible leak(s)
# Different order based on hashtable iterator order
!if UNIX
Error #1: LEAK 42 direct bytes + 17 indirect bytes
malloc.c:198
Error #2: LEAK 16 direct bytes + 48 indirect bytes
malloc.c:230
Error #3: POSSIBLE LEAK 16 direct bytes + 0 indirect bytes
malloc.c:235
Error #4: LEAK 16 direct bytes + 16 indirect bytes
malloc.c:236
!endif
!if WINDOWS
Error #1: LEAK 42 direct bytes + 17 indirect bytes
malloc.c:198
Error #2: POSSIBLE LEAK 16 direct bytes + 0 indirect bytes
malloc.c:235
Error #3: LEAK 16 direct bytes + 64 indirect bytes
malloc.c:236
Error #4: LEAK 16 direct bytes + 0 indirect bytes
malloc.c:230
!endif
