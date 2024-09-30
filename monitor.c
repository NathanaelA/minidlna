/* MiniDLNA media server
 * Copyright (C) 2008-2010  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "libav.h"

#include "upnpglobalvars.h"
#include "monitor.h"
#include "utils.h"
#include "sql.h"
#include "scanner.h"
#include "metadata.h"
#include "albumart.h"
#include "playlist.h"
#include "log.h"

time_t next_pl_fill = 0;

int
update_password(const char *password_path)
{
	typedef struct _node
	{
		char path[PATH_MAX];
		struct _node *next;
	} node;

	int error = 0;
	node head = { { 0 }, NULL };
	node *tail = &head;
	node *current = &head;
	char *password = NULL;

	DPRINTF(E_INFO, L_PASSWORD, "Updating password file: %s.\n", password_path);

	// Retrieve new password from file
	if( access(password_path, 0) == 0 )
	{
		password = malloc(PASSWORD_ARRAY_LEN);
		if( password == NULL )
		{
			error = -1;
			goto clear;
		}

		readPassword(password_path, password, PASSWORD_ARRAY_LEN);

		if ( password[0] == '\0' )
		{
			free(password);
			password = NULL;
		}
	}

	// Copy dirname of password_path
	memcpy(head.path, password_path, strlen(password_path) - strlen(PASSWORD_FILE) - 1);

	// Check if directory has an object representation
	if ( sql_get_text_field(db,
			"SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
			" where d.PATH = '%q' and REF_ID is NULL", head.path) )
	{
		// Check if password changed
		char *db_password = sql_get_text_field(db,
			"SELECT PASSWORD from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
			" where d.PATH = '%q' and REF_ID is NULL", head.path);
		if ( db_password == NULL && password == NULL )
		{
			DPRINTF(E_DEBUG, L_PASSWORD, "Directory: %s, password is the same\n", head.path);
			goto clear;
		}
		if ( db_password != NULL && password != NULL && strcmp(db_password, password) == 0 )
		{
			DPRINTF(E_DEBUG, L_PASSWORD, "Directory: %s, password is the same\n", head.path);
			goto clear;
		}
	}
	// Check if directory exists in db
	else if (sql_get_text_field(db,
		"SELECT ID from DETAILS where PATH = '%q'", head.path) == NULL )
	{
		DPRINTF(E_DEBUG, L_PASSWORD, "Directory: %s, isn't present in DB\n", head.path);
		goto clear;
	}

	// When password is null look for a password file in an upper directory
	if( password == NULL )
	{
		char path[PATH_MAX], *dirpath = path;
		if((password = malloc(PASSWORD_ARRAY_LEN)) == NULL )
		{
			error = -1;
			goto clear;
		}

		password[0] = '\0';
		strcpy(dirpath, head.path);
		while ( password[0] == '\0' &&
		        sql_get_text_field(db, "SELECT ID from DETAILS where PATH = '%q'",
		                           (dirpath = dirname(dirpath))) != NULL )
		{
			char password_path[PATH_MAX];
			int length = snprintf(password_path, PATH_MAX, "%s/"PASSWORD_FILE, dirpath);
			if ( length > 0 && length < PATH_MAX && access(password_path, 0) == 0 )
				readPassword(password_path, password, PASSWORD_ARRAY_LEN);

			// Stop on root directory
			if (dirpath[0] == '/' && dirpath[1] == '\0')
				break;
		}

		if ( password[0] == '\0' )
		{
			free(password);
			password = NULL;
		}
	}

	DPRINTF(E_DEBUG, L_PASSWORD, "Directory: %s, new password: %s\n",
			head.path, password == NULL ? "(null)" : password);

	// Populate diretory list
	while ( current != NULL )
	{
		DIR* srcdir = opendir(current->path);
		struct dirent* dent;

		if (srcdir == NULL)
			continue;

		while( (dent = readdir(srcdir)) != NULL )
		{
			int length;
			char password_path[PATH_MAX];
			struct stat st;

			// Ignore current and previous directory entries
			if( dent->d_name[0] == '\0' || (dent->d_name[0] == '.' && (
				dent->d_name[1] == '\0' || (dent->d_name[1] == '.' && dent->d_name[2] == '\0'))) )
				continue;

			if ( fstatat(dirfd(srcdir), dent->d_name, &st, 0) < 0 )
				continue;

			if ( !S_ISDIR(st.st_mode) )
				continue;

			// Ignore subdirectory that have a password file
			length = snprintf(password_path, PATH_MAX, "%s/%s/"PASSWORD_FILE, current->path, dent->d_name);
			if (length <= 0 || PATH_MAX <= length || access(password_path, 0) == 0 )
				continue;

			// Add subdirectory to list
			tail->next = malloc(sizeof(node));
			if( tail->next == NULL )
			{
				error = -1;
				goto clear;
			}

			length = snprintf(tail->next->path, PATH_MAX, "%s/%s", current->path, dent->d_name);
			if (length <= 0 || PATH_MAX <= length)
			{
				free(tail->next);
				tail->next = NULL;
				continue;
			}

			tail->next->next = NULL;
			tail = tail->next;
			DPRINTF(E_DEBUG, L_PASSWORD, "Propagate password change to subdirectory: %s\n", tail->path);
		}
		closedir(srcdir);
		current = current->next;
	}

	current = &head;
	while( current != NULL )
	{
		DPRINTF(E_DEBUG, L_PASSWORD, "Updating DB record for: %s\n", current->path);
		// Update password for current directory
		if( sql_exec(db, "UPDATE OBJECTS set PASSWORD = '%s' where DETAIL_ID in "
		                 "(SELECT ID from DETAILS where PATH = '%s')", password, current->path) != SQLITE_OK )
			DPRINTF(E_ERROR, L_DB_SQL, "Failed to update password for directory: %s!\n", current->path);
		// Update password for all files in directory
		else if (sql_exec(db, "UPDATE OBJECTS set PASSWORD = '%s' where DETAIL_ID in "
		                      "( SELECT I FROM ( SELECT ID as I, substr(PATH, length('%s/') + 1)"
		                      "   as P from DETAILS where PATH glob '%s/*' ) WHERE P NOT LIKE '%%/%%' ) "
		                      "and CLASS != 'container.storageFolder'", password, current->path, current->path) != SQLITE_OK )
			DPRINTF(E_ERROR, L_DB_SQL, "Failed to update password for files in directory: %s!\n", current->path);

		current = current->next;
	}

	clear:
		if ( password != NULL )
			free(password);

		while ( head.next != NULL )
		{
			current = head.next;
			head.next = current->next;
			free(current);
		}

	return error;
}

int
monitor_remove_file(const char * path)
{
	char sql[128];
	char art_cache[PATH_MAX];
	char *id;
	char *ptr;
	char **result;
	int64_t detailID;
	int rows, playlist;

	if( is_caption(path) )
	{
		return sql_exec(db, "DELETE from CAPTIONS where PATH = '%q'", path);
	}
	/* Invalidate the scanner cache so we don't insert files into non-existent containers */
	valid_cache = 0;
	playlist = is_playlist(path);
	id = sql_get_text_field(db, "SELECT ID from %s where PATH = '%q'", playlist?"PLAYLISTS":"DETAILS", path);
	if( !id )
		return 1;
	detailID = strtoll(id, NULL, 10);
	sqlite3_free(id);
	if( playlist )
	{
		sql_exec(db, "DELETE from PLAYLISTS where ID = %lld", detailID);
		sql_exec(db, "DELETE from DETAILS where ID ="
		             " (SELECT DETAIL_ID from OBJECTS where OBJECT_ID = '%s$%llX')",
		         MUSIC_PLIST_ID, detailID);
		sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s$%llX' or PARENT_ID = '%s$%llX'",
		         MUSIC_PLIST_ID, detailID, MUSIC_PLIST_ID, detailID);
	}
	else
	{
		/* Delete the parent containers if we are about to empty them. */
		snprintf(sql, sizeof(sql), "SELECT PARENT_ID from OBJECTS where DETAIL_ID = %lld"
		                           " and PARENT_ID not like '64$%%'",
		                           (long long int)detailID);
		if( (sql_get_table(db, sql, &result, &rows, NULL) == SQLITE_OK) )
		{
			int i, children;
			for( i = 1; i <= rows; i++ )
			{
				/* If it's a playlist item, adjust the item count of the playlist */
				if( strncmp(result[i], MUSIC_PLIST_ID, strlen(MUSIC_PLIST_ID)) == 0 )
				{
					sql_exec(db, "UPDATE PLAYLISTS set FOUND = (FOUND-1) where ID = %d",
					         atoi(strrchr(result[i], '$') + 1));
				}

				children = sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s'", result[i]);
				if( children < 0 )
					continue;
				if( children < 2 )
				{
					sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s'", result[i]);

					ptr = strrchr(result[i], '$');
					if( ptr )
						*ptr = '\0';
					if( sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s'", result[i]) == 0 )
					{
						sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s'", result[i]);
					}
				}
			}
			sqlite3_free_table(result);
		}
		/* Now delete the actual objects */
		sql_exec(db, "DELETE from DETAILS where ID = %lld", detailID);
		sql_exec(db, "DELETE from OBJECTS where DETAIL_ID = %lld", detailID);
	}
	snprintf(art_cache, sizeof(art_cache), "%s/art_cache%s", db_path, path);
#ifdef THUMBNAIL_CREATION
	/* Remove video thumbnails */
	if ( is_video(path) )
	{
		char *vthumb = art_cache;
		strcpy(strchr(vthumb, '\0')-4, ".jpg");
	}
#endif
	remove(art_cache);

	return 0;
}

