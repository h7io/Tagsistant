/*
   Tagsistant (tagfs) -- utils.c
   Copyright (C) 2006-2009 Tx0 <tx0@strumentiresistenti.org>

   Tagsistant (tagfs) mount binary written using FUSE userspace library.

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

#include "tagsistant.h"
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

GRegex *tagsistant_inode_extract_from_path_regex = NULL;
GThread *deduplication_thread = NULL;

#ifdef DEBUG_TO_LOGFILE
void open_debug_file()
{
	char debug_file[1024];
	sprintf(debug_file, "/tmp/tagsistant.debug.%d", getpid());
	tagsistant.debugfd = fopen(debug_file, "w");
	if (tagsistant.debugfd == NULL)
		dbg(LOG_ERR, "Can't open logfile %s: %s!", debug_file, strerror(errno));
}
#endif

#ifdef _DEBUG_SYSLOG
/**
 * initialize syslog stream
 */
void init_syslog()
{
	static int enabled = 0;
	if (!enabled) {
		openlog("tagsistant", LOG_PID, LOG_DAEMON);
		enabled = 1;
	}
}
#endif

#ifdef MACOSX
ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
	if (*lineptr == NULL)
		*lineptr = g_malloc0(sizeof(char) * (*n + 1));

	if (*lineptr == NULL)
		return(0);

	if (fgets(*lineptr, *n, stream) == NULL)
		*n = 0;
	else
		*n = strlen(*lineptr);

	return(*n);
}
#endif

/**
 * return the tagsistant inode contained into a path
 *
 * @param qtree the querytree object supposed to contain an inode
 * @return the inode, if found
 */
tagsistant_inode tagsistant_inode_extract_from_path(tagsistant_querytree *qtree)
{
	if (!qtree || !qtree->object_path) return (0);

	tagsistant_inode inode = 0;

	GMatchInfo *match_info;
	if (g_regex_match(tagsistant_inode_extract_from_path_regex, qtree->object_path, 0, &match_info)) {
		/*
		 * extract the inode
		 */
		gchar *inode_text = g_match_info_fetch(match_info, 1);
		gchar *backup_inode_text = inode_text;
		inode = strtoul(inode_text, &backup_inode_text, 10);
		g_free(inode_text);

		/*
		 * replace the inode and the separator with a blank string,
		 * actually stripping it from the object_path
		 */
		qtree->object_path = g_regex_replace(
			tagsistant_inode_extract_from_path_regex,
			qtree->object_path,
			strlen(qtree->object_path),
			0, "", 0, NULL);
	}
	g_match_info_free(match_info);

#if TAGSISTANT_VERBOSE_LOGGING
	if (inode) {
		dbg(LOG_INFO, "%s has inode %lu", qtree->object_path, (long unsigned int) inode);
	} else {
		dbg(LOG_INFO, "%s does not contain and inode", qtree->object_path);
	}
#endif

	return (inode);
}

/**
 * Print configuration lines on STDERR
 */
void tagsistant_show_config()
{
	int c;

	// repo internal data
	fprintf(stderr, "\n[Repository]\n");
	c = 1;
	fprintf(stderr, "repository: %s\n", tagsistant.repository);
	fprintf(stderr, "archive: %s\n", tagsistant.archive);
	fprintf(stderr, "mount_point: %s\n", tagsistant.mountpoint);

	// SQL backend
	fprintf(stderr, "\n[SQL]\n");
	fprintf(stderr, "db_options: %s\n", tagsistant.dboptions);
	dbi_driver driver = NULL;
	c = 1;
	while ((driver = dbi_driver_list(driver))) {
		fprintf(stderr, "driver_%02d: %s, %s\n", c++, dbi_driver_get_name(driver), dbi_driver_get_filename(driver));
	}

	// plugin infrastructure
	fprintf(stderr, "\n[Plugins]\n");
	tagsistant_plugin_t *pp = tagsistant.plugins;
	c = 1;
	while (pp != NULL) {
		fprintf(stderr, "%s: %s\n", pp->mime_type, pp->filename);
		pp = pp->next;
	}
}

/**
 * Create an object and tag it
 *
 * @param qtree the querytree asking object creation
 * @param tagsistant_errno error_reporting variable
 * @param force_create boolean: if true, creation is forced
 */
