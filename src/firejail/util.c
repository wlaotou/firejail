/*
 * Copyright (C) 2014-2018 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#define _XOPEN_SOURCE 500
#include "firejail.h"
#include <ftw.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <syslog.h>
#include <errno.h>
#include <dirent.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_GROUPS 1024
#define MAXBUF 4098



// send the error to /var/log/auth.log and exit after a small delay
void errLogExit(char* fmt, ...) {
	va_list args;
	va_start(args,fmt);
	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_AUTH);
	MountData *m = get_last_mount();

	char *msg1;
	char *msg2  = "Access error";
	if (vasprintf(&msg1, fmt, args) != -1 &&
	    asprintf(&msg2, "Access error: pid %d, last mount name:%s dir:%s type:%s - %s", getuid(), m->fsname, m->dir, m->fstype, msg1) != -1)
		syslog(LOG_CRIT, "%s", msg2);
	va_end(args);
	closelog();

	sleep(2);
	fprintf(stderr, "%s\n", msg2);
	exit(1);
}

static void clean_supplementary_groups(gid_t gid) {
	assert(cfg.username);
	gid_t groups[MAX_GROUPS];
	int ngroups = MAX_GROUPS;
	int rv = getgrouplist(cfg.username, gid, groups, &ngroups);
	if (rv == -1)
		goto clean_all;

	// clean supplementary group list
	// allow only tty, audio, video, games
	gid_t new_groups[MAX_GROUPS];
	int new_ngroups = 0;
	char *allowed[] = {
		"tty",
		"audio",
		"video",
		"games",
		NULL
	};

	int i = 0;
	while (allowed[i]) {
		gid_t g = get_group_id(allowed[i]);
	 	if (g) {
			int j;
			for (j = 0; j < ngroups; j++) {
				if (g == groups[j]) {
					new_groups[new_ngroups] = g;
					new_ngroups++;
					break;
				}
			}
		}
		i++;
	}

	if (new_ngroups) {
		rv = setgroups(new_ngroups, new_groups);
		if (rv)
			goto clean_all;

		if (arg_debug) {
			printf("Supplementary groups: ");
			for (i = 0; i < new_ngroups; i++)
				printf("%d ", new_groups[i]);
			printf("\n");
		}
	}
	else
		goto clean_all;

	return;

clean_all:
	fwarning("cleaning all supplementary groups\n");
	if (setgroups(0, NULL) < 0)
		errExit("setgroups");
}


// drop privileges
// - for root group or if nogroups is set, supplementary groups are not configured
void drop_privs(int nogroups) {
	EUID_ROOT();
	gid_t gid = getgid();
	if (arg_debug)
		printf("Drop privileges: pid %d, uid %d, gid %d, nogroups %d\n",  getpid(), getuid(), gid, nogroups);

	// configure supplementary groups
	if (gid == 0 || nogroups) {
		if (setgroups(0, NULL) < 0)
			errExit("setgroups");
		if (arg_debug)
			printf("No supplementary groups\n");
	}
	else if (arg_noroot)
		clean_supplementary_groups(gid);

	// set uid/gid
	if (setgid(getgid()) < 0)
		errExit("setgid/getgid");
	if (setuid(getuid()) < 0)
		errExit("setuid/getuid");
}


int mkpath_as_root(const char* path) {
	assert(path && *path);

	// work on a copy of the path
	char *file_path = strdup(path);
	if (!file_path)
		errExit("strdup");

	char* p;
	int done = 0;
	for (p=strchr(file_path+1, '/'); p; p=strchr(p+1, '/')) {
		*p='\0';
		if (mkdir(file_path, 0755)==-1) {
			if (errno != EEXIST) {
				*p='/';
				free(file_path);
				return -1;
			}
		}
		else {
			if (set_perms(file_path, 0, 0, 0755))
				errExit("set_perms");
			done = 1;
		}

		*p='/';
	}
	if (done)
		fs_logger2("mkpath", path);

	free(file_path);
	return 0;
}

void fwarning(char* fmt, ...) {
	if (arg_quiet)
		return;

	va_list args;
	va_start(args,fmt);
	fprintf(stderr, "Warning: ");
	vfprintf(stderr, fmt, args);
	va_end(args);
}

void fmessage(char* fmt, ...) { // TODO: this function is duplicated in src/fnet/interface.c
	if (arg_quiet)
		return;

	va_list args;
	va_start(args,fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(0);
}

void logsignal(int s) {
	if (!arg_debug)
		return;

	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_INFO, "Signal %d caught", s);
	closelog();
}


void logmsg(const char *msg) {
	if (!arg_debug)
		return;

	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_INFO, "%s\n", msg);
	closelog();
}


void logargs(int argc, char **argv) {
	if (!arg_debug)
		return;

	int i;
	int len = 0;

	// calculate message length
	for (i = 0; i < argc; i++)
		len += strlen(argv[i]) + 1;	  // + ' '

	// build message
	char msg[len + 1];
	char *ptr = msg;
	for (i = 0; i < argc; i++) {
		sprintf(ptr, "%s ", argv[i]);
		ptr += strlen(ptr);
	}

	// log message
	logmsg(msg);
}


void logerr(const char *msg) {
	if (!arg_debug)
		return;

	openlog("firejail", LOG_NDELAY | LOG_PID, LOG_USER);
	syslog(LOG_ERR, "%s\n", msg);
	closelog();
}

static int copy_file_by_fd(int src, int dst) {
	assert(src >= 0);
	assert(dst >= 0);

	ssize_t len;
	static const int BUFLEN = 1024;
	unsigned char buf[BUFLEN];
	while ((len = read(src, buf, BUFLEN)) > 0) {
		int done = 0;
		while (done != len) {
			int rv = write(dst, buf + done, len - done);
			if (rv == -1)
				return -1;
			done += rv;
		}
	}
//	fflush(0);
	return 0;
}

// return -1 if error, 0 if no error; if destname already exists, return error
int copy_file(const char *srcname, const char *destname, uid_t uid, gid_t gid, mode_t mode) {
	assert(srcname);
	assert(destname);

	// open source
	int src = open(srcname, O_RDONLY);
	if (src < 0) {
		fwarning("cannot open source file %s, file not copied\n", srcname);
		return -1;
	}

	// open destination
	int dst = open(destname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (dst < 0) {
		fwarning("cannot open destination file %s, file not copied\n", destname);
		close(src);
		return -1;
	}

	int errors = copy_file_by_fd(src, dst);
	if (!errors) {
		if (fchown(dst, uid, gid) == -1)
			errExit("fchown");
		if (fchmod(dst, mode) == -1)
			errExit("fchmod");
	}
	close(src);
	close(dst);
	return errors;
}

// return -1 if error, 0 if no error
void copy_file_as_user(const char *srcname, const char *destname, uid_t uid, gid_t gid, mode_t mode) {
	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// drop privileges
		drop_privs(0);

		// copy, set permissions and ownership
		int rv = copy_file(srcname, destname, uid, gid, mode); // already a regular user
		if (rv)
			fwarning("cannot copy %s\n", srcname);
#ifdef HAVE_GCOV
		__gcov_flush();
#endif
		_exit(0);
	}
	// wait for the child to finish
	waitpid(child, NULL, 0);
}

void copy_file_from_user_to_root(const char *srcname, const char *destname, uid_t uid, gid_t gid, mode_t mode) {
	// open destination
	int dst = open(destname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (dst < 0) {
		fwarning("cannot open destination file %s, file not copied\n", destname);
		return;
	}

	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// drop privileges
		drop_privs(0);

		int src = open(srcname, O_RDONLY);
		if (src < 0) {
			fwarning("cannot open source file %s, file not copied\n", srcname);
		} else {
			if (copy_file_by_fd(src, dst)) {
				fwarning("cannot copy %s\n", srcname);
			}
			close(src);
		}
		close(dst);
#ifdef HAVE_GCOV
		__gcov_flush();
#endif
		_exit(0);
	}
	// wait for the child to finish
	waitpid(child, NULL, 0);
	if (fchown(dst, uid, gid) == -1)
		errExit("fchown");
	if (fchmod(dst, mode) == -1)
		errExit("fchmod");
	close(dst);
}

// return -1 if error, 0 if no error
void touch_file_as_user(const char *fname, uid_t uid, gid_t gid, mode_t mode) {
	pid_t child = fork();
	if (child < 0)
		errExit("fork");
	if (child == 0) {
		// drop privileges
		drop_privs(0);

		FILE *fp = fopen(fname, "w");
		if (fp) {
			fprintf(fp, "\n");
			SET_PERMS_STREAM(fp, uid, gid, mode);
			fclose(fp);
		}
#ifdef HAVE_GCOV
		__gcov_flush();
#endif
		_exit(0);
	}
	// wait for the child to finish
	waitpid(child, NULL, 0);
}

// return 1 if the file is a directory
int is_dir(const char *fname) {
	assert(fname);
	if (*fname == '\0')
		return 0;

	// if fname doesn't end in '/', add one
	int rv;
	struct stat s;
	if (fname[strlen(fname) - 1] == '/')
		rv = stat(fname, &s);
	else {
		char *tmp;
		if (asprintf(&tmp, "%s/", fname) == -1) {
			fprintf(stderr, "Error: cannot allocate memory, %s:%d\n", __FILE__, __LINE__);
			errExit("asprintf");
		}
		rv = stat(tmp, &s);
		free(tmp);
	}

	if (rv == -1)
		return 0;

	if (S_ISDIR(s.st_mode))
		return 1;

	return 0;
}


// return 1 if the file is a link
int is_link(const char *fname) {
	assert(fname);
	if (*fname == '\0')
		return 0;

	char *dup = NULL;
	struct stat s;
	if (lstat(fname, &s) == 0) {
		if (S_ISLNK(s.st_mode))
			return 1;
		if (S_ISDIR(s.st_mode)) {
			// remove trailing slashes and single dots and try again
			dup = strdup(fname);
			if (!dup)
				errExit("strdup");
			trim_trailing_slash_or_dot(dup);
			if (lstat(dup, &s) == 0) {
				if (S_ISLNK(s.st_mode)) {
					free(dup);
					return 1;
				}
			}
		}
	}

	free(dup);
	return 0;
}

// remove all slashes and single dots from the end of a path
// for example /foo/bar///././. -> /foo/bar
void trim_trailing_slash_or_dot(char *path) {
	assert(path);

	char *end = strchr(path, '\0');
	assert(end);
	if ((end - path) > 1) {
		end--;
		while (*end == '/' ||
		      (*end == '.' && *(end - 1) == '/')) {
			*end = '\0';
			end--;
			if (end == path)
				break;
		}
	}
}

// remove multiple spaces and return allocated memory
char *line_remove_spaces(const char *buf) {
	EUID_ASSERT();
	assert(buf);
	if (strlen(buf) == 0)
		return NULL;

	// allocate memory for the new string
	char *rv = malloc(strlen(buf) + 1);
	if (rv == NULL)
		errExit("malloc");

	// remove space at start of line
	const char *ptr1 = buf;
	while (*ptr1 == ' ' || *ptr1 == '\t')
		ptr1++;

	// copy data and remove additional spaces
	char *ptr2 = rv;
	int state = 0;
	while (*ptr1 != '\0') {
		if (*ptr1 == '\n' || *ptr1 == '\r')
			break;

		if (state == 0) {
			if (*ptr1 != ' ' && *ptr1 != '\t')
				*ptr2++ = *ptr1++;
			else {
				*ptr2++ = ' ';
				ptr1++;
				state = 1;
			}
		}
		else {				  // state == 1
			while (*ptr1 == ' ' || *ptr1 == '\t')
				ptr1++;
			state = 0;
		}
	}

	// strip last blank character if any
	if (ptr2 > rv && *(ptr2 - 1) == ' ')
		--ptr2;
	*ptr2 = '\0';
	//	if (arg_debug)
	//		printf("Processing line #%s#\n", rv);

	return rv;
}


char *split_comma(char *str) {
	EUID_ASSERT();
	if (str == NULL || *str == '\0')
		return NULL;
	char *ptr = strchr(str, ',');
	if (!ptr)
		return NULL;
	*ptr = '\0';
	ptr++;
	if (*ptr == '\0')
		return NULL;
	return ptr;
}


void check_unsigned(const char *str, const char *msg) {
	EUID_ASSERT();
	const char *ptr = str;
	while (*ptr != ' ' && *ptr != '\t' && *ptr != '\0') {
		if (!isdigit(*ptr)) {
			fprintf(stderr, "%s %s\n", msg, str);
			exit(1);
		}
		ptr++;
	}
}


#define BUFLEN 4096
// find the first child for this parent; return 1 if error
int find_child(pid_t parent, pid_t *child) {
	EUID_ASSERT();
	*child = 0;				  // use it to flag a found child

	DIR *dir;
	EUID_ROOT();				  // grsecurity fix
	if (!(dir = opendir("/proc"))) {
		// sleep 2 seconds and try again
		sleep(2);
		if (!(dir = opendir("/proc"))) {
			fprintf(stderr, "Error: cannot open /proc directory\n");
			exit(1);
		}
	}

	struct dirent *entry;
	char *end;
	while (*child == 0 && (entry = readdir(dir))) {
		pid_t pid = strtol(entry->d_name, &end, 10);
		if (end == entry->d_name || *end)
			continue;
		if (pid == parent)
			continue;

		// open stat file
		char *file;
		if (asprintf(&file, "/proc/%u/status", pid) == -1) {
			perror("asprintf");
			exit(1);
		}
		FILE *fp = fopen(file, "r");
		if (!fp) {
			free(file);
			continue;
		}

		// look for firejail executable name
		char buf[BUFLEN];
		while (fgets(buf, BUFLEN - 1, fp)) {
			if (strncmp(buf, "PPid:", 5) == 0) {
				char *ptr = buf + 5;
				while (*ptr != '\0' && (*ptr == ' ' || *ptr == '\t')) {
					ptr++;
				}
				if (*ptr == '\0') {
					fprintf(stderr, "Error: cannot read /proc file\n");
					exit(1);
				}
				if (parent == atoi(ptr))
					*child = pid;
				break;		  // stop reading the file
			}
		}
		fclose(fp);
		free(file);
	}
	closedir(dir);
	EUID_USER();
	return (*child)? 0:1;			  // 0 = found, 1 = not found
}


void extract_command_name(int index, char **argv) {
	EUID_ASSERT();
	assert(argv);
	assert(argv[index]);

	// configure command index
	cfg.original_program_index = index;

	char *str = strdup(argv[index]);
	if (!str)
		errExit("strdup");

	// if we have a symbolic link, use the real path to extract the name
//	if (is_link(argv[index])) {
//		char*newname = realpath(argv[index], NULL);
//		if (newname) {
//			free(str);
//			str = newname;
//		}
//	}

	// configure command name
	cfg.command_name = str;
	if (!cfg.command_name)
		errExit("strdup");

	// restrict the command name to the first word
	char *ptr = cfg.command_name;
	while (*ptr != ' ' && *ptr != '\t' && *ptr != '\0')
		ptr++;
	*ptr = '\0';

	// remove the path: /usr/bin/firefox becomes firefox
	ptr = strrchr(cfg.command_name, '/');
	if (ptr) {
		ptr++;
		if (*ptr == '\0') {
			fprintf(stderr, "Error: invalid command name\n");
			exit(1);
		}

		char *tmp = strdup(ptr);
		if (!tmp)
			errExit("strdup");

		// limit the command to the first ' '
		char *ptr2 = tmp;
		while (*ptr2 != ' ' && *ptr2 != '\0')
			ptr2++;
		*ptr2 = '\0';

		free(cfg.command_name);
		cfg.command_name = tmp;
	}
}


void update_map(char *mapping, char *map_file) {
	int fd;
	size_t j;
	size_t map_len;				  /* Length of 'mapping' */

	/* Replace commas in mapping string with newlines */

	map_len = strlen(mapping);
	for (j = 0; j < map_len; j++)
		if (mapping[j] == ',')
			mapping[j] = '\n';

	fd = open(map_file, O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "Error: cannot open %s: %s\n", map_file, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (write(fd, mapping, map_len) != (ssize_t)map_len) {
		fprintf(stderr, "Error: cannot write to %s: %s\n", map_file, strerror(errno));
		exit(EXIT_FAILURE);
	}

	close(fd);
}