static char *
check_nfo(const char *path)
{
	char file[PATH_MAX];

	strncpyt(file, path, sizeof(file));
	strip_ext(file);

	return sql_get_text_field(db, "SELECT PATH from DETAILS where (PATH > '%q.' and PATH <= '%q.z')"
				      " and MIME glob 'video/*' limit 1", file, file);
}

int
monitor_insert_file(const char *name, const char *path)
{
	int len;
	char *last_dir;
	char *path_buf;
	char *base_name;
	char *base_copy;
	char *parent_buf = NULL;
	char *id = NULL;
	char video[PATH_MAX];
	const char *tbl = "DETAILS";
	char *dir_password = NULL;
	char password_buf[PASSWORD_ARRAY_LEN] = {0};
	char file_password[PASSWORD_ARRAY_LEN] = {0};
	char password_path[PATH_MAX];
	int depth = 1;
	int ts;
	media_types dir_types;
	media_types mtype = get_media_type(path);
	struct stat st;

	/* Is it cover art for another file? */
	if (mtype == TYPE_IMAGE)
		update_if_album_art(path);
	else if (mtype == TYPE_CAPTION)
		check_for_captions(path, 0);
	else if (mtype == TYPE_PLAYLIST)
		tbl = "PLAYLISTS";
	else if (mtype == TYPE_NFO)
	{
		char *vpath = check_nfo(path);
		if (!vpath)
			return -1;
		strncpyt(video, vpath, sizeof(video));
		sqlite3_free(vpath);
		DPRINTF(E_DEBUG, L_INOTIFY, "Found modified nfo %s\n", video);
		monitor_remove_file(video);
		name = strrchr(video, '/');
		if (!name)
			return -1;
		name++;
		path = video;
		mtype = TYPE_VIDEO;
	}

	/* Check if we're supposed to be scanning for this file type in this directory */
	dir_types = valid_media_types(path);
	if (!(mtype & dir_types))
		return -1;

	/* If it's already in the database and hasn't been modified, skip it. */
	if( stat(path, &st) != 0 )
		return -1;

	ts = sql_get_int_field(db, "SELECT TIMESTAMP from %s where PATH = '%q'", tbl, path);
	if( !ts )
	{
		DPRINTF(E_DEBUG, L_INOTIFY, "Adding: %s\n", path);
	}
	else if( ts != st.st_mtime )
	{
		DPRINTF(E_DEBUG, L_INOTIFY, "%s is %s than the last db entry.\n",
			path, (ts > st.st_mtime) ? "older" : "newer");
		monitor_remove_file(path);
	}
	else
	{
		if( ts == st.st_mtime && !GETFLAG(RESCAN_MASK) )
			DPRINTF(E_DEBUG, L_INOTIFY, "%s already exists\n", path);
		return 0;
	}

	/* Find the parentID. If it's not found, create all necessary parents. */
	len = strlen(path)+1;
	if( !(path_buf = malloc(len)) ||
	    !(last_dir = malloc(len)) ||
	    !(base_name = malloc(len)) )
		return -1;
	base_copy = base_name;
	while( depth )
	{
		depth = 0;
		strcpy(path_buf, path);
		parent_buf = dirname(path_buf);

		do
		{
			//DEBUG DPRINTF(E_DEBUG, L_INOTIFY, "Checking %s\n", parent_buf);
			id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
			                            " where d.PATH = '%q' and REF_ID is NULL", parent_buf);

			if( id )
			{
				dir_password = sql_get_text_field(db, "select PASSWORD from OBJECTS where OBJECT_ID='%s'", id);
				if ( dir_password != NULL && dir_password[0] == '\0' )
					dir_password = NULL;
			}
			else // Only relevant for root directory
			{
				snprintf(password_path, PATH_MAX, "%s/"PASSWORD_FILE, parent_buf);
				if( access(password_path, 0) == 0 )
				{
					readPassword(password_path, password_buf, PASSWORD_ARRAY_LEN);
					if ( password_buf[0] != '\0' )
						dir_password = password_buf;
				}
			}

			// Save first directory password seen as file password
			if ( dir_password != NULL && file_password[0] == '\0' )
				strcpy(file_password, dir_password);

			if( id )
			{
				if( !depth )
					break;
				DPRINTF(E_DEBUG, L_INOTIFY, "Found first known parentID: %s [%s]\n", id, parent_buf);
				/* Insert newly-found directory */
				strcpy(base_name, last_dir);
				base_copy = basename(base_name);
				insert_directory(base_copy, last_dir, BROWSEDIR_ID, id+2, get_next_available_id("OBJECTS", id), dir_password);
				sqlite3_free(id);
				break;
			}
			depth++;
			strcpy(last_dir, parent_buf);
			parent_buf = dirname(parent_buf);
		}
		while( strcmp(parent_buf, "/") != 0 );

		if( strcmp(parent_buf, "/") == 0 )
		{
			id = sqlite3_mprintf("%s", BROWSEDIR_ID);
			depth = 0;
			break;
		}
		strcpy(path_buf, path);
	}
	free(last_dir);
	free(path_buf);
	free(base_name);

	if( !depth )
	{
		//DEBUG DPRINTF(E_DEBUG, L_INOTIFY, "Inserting %s\n", name);
		int ret = insert_file(name, path, id+2, get_next_available_id("OBJECTS", id), dir_types, file_password[0] == '\0' ? NULL : file_password);
		if (ret == 1 && (mtype & TYPE_PLAYLIST))
		{
			next_pl_fill = time(NULL) + 120; // Schedule a playlist scan for 2 minutes from now.
			//DEBUG DPRINTF(E_MAXDEBUG, L_INOTIFY,  "Playlist scan scheduled for %s", ctime(&next_pl_fill));
		}
		sqlite3_free(id);
	}
	return depth;
}

