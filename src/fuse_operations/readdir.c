/*
   Tagsistant (tagfs) -- fuse_operations/readdir.c
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
 * used by add_entry_to_dir() SQL callback to perform readdir() operations
 */
struct tagsistant_use_filler_struct {
	fuse_fill_dir_t filler;			/**< libfuse filler hook to return dir entries */
	void *buf;						/**< libfuse buffer to hold readdir results */
	const char *path;				/**< the path that generates the query */
	tagsistant_querytree_t *qtree;	/**< the querytree that originated the readdir() */
};

/**
 * SQL callback. Add dir entries to libfuse buffer.
 *
 * @param filler_ptr struct tagsistant_use_filler_struct pointer (cast to void*)
 * @param result dbi_result pointer
 * @return(0 (always, see SQLite policy, may change in the future))
 */
static int tagsistant_add_entry_to_dir(void *filler_ptr, dbi_result result)
{
	struct tagsistant_use_filler_struct *ufs = (struct tagsistant_use_filler_struct *) filler_ptr;
	const char *dir = dbi_result_get_string_idx(result, 1);

	if (dir == NULL || strlen(dir) == 0)
		return(0);

	/* check if this tag has been already listed inside the path */
	ptree_or_node_t *ptx = ufs->qtree->tree;
	while (NULL != ptx->next) ptx = ptx->next; // last OR section

	ptree_and_node_t *and_t = ptx->and_set;
	while (NULL != and_t) {
		if (g_strcmp0(and_t->tag, dir) == 0) {
			return(0);
		}
		and_t = and_t->next;
	}

	return(ufs->filler(ufs->buf, dir, NULL, 0));
}

// TODO split readdir in smaller functions
/**
 * readdir equivalent (in FUSE paradigm)
 *
 * @param path the path of the directory to be read
 * @param buf buffer holding directory entries
 * @param filler libfuse fuse_fill_dir_t function to save entries in *buf
 * @param offset offset of next read
 * @param fi struct fuse_file_info passed by libfuse; unused.
 * @return(0 on success, -errno otherwise)
 */