void wait_for_other(int fd) {
	//****************************
	// wait for the parent to be initialized
	//****************************
	char childstr[BUFLEN + 1];
	int newfd = dup(fd);
	if (newfd == -1)
		errExit("dup");
	FILE* stream;
	stream = fdopen(newfd, "r");
	*childstr = '\0';
	if (fgets(childstr, BUFLEN, stream)) {
		// remove \n)
		char *ptr = childstr;
		while(*ptr !='\0' && *ptr != '\n')
			ptr++;
		if (*ptr == '\0')
			errExit("fgets");
		*ptr = '\0';
	}
	else {
		fprintf(stderr, "Error: proc %d cannot sync with peer: %s\n",
			getpid(), ferror(stream) ? strerror(errno) : "unexpected EOF");

		int status = 0;
		pid_t pid = wait(&status);
		if (pid != -1) {
			if (WIFEXITED(status))
				fprintf(stderr, "Peer %d unexpectedly exited with status %d\n",
					pid, WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				fprintf(stderr, "Peer %d unexpectedly killed (%s)\n",
					pid, strsignal(WTERMSIG(status)));
			else
				fprintf(stderr, "Peer %d unexpectedly exited "
					"(un-decodable wait status %04x)\n", pid, status);
		}
		exit(1);
	}

	if (strcmp(childstr, "arg_noroot=0") == 0)
		arg_noroot = 0;
	else if (strcmp(childstr, "arg_noroot=1") == 0)
		arg_noroot = 1;
	else {
		fprintf(stderr, "Error: unexpected message from peer: %s\n", childstr);
		exit(1);
	}

	fclose(stream);
}

void notify_other(int fd) {
	FILE* stream;
	int newfd = dup(fd);
	if (newfd == -1)
		errExit("dup");
	stream = fdopen(newfd, "w");
	fprintf(stream, "arg_noroot=%d\n", arg_noroot);
	fflush(stream);
	fclose(stream);
}




// Equivalent to the GNU version of basename, which is incompatible with
// the POSIX basename. A few lines of code saves any portability pain.
// https://www.gnu.org/software/libc/manual/html_node/Finding-Tokens-in-a-String.html#index-basename
const char *gnu_basename(const char *path) {
	const char *last_slash = strrchr(path, '/');
	if (!last_slash)
		return path;
	return last_slash+1;
}


uid_t pid_get_uid(pid_t pid) {
	EUID_ASSERT();
	uid_t rv = 0;

	// open status file
	char *file;
	if (asprintf(&file, "/proc/%u/status", pid) == -1) {
		perror("asprintf");
		exit(1);
	}
	EUID_ROOT();				  // grsecurity fix
	FILE *fp = fopen(file, "r");
	if (!fp) {
		free(file);
		fprintf(stderr, "Error: cannot open /proc file\n");
		exit(1);
	}

	// extract uid
	static const int PIDS_BUFLEN = 1024;
	char buf[PIDS_BUFLEN];
	while (fgets(buf, PIDS_BUFLEN - 1, fp)) {
		if (strncmp(buf, "Uid:", 4) == 0) {
			char *ptr = buf + 5;
			while (*ptr != '\0' && (*ptr == ' ' || *ptr == '\t')) {
				ptr++;
			}
			if (*ptr == '\0')
				break;

			rv = atoi(ptr);
			break;			  // break regardless!
		}
	}

	fclose(fp);
	free(file);
	EUID_USER();				  // grsecurity fix

	if (rv == 0) {
		fprintf(stderr, "Error: cannot read /proc file\n");
		exit(1);
	}
	return rv;
}




uid_t get_group_id(const char *group) {
	// find tty group id
	gid_t gid = 0;
	struct group *g = getgrnam(group);
	if (g)
		gid = g->gr_gid;

	return gid;
}

static int len_homedir = 0;
static int remove_callback(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void) sb;
	(void) typeflag;
	(void) ftwbuf;
	assert(fpath);

	if (len_homedir == 0)
		len_homedir = strlen(cfg.homedir);

	char *rp = realpath(fpath, NULL);	// this should never fail!
	if (!rp)
		return 1;
	if (strncmp(rp, cfg.homedir, len_homedir) != 0)
		return 1;
	free(rp);

	if (remove(fpath)) {	// removes the link not the actual file
		fprintf(stderr, "Error: cannot remove file %s\n", fpath);
		exit(1);
	}

	return 0;
}


