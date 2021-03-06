# Copyright (c) 2006-2008 MySQL AB, 2009 Sun Microsystems, Inc.
# Use is subject to license terms.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DSAFEMALLOC -DSAFE_MUTEX")
SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DSAFEMALLOC -DSAFE_MUTEX")

INCLUDE("${PROJECT_SOURCE_DIR}/storage/mysql_storage_engine.cmake")

SET(MYISAMMRG_SOURCES  myrg_close.c myrg_create.c myrg_delete.c myrg_extra.c myrg_info.c
				ha_myisammrg.cc
				myrg_locking.c myrg_open.c myrg_panic.c myrg_queue.c myrg_range.c
				myrg_rfirst.c myrg_rkey.c myrg_rlast.c myrg_rnext.c myrg_rnext_same.c
				myrg_rprev.c myrg_rrnd.c myrg_rsame.c myrg_static.c myrg_update.c
				myrg_write.c myrg_records.c)

MYSQL_STORAGE_ENGINE(MYISAMMRG)
