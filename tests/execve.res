# **********************************************************
# Copyright (c) 2010-2013 Google, Inc.  All rights reserved.
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
Error #1: UNADDRESSABLE ACCESS: reading 1 byte(s)
malloc.c:108
Error #2: UNINITIALIZED READ
malloc.c:118
Error #3: UNINITIALIZED READ
malloc.c:131
Error #4: INVALID HEAP ARGUMENT
malloc.c:175
%OUT_OF_ORDER
: LEAK 42 direct bytes + 17 indirect bytes
malloc.c:235
: LEAK 16 direct bytes + 48 indirect bytes
malloc.c:267
: POSSIBLE LEAK 16 direct bytes + 0 indirect bytes
malloc.c:272
: LEAK 16 direct bytes + 16 indirect bytes
malloc.c:273