int remove_overlay_directory(void) {
	sleep(1);

	char *path;
	if (asprintf(&path, "%s/.firejail", cfg.homedir) == -1)
		errExit("asprintf");

	// deal with obvious problems such as symlinks and root ownership
	if (is_link(path))
		errLogExit("overlay directory is a symlink\n");
	if (access(path, R_OK | W_OK | X_OK) == -1)
		errLogExit("no access to overlay directory\n");

	EUID_ROOT();
	if (setreuid(0, 0) < 0 ||
	    setregid(0, 0) < 0)
		errExit("setreuid/setregid");
	errno = 0;

	// FTW_PHYS - do not follow symbolic links
	return nftw(path, remove_callback, 64, FTW_DEPTH | FTW_PHYS);
}

void flush_stdin(void) {
	if (isatty(STDIN_FILENO)) {
		int cnt = 0;
		int rv = ioctl(STDIN_FILENO, FIONREAD, &cnt);
		if (rv == 0 && cnt) {
			fwarning("removing %d bytes from stdin\n", cnt);
			rv = ioctl(STDIN_FILENO, TCFLSH, TCIFLUSH);
			(void) rv;
		}
	}
}

void create_empty_dir_as_root(const char *dir, mode_t mode) {
	assert(dir);
	mode &= 07777;
	struct stat s;

	if (stat(dir, &s)) {
		if (arg_debug)
			printf("Creating empty %s directory\n", dir);
		/* coverity[toctou] */
		// don't fail if directory already exists. This can be the case in a race
		// condition, when two jails launch at the same time. See #1013
		if (mkdir(dir, mode) == -1 && errno != EEXIST)
			errExit("mkdir");
		if (set_perms(dir, 0, 0, mode))
			errExit("set_perms");
		ASSERT_PERMS(dir, 0, 0, mode);
	}
}