int tagsistant_inner_create_and_tag_object(tagsistant_querytree *qtree, int *tagsistant_errno, int force_create)
{
	tagsistant_inode inode = 0;

	// 1. create the object on db or get its inode if exists
	//    if force_create is true, create a new object and fetch its inode
	//    if force_create is false, try to find an object with name and path matching
	//    and use its inode, otherwise create a new one
	if (!force_create) {
		tagsistant_query(
			"select inode from objects where objectname = \"%s\" limit 1",
			qtree->dbi,
			tagsistant_return_integer,
			&inode,
			qtree->object_path);
	}

	if (force_create || (!inode)) {
		tagsistant_query(
			"insert into objects (objectname) values (\"%s\")",
			qtree->dbi, NULL, NULL, qtree->object_path);

		inode = tagsistant_last_insert_id(qtree->dbi);
	}

	if (!inode) {
		dbg(LOG_ERR, "Object %s recorded as inode 0!", qtree->object_path);
		*tagsistant_errno = EIO;
		return(-1);
	}

	// 2. adjust archive_path and full_archive_path with leading inode
	tagsistant_querytree_set_inode(qtree, inode);

	// 3. tag the object
	tagsistant_querytree_traverse(qtree, tagsistant_sql_tag_object, inode);

	// 4. use autotagging plugin stack
	// the object is empty, how should autotagging be able to extract anything?
	//	tagsistant_process(qtree);

#if TAGSISTANT_VERBOSE_LOGGING
	if (force_create) {
		dbg(LOG_INFO, "Forced creation of object %s", qtree->full_path);
	} else {
		dbg(LOG_INFO, "Tried creation of object %s", qtree->full_path);
	}
#endif

	return(inode);
}

/****************************************************************************/
/***                                                                      ***/
/***   Checksumming and deduplication support                             ***/
/***                                                                      ***/
/****************************************************************************/

/**
 * Invalidate an object checksum
 *
 * @param inode the object inode
 */
void tagsistant_invalidate_object_checksum(tagsistant_inode inode, dbi_conn conn)
{
	tagsistant_query("update objects set checksum = \"\" where inode = %d", conn, NULL, NULL, inode);
}

#if 0
/**
 * Internal structure passed to tagsistant_remove_duplicated_inode
 */
typedef struct {
	tagsistant_inode main_inode;
	dbi_conn dbi;
	gchar *path;
} tagsistant_rdi;

/**
 * callback called by tagsistant_find_duplicated_objects
 *
 * @param _context pointer to tagsistant_rdi struct
 * @param result DBI result
 */
int tagsistant_remove_duplicated_inode(void *_context, dbi_result result)
{
	tagsistant_rdi *context = (tagsistant_rdi *) _context;
	tagsistant_inode duplicated_inode = 0;

	if (tagsistant.sql_database_driver == TAGSISTANT_DBI_SQLITE_BACKEND)
		duplicated_inode = dbi_result_get_ulonglong_idx(result, 1);
	else
		duplicated_inode = dbi_result_get_uint_idx(result, 1);

	if (context->main_inode != duplicated_inode) {
		/* transfer tags to the main inode */
		tagsistant_query(
			"update tagging set inode = %d where inode = %d",
			context->dbi,
			NULL, NULL,
			context->main_inode,
			duplicated_inode);

		/* unlink the removable inode */
		tagsistant_query(
			"delete from objects where inode = %d",
			context->dbi,
			NULL, NULL,
			duplicated_inode);

		unlink(context->path);
	}

	return (0);
}
#endif

/**
 * deduplication function called by tagsistant_calculate_object_checksum
 *
 * @param inode the object inode
 * @param hex the checksum string
 * @param dbi DBI connection handle
 */
void tagsistant_find_duplicated_objects(tagsistant_inode inode, gchar *hex, gchar *path, dbi_conn dbi)
{
	tagsistant_inode main_inode = 0;

	/* get the first inode matching the checksum */
	tagsistant_query(
		"select inode from objects "
			"where checksum = \"%s\" "
			"order by inode limit 1",
		dbi,
		tagsistant_return_integer,
		&main_inode,
		hex);

	/* if we have just one file, we can return */
	if (inode == main_inode) return;

	dbg(LOG_INFO, "Deduplicating %s: %d -> %d", path, inode, main_inode);

	/* first move all the tags of inode to main_inode */
	tagsistant_query(
		"update tagging set inode = %d where inode = %d",
		dbi,
		NULL, NULL,
		main_inode,
		inode);

	/* then delete records left because of duplicates in key(inode, tag_id) in the tagging table */
	tagsistant_query(
		"delete from tagging where inode = %d",
		dbi,
		NULL, NULL,
		inode);

	/* and finally unlink the removable inode */
	tagsistant_query(
		"delete from objects where inode = %d",
		dbi,
		NULL, NULL,
		inode);

	unlink(path);

#if 0
	tagsistant_rdi *rdi = g_new0(tagsistant_rdi, 1);
	if (!rdi) return;

	rdi->dbi = dbi;
	rdi->main_inode = inode;
	rdi->path = path;

	tagsistant_query(
		"select inode from objects "
			"where checksum = \"%s\" and inode <> %d "
			"order by inode",
		dbi,
		tagsistant_remove_duplicated_inode,
		rdi,
		hex,
		main_inode);
#endif
}