int tagsistant_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	int res = 0, tagsistant_errno = 0;
	struct dirent *de;
	gchar *readdir_path = NULL;

	(void) fi;
	(void) offset;

	TAGSISTANT_START("/ READDIR on %s", path);

	// build querytree
	tagsistant_querytree_t *qtree = tagsistant_build_querytree(path, 0);

	// -- malformed --
	if (QTREE_IS_MALFORMED(qtree)) {
		dbg(LOG_INFO, "readdir on malformed path %s", path);
		res = -1;
		tagsistant_errno = ENOENT;
		goto READDIR_EXIT;
	}

	if (QTREE_POINTS_TO_OBJECT(qtree)) {
		dbg(LOG_INFO, "readdir on object %s", path);
		DIR *dp = opendir(qtree->full_archive_path);
		if (NULL == dp) {

			readdir_path = tagsistant_get_alias(path);
			if (NULL != readdir_path) {
				dp = opendir(readdir_path);
				tagsistant_errno = errno;

				if (NULL == dp) {
					tagsistant_errno = errno;
					goto READDIR_EXIT;
				}
			}
		}

		while ((de = readdir(dp)) != NULL) {
			struct stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (filler(buf, de->d_name, &st, 0))
				break;
		}

		closedir(dp);
	} else if (QTREE_IS_ROOT(qtree)) {
		dbg(LOG_INFO, "readdir on root %s", path);
		/*
		 * insert pseudo directories: tags/ archive/ relations/ and stats/
		 */
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		filler(buf, "archive", NULL, 0);
		filler(buf, "relations", NULL, 0);
		filler(buf, "stats", NULL, 0);
		filler(buf, "tags", NULL, 0);
	} else if (QTREE_IS_ARCHIVE(qtree)) {
		dbg(LOG_INFO, "readdir on archive");
		/*
		 * already served by QTREE_POINTS_TO_OBJECT()?
		 */
		DIR *dp = opendir(tagsistant.archive);
		if (dp == NULL) {
			tagsistant_errno = errno;
			goto READDIR_EXIT;
		}

		while ((de = readdir(dp)) != NULL) {
			struct stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (filler(buf, de->d_name, &st, 0))
				break;
		}
	} else if (QTREE_IS_TAGS(qtree)) {
		dbg(LOG_INFO, "readdir on tags");
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		if (qtree->complete) {
			// build the filetree
			file_handle_t *fh = tagsistant_build_filetree(qtree->tree, path);

			// check filetree is not null
			if (NULL == fh) {
				tagsistant_errno = EBADF;
				goto READDIR_EXIT;
			}

			// save filetree reference to later destroy it
			file_handle_t *fh_save = fh;

			// add each filetree node to directory
			do {
				if ( (fh->name != NULL) && strlen(fh->name)) {
					dbg(LOG_INFO, "Adding %s to directory", fh->name);
					if (filler(buf, fh->name, NULL, offset))
						break;
				}
				fh = fh->next;
			} while ( fh != NULL && fh->name != NULL );

			// destroy the file tree
			tagsistant_destroy_filetree(fh_save);
		} else {
			// add operators if path is not "/tags", to avoid
			// "/tags/+" and "/tags/="
			if (g_strcmp0(path, "/tags") != 0) {
				filler(buf, "+", NULL, 0);
				filler(buf, "=", NULL, 0);
			}

			/*
		 	* if path does not terminate by =,
		 	* directory should be filled with tagsdir registered tags
		 	*/
			struct tagsistant_use_filler_struct *ufs = g_new0(struct tagsistant_use_filler_struct, 1);
			if (ufs == NULL) {
				dbg(LOG_ERR, "Error allocating memory");
				goto READDIR_EXIT;
			}

			ufs->filler = filler;
			ufs->buf = buf;
			ufs->path = path;
			ufs->qtree = qtree;

			/* parse tagsdir list */
			tagsistant_query("select tagname from tags;", tagsistant_add_entry_to_dir, ufs);
			freenull(ufs);
		}
	} else if (QTREE_IS_RELATIONS(qtree)) {
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);

		struct tagsistant_use_filler_struct *ufs = g_new0(struct tagsistant_use_filler_struct, 1);
		if (ufs == NULL) {
			dbg(LOG_ERR, "Error allocating memory");
			goto READDIR_EXIT;
		}

		ufs->filler = filler;
		ufs->buf = buf;
		ufs->path = path;
		ufs->qtree = qtree;

		if (qtree->second_tag) {
			// nothin'
			dbg(LOG_INFO, "readdir on /relations/somethins/relations/somethingelse");
		} else if (qtree->relation) {
			// list all tags related to first_tag with this relation
			dbg(LOG_INFO, "readdir on /relations/somethind/relation/");
			tagsistant_query("select tags.tagname from tags join relations on relations.tag2_id = tags.tag_id join tags as firsttags on firsttags.tag_id = relations.tag1_id where firsttags.tagname = '%s' and relation = '%s';",
				tagsistant_add_entry_to_dir, ufs, qtree->first_tag, qtree->relation);

		} else if (qtree->first_tag) {
			// list all relations
			dbg(LOG_INFO, "readdir on /relations/something/");
			tagsistant_query("select relation from relations join tags on tags.tag_id = relations.tag1_id where tagname = '%s';",
				tagsistant_add_entry_to_dir, ufs, qtree->first_tag);
		} else {
			// list all tags
			dbg(LOG_INFO, "readdir on /relations");
			tagsistant_query("select tagname from tags;", tagsistant_add_entry_to_dir, ufs);
		}

		freenull(ufs);

	} else if (QTREE_IS_STATS(qtree)) {
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		// fill with available statistics
	}

READDIR_EXIT:
	if ( res == -1 ) {
		TAGSISTANT_STOP_ERROR("\\ READDIR on %s (%s): %d %d: %s", path, tagsistant_query_type(qtree), res, tagsistant_errno, strerror(tagsistant_errno));
	} else {
		TAGSISTANT_STOP_OK("\\ READDIR on %s (%s): OK", path, tagsistant_query_type(qtree));
	}

	tagsistant_destroy_querytree(qtree);
	return((res == -1) ? -tagsistant_errno : 0);
}