void create_empty_file_as_root(const char *fname, mode_t mode) {
	assert(fname);
	mode &= 07777;
	struct stat s;

	if (stat(fname, &s)) {
		if (arg_debug)
			printf("Creating empty %s file\n", fname);

		/* coverity[toctou] */
		FILE *fp = fopen(fname, "w");
		if (!fp)
			errExit("fopen");
		SET_PERMS_STREAM(fp, 0, 0, S_IRUSR);
		fclose(fp);
		if (chmod(fname, mode) == -1)
			errExit("chmod");
	}
}

// return 1 if error
int set_perms(const char *fname, uid_t uid, gid_t gid, mode_t mode) {
	assert(fname);
	if (chmod(fname, mode) == -1)
		return 1;
	if (chown(fname, uid, gid) == -1)
		return 1;
	return 0;
}

void mkdir_attr(const char *fname, mode_t mode, uid_t uid, gid_t gid) {
	assert(fname);
	mode &= 07777;
#if 0
	printf("fname %s, uid %d, gid %d, mode %x - ", fname, uid, gid, (unsigned) mode);
	if (S_ISLNK(mode))
		printf("l");
	else if (S_ISDIR(mode))
		printf("d");
	else if (S_ISCHR(mode))
		printf("c");
	else if (S_ISBLK(mode))
		printf("b");
	else if (S_ISSOCK(mode))
		printf("s");
	else
		printf("-");
	printf( (mode & S_IRUSR) ? "r" : "-");
	printf( (mode & S_IWUSR) ? "w" : "-");
	printf( (mode & S_IXUSR) ? "x" : "-");
	printf( (mode & S_IRGRP) ? "r" : "-");
	printf( (mode & S_IWGRP) ? "w" : "-");
	printf( (mode & S_IXGRP) ? "x" : "-");
	printf( (mode & S_IROTH) ? "r" : "-");
	printf( (mode & S_IWOTH) ? "w" : "-");
	printf( (mode & S_IXOTH) ? "x" : "-");
	printf("\n");
#endif
	if (mkdir(fname, mode) == -1 ||
	    chmod(fname, mode) == -1 ||
	    chown(fname, uid, gid)) {
	    	fprintf(stderr, "Error: failed to create %s directory\n", fname);
		errExit("mkdir/chmod");
	}

	ASSERT_PERMS(fname, uid, gid, mode);
}