/**
 * Calculate the checksum of an object and look for duplicated objects
 *
 * @param inode the object inode
 */
void tagsistant_calculate_object_checksum(tagsistant_inode inode, dbi_conn dbi)
{
	gchar *objectname = NULL;

	/* fetch the object name */
	tagsistant_query(
		"select objectname from objects where inode = %d",
		dbi,
		tagsistant_return_string,
		&objectname,
		inode);

	if (!objectname) return;

	/* compute the object path */
	gchar *path = g_strdup_printf(
		"%s%d" TAGSISTANT_INODE_DELIMITER "%s",
		tagsistant.archive,
		inode,
		objectname);

	if (path) {
		/* guess if the object is a file or a symlink */
		struct stat buf;
		if ((-1 == lstat(path, &buf)) || (!S_ISREG(buf.st_mode) && !S_ISLNK(buf.st_mode))) {
			g_free(path);
			g_free(objectname);
			return;
		}

		dbg(LOG_INFO, "Checksumming %s", path);

		/* open the file and read its content */
		int fd = open(path, O_RDONLY|O_NOATIME);
		if (-1 != fd) {
			GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA1);
			guchar *buffer = g_new0(guchar, 65535);

			if (checksum && buffer) {
				/* feed the checksum object */
				do {
					int length = read(fd, buffer, 65535);
					if (length > 0)
						g_checksum_update(checksum, buffer, length);
					else
						break;
				} while (1);

				/* get the hexadecimal checksum string */
				gchar *hex = g_strdup(g_checksum_get_string(checksum));

				/* destroy the checksum object */
				g_checksum_free(checksum);
				g_free(buffer);

				/* save the string into the objects table */
				tagsistant_query(
					"update objects set checksum = '%s' where inode = %d;",
					dbi, NULL, NULL, hex, inode);

				/* look for duplicated objects */
				tagsistant_find_duplicated_objects(inode, hex, path, dbi);

				/* free the hex checksum string */
				g_free(hex);
			}
			close(fd);
		}
		g_free(path);
	}

	g_free(objectname);
}

/**
 * Deduplicator callback: calculate the checksum of an object
 *
 * @param data pointer to dbi connection
 * @param result
 * @return
 */
int tagsistant_deduplicator_callback(void *data, dbi_result result)
{
	dbi_conn dbi = (dbi_conn) data;
	tagsistant_inode inode = 0;

	if (tagsistant.sql_database_driver == TAGSISTANT_DBI_SQLITE_BACKEND)
		inode = dbi_result_get_ulonglong_idx(result, 1);
	else
		inode = dbi_result_get_uint_idx(result, 1);

	tagsistant_calculate_object_checksum(inode, dbi);

	return (0);
}

/**
 * Deduplication thread kernel
 *
 * @data unused
 */
gpointer tagsistant_deduplicator(gpointer data)
{
	(void) data;

	while (1) {
		tagsistant_dbi_connection *conn = tagsistant_db_connection(TAGSISTANT_START_TRANSACTION);

		/* iterate over all the object with null checksum */
		tagsistant_query(
			"select inode from objects where checksum = \"\"",
			conn->dbi,
			tagsistant_deduplicator_callback,
			conn->dbi);

		tagsistant_commit_transaction(conn->dbi);

		/* close the connection to the DBMS */
		conn->in_use = 0;

		/* sleep for one minute */
		g_usleep(TAGSISTANT_DEDUPLICATION_FREQUENCY * G_USEC_PER_SEC);
	}

	return (NULL);
}

/**
 * Initialize all the utilities
 */
void tagsistant_utils_init()
{
	/* compile regular expressions */
	tagsistant_inode_extract_from_path_regex = g_regex_new("^([0-9]+)" TAGSISTANT_INODE_DELIMITER, 0, 0, NULL);

	/* start deduplication thread */
#if TAGSISTANT_ENABLE_DEDUPLICATOR
	deduplication_thread = g_thread_new("deduplication", tagsistant_deduplicator, NULL);
#endif
}