static bool
check_notsparse(const char *path)
#if HAVE_DECL_SEEK_HOLE
{
	int fd;
	bool rv;

	if ((fd = open(path, O_RDONLY)) == -1)
		return (false);
	if (lseek(fd, 0, SEEK_HOLE) == lseek(fd, 0, SEEK_END))
		rv = true;
	else
		rv = false;
	close(fd);
	return (rv);
}
#else
{
	struct stat st;

	return (stat(path, &st) == 0 && (st.st_blocks << 9 >= st.st_size));
}
#endif

int
monitor_insert_directory(int fd, char *name, const char * path)
{
	DIR * ds;
	struct dirent * e;
	char *id, *parent_buf, *esc_name;
	char path_buf[PATH_MAX];
	enum file_types type = TYPE_UNKNOWN;
	media_types dir_types;

	if( access(path, R_OK|X_OK) != 0 )
	{
		DPRINTF(E_WARN, L_INOTIFY, "Could not access %s [%s]\n", path, strerror(errno));
		return -1;
	}
	if( sql_get_int_field(db, "SELECT ID from DETAILS where PATH = '%q'", path) > 0 )
	{
		fd = 0;
		if (!GETFLAG(RESCAN_MASK))
			DPRINTF(E_DEBUG, L_INOTIFY, "%s already exists\n", path);
	}
	else
	{
		parent_buf = strdup(path);
		id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
					    " WHERE d.PATH = '%q' and REF_ID is NULL", dirname(parent_buf));
		if( !id )
			id = sqlite3_mprintf("%s", BROWSEDIR_ID);

	    // Since we are adding a new directory, the password would be blank...
		insert_directory(name, path, BROWSEDIR_ID, id+2, get_next_available_id("OBJECTS", id), "");
		sqlite3_free(id);
		free(parent_buf);
	}