unsigned extract_timeout(const char *str) {
	unsigned s;
	unsigned m;
	unsigned h;
	int rv = sscanf(str, "%02u:%02u:%02u", &h, &m, &s);
	if (rv != 3) {
		fprintf(stderr, "Error: invalid timeout, please use a hh:mm:ss format\n");
		exit(1);
	}

	return h * 3600 + m * 60 + s;
}

void disable_file_or_dir(const char *fname) {
	if (arg_debug)
		printf("blacklist %s\n", fname);
	struct stat s;
	if (stat(fname, &s) != -1) {
		if (is_dir(fname)) {
			if (mount(RUN_RO_DIR, fname, "none", MS_BIND, "mode=400,gid=0") < 0)
				errExit("disable directory");
		}
		else {
			if (mount(RUN_RO_FILE, fname, "none", MS_BIND, "mode=400,gid=0") < 0)
				errExit("disable file");
		}
	}
	fs_logger2("blacklist", fname);
}

void disable_file_path(const char *path, const char *file) {
	assert(file);
	assert(path);

	char *fname;
	if (asprintf(&fname, "%s/%s", path, file) == -1)
		errExit("asprintf");

	disable_file_or_dir(fname);
	free(fname);
}

// The returned file descriptor should be suitable for privileged operations on
// user controlled paths. Passed flags are ignored if path is a top level directory.
int safe_fd(const char *path, int flags) {
	assert(path);

	// reject empty string, relative path
	if (*path != '/')
		goto errexit;
	// reject ".."
	if (strstr(path, ".."))
		goto errexit;

	// work with a copy of path
	char *dup = strdup(path);
	if (dup == NULL)
		errExit("strdup");

	char *p = strrchr(dup, '/');
	assert(p);
	// reject trailing slash, root directory
	if (*(p + 1) == '\0')
		goto errexit;
	// reject trailing dot
	if (*(p + 1) == '.' && *(p + 2) == '\0')
		goto errexit;
	// if there is more than one path segment, keep the last one for later
	if (p != dup)
		*p = '\0';

	int parentfd = open("/", O_PATH|O_DIRECTORY|O_CLOEXEC);
	if (parentfd == -1)
		errExit("open");

	// traverse the path and return -1 if a symlink is encountered
	int entered = 0;
	int fd = -1;
	char *tok = strtok(dup, "/");
	while (tok) {
		// skip all "/./"
		if (strcmp(tok, ".") == 0) {
			tok = strtok(NULL, "/");
			continue;
		}
		entered = 1;

		// open the directory
		fd = openat(parentfd, tok, O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
		close(parentfd);
		if (fd == -1) {
			free(dup);
			return -1;
		}

		parentfd = fd;
		tok = strtok(NULL, "/");
	}
	if (p != dup) {
		// consistent flags for top level directories (////foo, /.///foo)
		if (!entered)
			flags = O_PATH|O_DIRECTORY|O_CLOEXEC;
		// open last path segment
		fd = openat(parentfd, p + 1, flags|O_NOFOLLOW);
		close(parentfd);
	}
	free(dup);
	return fd; // -1 if open failed

errexit:
	fprintf(stderr, "Error: cannot open \"%s\", invalid filename\n", path);
	exit(1);
}
