/*
   Tagsistant (tagfs) -- fuse_operations/open.c
   Copyright (C) 2006-2009 Tx0 <tx0@strumentiresistenti.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include "../tagsistant.h"

/**
 * close() equivalent [first part, second is tagsistant_release()]
 *
 * @param path the path to be open()ed
 * @param fi struct fuse_file_info holding open() flags
 * @return(0 on success, -errno otherwise)
 */
int tagsistant_flush(const char *path, struct fuse_file_info *fi)
{
    int res = 0, tagsistant_errno = 0;
    (void) fi;

	TAGSISTANT_START("FLUSH on %s", path);

	// build querytree
	tagsistant_querytree *qtree = tagsistant_querytree_new(path, 1, 0, 0);

	// -- malformed --
	if (QTREE_IS_MALFORMED(qtree))
		TAGSISTANT_ABORT_OPERATION(ENOENT);

	// -- object --
	if (QTREE_IS_TAGGABLE(qtree)) {
		// check if the file has been modified
		int modified = 0;
		tagsistant_query(
			"select inode from objects where inode = %d and checksum = \"\"",
			qtree->dbi, tagsistant_return_integer, &modified, qtree->inode);

		if (modified) {
			// run the autotagging plugin stack
			tagsistant_process(qtree);

			// deduplicate the object
			tagsistant_querytree_deduplicate(qtree);
		}
	}

TAGSISTANT_EXIT_OPERATION:
	if ( res == -1 ) {
		TAGSISTANT_STOP_ERROR("FLUSH on %s (%s) (%s): %d %d: %s", path, qtree->full_archive_path, tagsistant_querytree_type(qtree), res, tagsistant_errno, strerror(tagsistant_errno));
		tagsistant_querytree_destroy(qtree, TAGSISTANT_ROLLBACK_TRANSACTION);
	} else {
		TAGSISTANT_STOP_OK("FLUSH on %s (%s): OK", path, tagsistant_querytree_type(qtree));
		tagsistant_querytree_destroy(qtree, TAGSISTANT_COMMIT_TRANSACTION);
	}

	return ((res == -1) ? -tagsistant_errno : 0);
}