#ifdef HAVE_WATCH
	if( fd > 0 )
		monitor_add_watch(fd, path);
#endif

	dir_types = valid_media_types(path);

	ds = opendir(path);
	if( !ds )
	{
		DPRINTF(E_ERROR, L_INOTIFY, "opendir failed! [%s]\n", strerror(errno));
		return -1;
	}
	while( !quitting && (e = readdir(ds)) )
	{
		if( e->d_name[0] == '.' )
			continue;
		esc_name = escape_tag(e->d_name, 1);
		snprintf(path_buf, sizeof(path_buf), "%s/%s", path, e->d_name);
		switch( e->d_type )
		{
			case DT_DIR:
			case DT_REG:
			case DT_LNK:
			case DT_UNKNOWN:
				type = resolve_unknown_type(path_buf, dir_types);
			default:
				break;
		}
		if( type == TYPE_DIR )
		{
			monitor_insert_directory(fd, esc_name, path_buf);
		}
		else if( type == TYPE_FILE && check_notsparse(path_buf)) {
			monitor_insert_file(esc_name, path_buf);
		}
		free(esc_name);
	}
	closedir(ds);

	return 0;
}

int
monitor_remove_tree(const char * path)
{
	char * sql;
	char **result;
	int64_t detailID = 0;
	int rows, i, ret = 1;

	sql = sqlite3_mprintf("SELECT ID from DETAILS where (PATH > '%q/' and PATH <= '%q/%c')"
	                      " or PATH = '%q'", path, path, 0xFF, path);
	if( (sql_get_table(db, sql, &result, &rows, NULL) == SQLITE_OK) )
	{
		if( rows )
		{
			for( i=1; i <= rows; i++ )
			{
				detailID = strtoll(result[i], NULL, 10);
				sql_exec(db, "DELETE from DETAILS where ID = %lld", detailID);
				sql_exec(db, "DELETE from OBJECTS where DETAIL_ID = %lld", detailID);
			}
			ret = 0;
		}
		sqlite3_free_table(result);
	}
	sqlite3_free(sql);
	/* Clean up any album art entries in the deleted directory */
	sql_exec(db, "DELETE from ALBUM_ART where (PATH > '%q/' and PATH <= '%q/%c')", path, path, 0xFF);

	return ret;
}

int
monitor_remove_directory(int fd, const char * path)
{
	/* Invalidate the scanner cache so we don't insert files into non-existent containers */
	valid_cache = 0;
#ifdef HAVE_WATCH
	if( fd > 0 )
	{
		monitor_remove_watch(fd, path);
	}
#endif
    return monitor_remove_tree(path);
}
